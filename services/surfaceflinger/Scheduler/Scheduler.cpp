/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#undef LOG_TAG
#define LOG_TAG "Scheduler"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "Scheduler.h"

#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android/hardware/configstore/1.0/ISurfaceFlingerConfigs.h>
#include <android/hardware/configstore/1.1/ISurfaceFlingerConfigs.h>
#include <configstore/Utils.h>
#include <ftl/fake_guard.h>
#include <gui/WindowInfo.h>
#include <system/window.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include <FrameTimeline/FrameTimeline.h>
#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>

#include "../Layer.h"
#include "DispSyncSource.h"
#include "EventThread.h"
#include "FrameRateOverrideMappings.h"
#include "InjectVSyncSource.h"
#include "OneShotTimer.h"
#include "SurfaceFlingerProperties.h"
#include "VSyncPredictor.h"
#include "VSyncReactor.h"

#define RETURN_IF_INVALID_HANDLE(handle, ...)                        \
    do {                                                             \
        if (mConnections.count(handle) == 0) {                       \
            ALOGE("Invalid connection handle %" PRIuPTR, handle.id); \
            return __VA_ARGS__;                                      \
        }                                                            \
    } while (false)

namespace {

using android::Fps;
using android::FpsApproxEqual;
using android::FpsHash;
using android::scheduler::AggregatedFpsScore;
using android::scheduler::RefreshRateRankingsAndSignals;

// Returns the aggregated score per Fps for the RefreshRateRankingsAndSignals sourced.
auto getAggregatedScoresPerFps(
        const std::vector<RefreshRateRankingsAndSignals>& refreshRateRankingsAndSignalsPerDisplay)
        -> std::unordered_map<Fps, AggregatedFpsScore, FpsHash, FpsApproxEqual> {
    std::unordered_map<Fps, AggregatedFpsScore, FpsHash, FpsApproxEqual> aggregatedScoresPerFps;

    for (const auto& refreshRateRankingsAndSignal : refreshRateRankingsAndSignalsPerDisplay) {
        const auto& refreshRateRankings = refreshRateRankingsAndSignal.refreshRateRankings;

        std::for_each(refreshRateRankings.begin(), refreshRateRankings.end(), [&](const auto& it) {
            const auto [score, result] =
                    aggregatedScoresPerFps.try_emplace(it.displayModePtr->getFps(),
                                                       AggregatedFpsScore{it.score,
                                                                          /* numDisplays */ 1});
            if (!result) { // update
                score->second.totalScore += it.score;
                score->second.numDisplays++;
            }
        });
    }
    return aggregatedScoresPerFps;
}

} // namespace

namespace android::scheduler {

Scheduler::Scheduler(ICompositor& compositor, ISchedulerCallback& callback, FeatureFlags features)
      : impl::MessageQueue(compositor), mFeatures(features), mSchedulerCallback(callback) {}

Scheduler::~Scheduler() {
    // Stop timers and wait for their threads to exit.
    mDisplayPowerTimer.reset();
    mTouchTimer.reset();

    // Stop idle timer and clear callbacks, as the RefreshRateConfigs may outlive the Scheduler.
    setRefreshRateConfigs(nullptr);
}

void Scheduler::startTimers() {
    using namespace sysprop;
    using namespace std::string_literals;

    if (const int64_t millis = set_touch_timer_ms(0); millis > 0) {
        // Touch events are coming to SF every 100ms, so the timer needs to be higher than that
        mTouchTimer.emplace(
                "TouchTimer", std::chrono::milliseconds(millis),
                [this] { touchTimerCallback(TimerState::Reset); },
                [this] { touchTimerCallback(TimerState::Expired); });
        mTouchTimer->start();
    }

    if (const int64_t millis = set_display_power_timer_ms(0); millis > 0) {
        mDisplayPowerTimer.emplace(
                "DisplayPowerTimer", std::chrono::milliseconds(millis),
                [this] { displayPowerTimerCallback(TimerState::Reset); },
                [this] { displayPowerTimerCallback(TimerState::Expired); });
        mDisplayPowerTimer->start();
    }
}

void Scheduler::setRefreshRateConfigs(std::shared_ptr<RefreshRateConfigs> configs) {
    // The current RefreshRateConfigs instance may outlive this call, so unbind its idle timer.
    {
        // mRefreshRateConfigsLock is not locked here to avoid the deadlock
        // as the callback can attempt to acquire the lock before stopIdleTimer can finish
        // the execution. It's safe to FakeGuard as main thread is the only thread that
        // writes to the mRefreshRateConfigs.
        ftl::FakeGuard guard(mRefreshRateConfigsLock);
        if (mRefreshRateConfigs) {
            mRefreshRateConfigs->stopIdleTimer();
            mRefreshRateConfigs->clearIdleTimerCallbacks();
        }
    }
    {
        // Clear state that depends on the current instance.
        std::scoped_lock lock(mPolicyLock);
        mPolicy = {};
    }

    std::scoped_lock lock(mRefreshRateConfigsLock);
    mRefreshRateConfigs = std::move(configs);
    if (!mRefreshRateConfigs) return;

    mRefreshRateConfigs->setIdleTimerCallbacks(
            {.platform = {.onReset = [this] { idleTimerCallback(TimerState::Reset); },
                          .onExpired = [this] { idleTimerCallback(TimerState::Expired); }},
             .kernel = {.onReset = [this] { kernelIdleTimerCallback(TimerState::Reset); },
                        .onExpired = [this] { kernelIdleTimerCallback(TimerState::Expired); }}});

    mRefreshRateConfigs->startIdleTimer();
}

void Scheduler::run() {
    while (true) {
        waitMessage();
    }
}

void Scheduler::onFrameSignal(ICompositor& compositor, VsyncId vsyncId,
                              TimePoint expectedVsyncTime) {
    const TimePoint frameTime = SchedulerClock::now();

    if (!compositor.commit(frameTime, vsyncId, expectedVsyncTime)) {
        return;
    }

    compositor.composite(frameTime, vsyncId);
    compositor.sample();
}

void Scheduler::createVsyncSchedule(FeatureFlags features) {
    mVsyncSchedule.emplace(features);
}

std::unique_ptr<VSyncSource> Scheduler::makePrimaryDispSyncSource(
        const char* name, std::chrono::nanoseconds workDuration,
        std::chrono::nanoseconds readyDuration, bool traceVsync) {
    return std::make_unique<scheduler::DispSyncSource>(mVsyncSchedule->getDispatch(),
                                                       mVsyncSchedule->getTracker(), workDuration,
                                                       readyDuration, traceVsync, name);
}

std::optional<Fps> Scheduler::getFrameRateOverride(uid_t uid) const {
    const auto refreshRateConfigs = holdRefreshRateConfigs();
    const bool supportsFrameRateOverrideByContent =
            refreshRateConfigs->supportsFrameRateOverrideByContent();
    return mFrameRateOverrideMappings
            .getFrameRateOverrideForUid(uid, supportsFrameRateOverrideByContent);
}

bool Scheduler::isVsyncValid(TimePoint expectedVsyncTimestamp, uid_t uid) const {
    const auto frameRate = getFrameRateOverride(uid);
    if (!frameRate.has_value()) {
        return true;
    }

    return mVsyncSchedule->getTracker().isVSyncInPhase(expectedVsyncTimestamp.ns(), *frameRate);
}

impl::EventThread::ThrottleVsyncCallback Scheduler::makeThrottleVsyncCallback() const {
    std::scoped_lock lock(mRefreshRateConfigsLock);

    return [this](nsecs_t expectedVsyncTimestamp, uid_t uid) {
        return !isVsyncValid(TimePoint::fromNs(expectedVsyncTimestamp), uid);
    };
}

impl::EventThread::GetVsyncPeriodFunction Scheduler::makeGetVsyncPeriodFunction() const {
    return [this](uid_t uid) {
        const Fps refreshRate = holdRefreshRateConfigs()->getActiveModePtr()->getFps();
        const nsecs_t currentPeriod = mVsyncSchedule->period().ns() ?: refreshRate.getPeriodNsecs();

        const auto frameRate = getFrameRateOverride(uid);
        if (!frameRate.has_value()) {
            return currentPeriod;
        }

        const auto divisor = RefreshRateConfigs::getFrameRateDivisor(refreshRate, *frameRate);
        if (divisor <= 1) {
            return currentPeriod;
        }
        return currentPeriod * divisor;
    };
}

ConnectionHandle Scheduler::createConnection(
        const char* connectionName, frametimeline::TokenManager* tokenManager,
        std::chrono::nanoseconds workDuration, std::chrono::nanoseconds readyDuration,
        impl::EventThread::InterceptVSyncsCallback interceptCallback) {
    auto vsyncSource = makePrimaryDispSyncSource(connectionName, workDuration, readyDuration);
    auto throttleVsync = makeThrottleVsyncCallback();
    auto getVsyncPeriod = makeGetVsyncPeriodFunction();
    auto eventThread = std::make_unique<impl::EventThread>(std::move(vsyncSource), tokenManager,
                                                           std::move(interceptCallback),
                                                           std::move(throttleVsync),
                                                           std::move(getVsyncPeriod));
    return createConnection(std::move(eventThread));
}

ConnectionHandle Scheduler::createConnection(std::unique_ptr<EventThread> eventThread) {
    const ConnectionHandle handle = ConnectionHandle{mNextConnectionHandleId++};
    ALOGV("Creating a connection handle with ID %" PRIuPTR, handle.id);

    auto connection = createConnectionInternal(eventThread.get());

    std::lock_guard<std::mutex> lock(mConnectionsLock);
    mConnections.emplace(handle, Connection{connection, std::move(eventThread)});
    return handle;
}

sp<EventThreadConnection> Scheduler::createConnectionInternal(
        EventThread* eventThread, EventRegistrationFlags eventRegistration) {
    return eventThread->createEventConnection([&] { resync(); }, eventRegistration);
}

sp<IDisplayEventConnection> Scheduler::createDisplayEventConnection(
        ConnectionHandle handle, EventRegistrationFlags eventRegistration) {
    std::lock_guard<std::mutex> lock(mConnectionsLock);
    RETURN_IF_INVALID_HANDLE(handle, nullptr);
    return createConnectionInternal(mConnections[handle].thread.get(), eventRegistration);
}

sp<EventThreadConnection> Scheduler::getEventConnection(ConnectionHandle handle) {
    std::lock_guard<std::mutex> lock(mConnectionsLock);
    RETURN_IF_INVALID_HANDLE(handle, nullptr);
    return mConnections[handle].connection;
}

void Scheduler::onHotplugReceived(ConnectionHandle handle, PhysicalDisplayId displayId,
                                  bool connected) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }

    thread->onHotplugReceived(displayId, connected);
}

void Scheduler::onScreenAcquired(ConnectionHandle handle) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->onScreenAcquired();
    mScreenAcquired = true;
}

void Scheduler::onScreenReleased(ConnectionHandle handle) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->onScreenReleased();
    mScreenAcquired = false;
}

void Scheduler::onFrameRateOverridesChanged(ConnectionHandle handle, PhysicalDisplayId displayId) {
    const auto refreshRateConfigs = holdRefreshRateConfigs();
    const bool supportsFrameRateOverrideByContent =
            refreshRateConfigs->supportsFrameRateOverrideByContent();

    std::vector<FrameRateOverride> overrides =
            mFrameRateOverrideMappings.getAllFrameRateOverrides(supportsFrameRateOverrideByContent);

    android::EventThread* thread;
    {
        std::lock_guard lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->onFrameRateOverridesChanged(displayId, std::move(overrides));
}

void Scheduler::onPrimaryDisplayModeChanged(ConnectionHandle handle, DisplayModePtr mode) {
    {
        std::lock_guard<std::mutex> lock(mPolicyLock);
        // Cache the last reported modes for primary display.
        mPolicy.cachedModeChangedParams = {handle, mode};

        // Invalidate content based refresh rate selection so it could be calculated
        // again for the new refresh rate.
        mPolicy.contentRequirements.clear();
    }
    onNonPrimaryDisplayModeChanged(handle, mode);
}

void Scheduler::dispatchCachedReportedMode() {
    // Check optional fields first.
    if (!mPolicy.mode) {
        ALOGW("No mode ID found, not dispatching cached mode.");
        return;
    }
    if (!mPolicy.cachedModeChangedParams) {
        ALOGW("No mode changed params found, not dispatching cached mode.");
        return;
    }

    // If the mode is not the current mode, this means that a
    // mode change is in progress. In that case we shouldn't dispatch an event
    // as it will be dispatched when the current mode changes.
    if (std::scoped_lock lock(mRefreshRateConfigsLock);
        mRefreshRateConfigs->getActiveModePtr() != mPolicy.mode) {
        return;
    }

    // If there is no change from cached mode, there is no need to dispatch an event
    if (mPolicy.mode == mPolicy.cachedModeChangedParams->mode) {
        return;
    }

    mPolicy.cachedModeChangedParams->mode = mPolicy.mode;
    onNonPrimaryDisplayModeChanged(mPolicy.cachedModeChangedParams->handle,
                                   mPolicy.cachedModeChangedParams->mode);
}

void Scheduler::onNonPrimaryDisplayModeChanged(ConnectionHandle handle, DisplayModePtr mode) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->onModeChanged(mode);
}

size_t Scheduler::getEventThreadConnectionCount(ConnectionHandle handle) {
    std::lock_guard<std::mutex> lock(mConnectionsLock);
    RETURN_IF_INVALID_HANDLE(handle, 0);
    return mConnections[handle].thread->getEventThreadConnectionCount();
}

void Scheduler::dump(ConnectionHandle handle, std::string& result) const {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections.at(handle).thread.get();
    }
    thread->dump(result);
}

void Scheduler::setDuration(ConnectionHandle handle, std::chrono::nanoseconds workDuration,
                            std::chrono::nanoseconds readyDuration) {
    android::EventThread* thread;
    {
        std::lock_guard<std::mutex> lock(mConnectionsLock);
        RETURN_IF_INVALID_HANDLE(handle);
        thread = mConnections[handle].thread.get();
    }
    thread->setDuration(workDuration, readyDuration);
}

ConnectionHandle Scheduler::enableVSyncInjection(bool enable) {
    if (mInjectVSyncs == enable) {
        return {};
    }

    ALOGV("%s VSYNC injection", enable ? "Enabling" : "Disabling");

    if (!mInjectorConnectionHandle) {
        auto vsyncSource = std::make_unique<InjectVSyncSource>();
        mVSyncInjector = vsyncSource.get();

        auto eventThread =
                std::make_unique<impl::EventThread>(std::move(vsyncSource),
                                                    /*tokenManager=*/nullptr,
                                                    impl::EventThread::InterceptVSyncsCallback(),
                                                    impl::EventThread::ThrottleVsyncCallback(),
                                                    impl::EventThread::GetVsyncPeriodFunction());

        // EventThread does not dispatch VSYNC unless the display is connected and powered on.
        eventThread->onHotplugReceived(PhysicalDisplayId::fromPort(0), true);
        eventThread->onScreenAcquired();

        mInjectorConnectionHandle = createConnection(std::move(eventThread));
    }

    mInjectVSyncs = enable;
    return mInjectorConnectionHandle;
}

bool Scheduler::injectVSync(nsecs_t when, nsecs_t expectedVSyncTime, nsecs_t deadlineTimestamp) {
    if (!mInjectVSyncs || !mVSyncInjector) {
        return false;
    }

    mVSyncInjector->onInjectSyncEvent(when, expectedVSyncTime, deadlineTimestamp);
    return true;
}

void Scheduler::enableHardwareVsync() {
    std::lock_guard<std::mutex> lock(mHWVsyncLock);
    if (!mPrimaryHWVsyncEnabled && mHWVsyncAvailable) {
        mVsyncSchedule->getTracker().resetModel();
        mSchedulerCallback.setVsyncEnabled(true);
        mPrimaryHWVsyncEnabled = true;
    }
}

void Scheduler::disableHardwareVsync(bool makeUnavailable) {
    std::lock_guard<std::mutex> lock(mHWVsyncLock);
    if (mPrimaryHWVsyncEnabled) {
        mSchedulerCallback.setVsyncEnabled(false);
        mPrimaryHWVsyncEnabled = false;
    }
    if (makeUnavailable) {
        mHWVsyncAvailable = false;
    }
}

void Scheduler::resyncToHardwareVsync(bool makeAvailable, Fps refreshRate) {
    {
        std::lock_guard<std::mutex> lock(mHWVsyncLock);
        if (makeAvailable) {
            mHWVsyncAvailable = makeAvailable;
        } else if (!mHWVsyncAvailable) {
            // Hardware vsync is not currently available, so abort the resync
            // attempt for now
            return;
        }
    }

    setVsyncPeriod(refreshRate.getPeriodNsecs());
}

void Scheduler::resync() {
    static constexpr nsecs_t kIgnoreDelay = ms2ns(750);

    const nsecs_t now = systemTime();
    const nsecs_t last = mLastResyncTime.exchange(now);

    if (now - last > kIgnoreDelay) {
        const auto refreshRate = [&] {
            std::scoped_lock lock(mRefreshRateConfigsLock);
            return mRefreshRateConfigs->getActiveModePtr()->getFps();
        }();
        resyncToHardwareVsync(false, refreshRate);
    }
}

void Scheduler::setVsyncPeriod(nsecs_t period) {
    if (period <= 0) return;

    std::lock_guard<std::mutex> lock(mHWVsyncLock);
    mVsyncSchedule->getController().startPeriodTransition(period);

    if (!mPrimaryHWVsyncEnabled) {
        mVsyncSchedule->getTracker().resetModel();
        mSchedulerCallback.setVsyncEnabled(true);
        mPrimaryHWVsyncEnabled = true;
    }
}

void Scheduler::addResyncSample(nsecs_t timestamp, std::optional<nsecs_t> hwcVsyncPeriod,
                                bool* periodFlushed) {
    bool needsHwVsync = false;
    *periodFlushed = false;
    { // Scope for the lock
        std::lock_guard<std::mutex> lock(mHWVsyncLock);
        if (mPrimaryHWVsyncEnabled) {
            needsHwVsync =
                    mVsyncSchedule->getController().addHwVsyncTimestamp(timestamp, hwcVsyncPeriod,
                                                                        periodFlushed);
        }
    }

    if (needsHwVsync) {
        enableHardwareVsync();
    } else {
        disableHardwareVsync(false);
    }
}

void Scheduler::addPresentFence(std::shared_ptr<FenceTime> fence) {
    if (mVsyncSchedule->getController().addPresentFence(std::move(fence))) {
        enableHardwareVsync();
    } else {
        disableHardwareVsync(false);
    }
}

void Scheduler::registerLayer(Layer* layer) {
    // If the content detection feature is off, we still keep the layer history,
    // since we use it for other features (like Frame Rate API), so layers
    // still need to be registered.
    mLayerHistory.registerLayer(layer, mFeatures.test(Feature::kContentDetection));
}

void Scheduler::deregisterLayer(Layer* layer) {
    mLayerHistory.deregisterLayer(layer);
}

void Scheduler::recordLayerHistory(Layer* layer, nsecs_t presentTime,
                                   LayerHistory::LayerUpdateType updateType) {
    {
        std::scoped_lock lock(mRefreshRateConfigsLock);
        if (!mRefreshRateConfigs->canSwitch()) return;
    }

    mLayerHistory.record(layer, presentTime, systemTime(), updateType);
}

void Scheduler::setModeChangePending(bool pending) {
    mLayerHistory.setModeChangePending(pending);
}

void Scheduler::setDefaultFrameRateCompatibility(Layer* layer) {
    mLayerHistory.setDefaultFrameRateCompatibility(layer,
                                                   mFeatures.test(Feature::kContentDetection));
}

void Scheduler::chooseRefreshRateForContent() {
    const auto configs = holdRefreshRateConfigs();
    if (!configs->canSwitch()) return;

    ATRACE_CALL();

    LayerHistory::Summary summary = mLayerHistory.summarize(*configs, systemTime());
    applyPolicy(&Policy::contentRequirements, std::move(summary));
}

void Scheduler::resetIdleTimer() {
    std::scoped_lock lock(mRefreshRateConfigsLock);
    mRefreshRateConfigs->resetIdleTimer(/*kernelOnly*/ false);
}

void Scheduler::onTouchHint() {
    if (mTouchTimer) {
        mTouchTimer->reset();

        std::scoped_lock lock(mRefreshRateConfigsLock);
        mRefreshRateConfigs->resetIdleTimer(/*kernelOnly*/ true);
    }
}

void Scheduler::setDisplayPowerMode(hal::PowerMode powerMode) {
    {
        std::lock_guard<std::mutex> lock(mPolicyLock);
        mPolicy.displayPowerMode = powerMode;
    }
    mVsyncSchedule->getController().setDisplayPowerMode(powerMode);

    if (mDisplayPowerTimer) {
        mDisplayPowerTimer->reset();
    }

    // Display Power event will boost the refresh rate to performance.
    // Clear Layer History to get fresh FPS detection
    mLayerHistory.clear();
}

void Scheduler::kernelIdleTimerCallback(TimerState state) {
    ATRACE_INT("ExpiredKernelIdleTimer", static_cast<int>(state));

    // TODO(145561154): cleanup the kernel idle timer implementation and the refresh rate
    // magic number
    const Fps refreshRate = [&] {
        std::scoped_lock lock(mRefreshRateConfigsLock);
        return mRefreshRateConfigs->getActiveModePtr()->getFps();
    }();

    constexpr Fps FPS_THRESHOLD_FOR_KERNEL_TIMER = 65_Hz;
    using namespace fps_approx_ops;

    if (state == TimerState::Reset && refreshRate > FPS_THRESHOLD_FOR_KERNEL_TIMER) {
        // If we're not in performance mode then the kernel timer shouldn't do
        // anything, as the refresh rate during DPU power collapse will be the
        // same.
        resyncToHardwareVsync(true /* makeAvailable */, refreshRate);
    } else if (state == TimerState::Expired && refreshRate <= FPS_THRESHOLD_FOR_KERNEL_TIMER) {
        // Disable HW VSYNC if the timer expired, as we don't need it enabled if
        // we're not pushing frames, and if we're in PERFORMANCE mode then we'll
        // need to update the VsyncController model anyway.
        disableHardwareVsync(false /* makeUnavailable */);
    }

    mSchedulerCallback.kernelTimerChanged(state == TimerState::Expired);
}

void Scheduler::idleTimerCallback(TimerState state) {
    applyPolicy(&Policy::idleTimer, state);
    ATRACE_INT("ExpiredIdleTimer", static_cast<int>(state));
}

void Scheduler::touchTimerCallback(TimerState state) {
    const TouchState touch = state == TimerState::Reset ? TouchState::Active : TouchState::Inactive;
    // Touch event will boost the refresh rate to performance.
    // Clear layer history to get fresh FPS detection.
    // NOTE: Instead of checking all the layers, we should be checking the layer
    // that is currently on top. b/142507166 will give us this capability.
    if (applyPolicy(&Policy::touch, touch).touch) {
        mLayerHistory.clear();
    }
    ATRACE_INT("TouchState", static_cast<int>(touch));
}

void Scheduler::displayPowerTimerCallback(TimerState state) {
    applyPolicy(&Policy::displayPowerTimer, state);
    ATRACE_INT("ExpiredDisplayPowerTimer", static_cast<int>(state));
}

void Scheduler::dump(std::string& result) const {
    using base::StringAppendF;

    StringAppendF(&result, "+  Touch timer: %s\n",
                  mTouchTimer ? mTouchTimer->dump().c_str() : "off");
    StringAppendF(&result, "+  Content detection: %s %s\n\n",
                  mFeatures.test(Feature::kContentDetection) ? "on" : "off",
                  mLayerHistory.dump().c_str());

    mFrameRateOverrideMappings.dump(result);

    {
        std::lock_guard lock(mHWVsyncLock);
        StringAppendF(&result,
                      "mScreenAcquired=%d mPrimaryHWVsyncEnabled=%d mHWVsyncAvailable=%d\n",
                      mScreenAcquired.load(), mPrimaryHWVsyncEnabled, mHWVsyncAvailable);
    }
}

void Scheduler::dumpVsync(std::string& out) const {
    mVsyncSchedule->dump(out);
}

bool Scheduler::updateFrameRateOverrides(GlobalSignals consideredSignals, Fps displayRefreshRate) {
    const auto refreshRateConfigs = holdRefreshRateConfigs();

    // we always update mFrameRateOverridesByContent here
    // supportsFrameRateOverridesByContent will be checked
    // when getting FrameRateOverrides from mFrameRateOverrideMappings
    if (!consideredSignals.idle) {
        const auto frameRateOverrides =
                refreshRateConfigs->getFrameRateOverrides(mPolicy.contentRequirements,
                                                          displayRefreshRate, consideredSignals);
        return mFrameRateOverrideMappings.updateFrameRateOverridesByContent(frameRateOverrides);
    }
    return false;
}

template <typename S, typename T>
auto Scheduler::applyPolicy(S Policy::*statePtr, T&& newState) -> GlobalSignals {
    DisplayModePtr newMode;
    GlobalSignals consideredSignals;
    std::vector<DisplayModeConfig> displayModeConfigs;

    bool refreshRateChanged = false;
    bool frameRateOverridesChanged;

    const auto refreshRateConfigs = holdRefreshRateConfigs();
    {
        std::lock_guard<std::mutex> lock(mPolicyLock);

        auto& currentState = mPolicy.*statePtr;
        if (currentState == newState) return {};
        currentState = std::forward<T>(newState);

        displayModeConfigs = getBestDisplayModeConfigs();

        // mPolicy holds the current mode, using the current mode we find out
        // what display is currently being tracked through the policy and
        // then find the DisplayModeConfig for that display. So that
        // later we check if the policy mode has changed for the same display in policy.
        // If mPolicy mode isn't available then we take the first display from the best display
        // modes as the candidate for policy changes and frame rate overrides.
        // TODO(b/240743786) Update the single display based assumptions and make mode changes
        // and mPolicy per display.
        const DisplayModeConfig& displayModeConfigForCurrentPolicy = mPolicy.mode
                ? *std::find_if(displayModeConfigs.begin(), displayModeConfigs.end(),
                                [&](const auto& displayModeConfig) REQUIRES(mPolicyLock) {
                                    return displayModeConfig.displayModePtr
                                                   ->getPhysicalDisplayId() ==
                                            mPolicy.mode->getPhysicalDisplayId();
                                })
                : displayModeConfigs.front();

        newMode = displayModeConfigForCurrentPolicy.displayModePtr;
        consideredSignals = displayModeConfigForCurrentPolicy.signals;
        frameRateOverridesChanged = updateFrameRateOverrides(consideredSignals, newMode->getFps());

        if (mPolicy.mode == newMode) {
            // We don't need to change the display mode, but we might need to send an event
            // about a mode change, since it was suppressed if previously considered idle.
            if (!consideredSignals.idle) {
                dispatchCachedReportedMode();
            }
        } else {
            mPolicy.mode = newMode;
            refreshRateChanged = true;
        }
    }
    if (refreshRateChanged) {
        mSchedulerCallback.requestDisplayModes(std::move(displayModeConfigs));
    }
    if (frameRateOverridesChanged) {
        mSchedulerCallback.triggerOnFrameRateOverridesChanged();
    }
    return consideredSignals;
}

void Scheduler::registerDisplay(const sp<const DisplayDevice>& display) {
    const bool ok = mDisplays.try_emplace(display->getPhysicalId(), display).second;
    ALOGE_IF(!ok, "Duplicate display registered");
}

void Scheduler::unregisterDisplay(PhysicalDisplayId displayId) {
    mDisplays.erase(displayId);
}

std::vector<DisplayModeConfig> Scheduler::getBestDisplayModeConfigs() const {
    ATRACE_CALL();

    std::vector<RefreshRateRankingsAndSignals> refreshRateRankingsAndSignalsPerDisplay;
    refreshRateRankingsAndSignalsPerDisplay.reserve(mDisplays.size());

    const auto displayModeSelectionParams = getDisplayModeSelectionParams();

    std::for_each(mDisplays.begin(), mDisplays.end(), [&](const auto& display) {
        const auto& [refreshRateRankings, globalSignals] =
                display.second->holdRefreshRateConfigs()
                        ->getRankedRefreshRates(displayModeSelectionParams.layerRequirements,
                                                displayModeSelectionParams.globalSignals);
        refreshRateRankingsAndSignalsPerDisplay.emplace_back(
                RefreshRateRankingsAndSignals{refreshRateRankings, globalSignals});
    });

    // FPS and their Aggregated score.
    std::unordered_map<Fps, AggregatedFpsScore, FpsHash, FpsApproxEqual> aggregatedScoresPerFps =
            getAggregatedScoresPerFps(refreshRateRankingsAndSignalsPerDisplay);

    auto maxScoreIt = aggregatedScoresPerFps.cbegin();
    // Selects the max Fps that is present on all the displays.
    for (auto it = aggregatedScoresPerFps.cbegin(); it != aggregatedScoresPerFps.cend(); ++it) {
        const auto [fps, aggregatedScore] = *it;
        if (aggregatedScore.numDisplays == mDisplays.size() &&
            aggregatedScore.totalScore >= maxScoreIt->second.totalScore) {
            maxScoreIt = it;
        }
    }
    return getDisplayModeConfigsForTheChosenFps(maxScoreIt->first,
                                                refreshRateRankingsAndSignalsPerDisplay);
}

std::vector<DisplayModeConfig> Scheduler::getDisplayModeConfigsForTheChosenFps(
        Fps chosenFps,
        const std::vector<RefreshRateRankingsAndSignals>& refreshRateRankingsAndSignalsPerDisplay)
        const {
    std::vector<DisplayModeConfig> displayModeConfigs;
    displayModeConfigs.reserve(mDisplays.size());
    using fps_approx_ops::operator==;
    std::for_each(refreshRateRankingsAndSignalsPerDisplay.begin(),
                  refreshRateRankingsAndSignalsPerDisplay.end(),
                  [&](const auto& refreshRateRankingsAndSignal) {
                      for (const auto& ranking : refreshRateRankingsAndSignal.refreshRateRankings) {
                          if (ranking.displayModePtr->getFps() == chosenFps) {
                              displayModeConfigs.emplace_back(
                                      DisplayModeConfig{refreshRateRankingsAndSignal.globalSignals,
                                                        ranking.displayModePtr});
                              break;
                          }
                      }
                  });
    return displayModeConfigs;
}

DisplayModeSelectionParams Scheduler::getDisplayModeSelectionParams() const {
    const bool powerOnImminent = mDisplayPowerTimer &&
            (mPolicy.displayPowerMode != hal::PowerMode::ON ||
             mPolicy.displayPowerTimer == TimerState::Reset);

    const GlobalSignals signals{.touch = mTouchTimer && mPolicy.touch == TouchState::Active,
                                .idle = mPolicy.idleTimer == TimerState::Expired,
                                .powerOnImminent = powerOnImminent};

    return {mPolicy.contentRequirements, signals};
}

auto Scheduler::getRankedDisplayModes()
        -> std::pair<std::vector<RefreshRateRanking>, GlobalSignals> {
    ATRACE_CALL();

    const auto configs = holdRefreshRateConfigs();

    const auto displayModeSelectionParams = getDisplayModeSelectionParams();
    return configs->getRankedRefreshRates(displayModeSelectionParams.layerRequirements,
                                          displayModeSelectionParams.globalSignals);
}

DisplayModePtr Scheduler::getPreferredDisplayMode() {
    std::lock_guard<std::mutex> lock(mPolicyLock);
    // Make sure the stored mode is up to date.
    if (mPolicy.mode) {
        mPolicy.mode = getRankedDisplayModes().first.front().displayModePtr;
    }
    return mPolicy.mode;
}

void Scheduler::onNewVsyncPeriodChangeTimeline(const hal::VsyncPeriodChangeTimeline& timeline) {
    std::lock_guard<std::mutex> lock(mVsyncTimelineLock);
    mLastVsyncPeriodChangeTimeline = std::make_optional(timeline);

    const auto maxAppliedTime = systemTime() + MAX_VSYNC_APPLIED_TIME.count();
    if (timeline.newVsyncAppliedTimeNanos > maxAppliedTime) {
        mLastVsyncPeriodChangeTimeline->newVsyncAppliedTimeNanos = maxAppliedTime;
    }
}

bool Scheduler::onPostComposition(nsecs_t presentTime) {
    std::lock_guard<std::mutex> lock(mVsyncTimelineLock);
    if (mLastVsyncPeriodChangeTimeline && mLastVsyncPeriodChangeTimeline->refreshRequired) {
        if (presentTime < mLastVsyncPeriodChangeTimeline->refreshTimeNanos) {
            // We need to composite again as refreshTimeNanos is still in the future.
            return true;
        }

        mLastVsyncPeriodChangeTimeline->refreshRequired = false;
    }
    return false;
}

void Scheduler::onActiveDisplayAreaChanged(uint32_t displayArea) {
    mLayerHistory.setDisplayArea(displayArea);
}

void Scheduler::setGameModeRefreshRateForUid(FrameRateOverride frameRateOverride) {
    if (frameRateOverride.frameRateHz > 0.f && frameRateOverride.frameRateHz < 1.f) {
        return;
    }

    mFrameRateOverrideMappings.setGameModeRefreshRateForUid(frameRateOverride);
}

void Scheduler::setPreferredRefreshRateForUid(FrameRateOverride frameRateOverride) {
    if (frameRateOverride.frameRateHz > 0.f && frameRateOverride.frameRateHz < 1.f) {
        return;
    }

    mFrameRateOverrideMappings.setPreferredRefreshRateForUid(frameRateOverride);
}

} // namespace android::scheduler
