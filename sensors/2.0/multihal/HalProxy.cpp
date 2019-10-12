/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "HalProxy.h"

#include "SubHal.h"

#include <android/hardware/sensors/2.0/types.h>

#include "hardware_legacy/power.h"

#include <dlfcn.h>

#include <fstream>
#include <functional>
#include <thread>

namespace android {
namespace hardware {
namespace sensors {
namespace V2_0 {
namespace implementation {

using ::android::hardware::sensors::V2_0::EventQueueFlagBits;

typedef ISensorsSubHal*(SensorsHalGetSubHalFunc)(uint32_t*);

/**
 * Set the subhal index as first byte of sensor handle and return this modified version.
 *
 * @param sensorHandle The sensor handle to modify.
 * @param subHalIndex The index in the hal proxy of the sub hal this sensor belongs to.
 *
 * @return The modified sensor handle.
 */
uint32_t setSubHalIndex(uint32_t sensorHandle, size_t subHalIndex) {
    return sensorHandle | (subHalIndex << 24);
}

HalProxy::HalProxy() {
    const char* kMultiHalConfigFile = "/vendor/etc/sensors/hals.conf";
    initializeSubHalListFromConfigFile(kMultiHalConfigFile);
    initializeSubHalCallbacksAndSensorList();
}

HalProxy::HalProxy(std::vector<ISensorsSubHal*>& subHalList) : mSubHalList(subHalList) {
    initializeSubHalCallbacksAndSensorList();
}

HalProxy::~HalProxy() {
    {
        std::lock_guard<std::mutex> lockGuard(mEventQueueWriteMutex);
        mPendingWritesRun = false;
        mEventQueueWriteCV.notify_one();
    }
    if (mPendingWritesThread.joinable()) {
        mPendingWritesThread.join();
    }
    // TODO: Cleanup wakeup thread once it is implemented
}

Return<void> HalProxy::getSensorsList(getSensorsList_cb _hidl_cb) {
    std::vector<SensorInfo> sensors;
    for (const auto& iter : mSensors) {
        sensors.push_back(iter.second);
    }
    _hidl_cb(sensors);
    return Void();
}

Return<Result> HalProxy::setOperationMode(OperationMode mode) {
    Result result = Result::OK;
    size_t subHalIndex;
    for (subHalIndex = 0; subHalIndex < mSubHalList.size(); subHalIndex++) {
        ISensorsSubHal* subHal = mSubHalList[subHalIndex];
        result = subHal->setOperationMode(mode);
        if (result != Result::OK) {
            ALOGE("setOperationMode failed for SubHal: %s", subHal->getName().c_str());
            break;
        }
    }
    if (result != Result::OK) {
        // Reset the subhal operation modes that have been flipped
        for (size_t i = 0; i < subHalIndex; i++) {
            ISensorsSubHal* subHal = mSubHalList[i];
            subHal->setOperationMode(mCurrentOperationMode);
        }
    } else {
        mCurrentOperationMode = mode;
    }
    return result;
}

Return<Result> HalProxy::activate(int32_t sensorHandle, bool enabled) {
    return getSubHalForSensorHandle(sensorHandle)
            ->activate(clearSubHalIndex(sensorHandle), enabled);
}

Return<Result> HalProxy::initialize(
        const ::android::hardware::MQDescriptorSync<Event>& eventQueueDescriptor,
        const ::android::hardware::MQDescriptorSync<uint32_t>& wakeLockDescriptor,
        const sp<ISensorsCallback>& sensorsCallback) {
    Result result = Result::OK;

    // TODO: clean up sensor requests, if not already done elsewhere through a death recipient, and
    // clean up any other resources that exist (FMQs, flags, threads, etc.)

    mDynamicSensorsCallback = sensorsCallback;

    // Create the Event FMQ from the eventQueueDescriptor. Reset the read/write positions.
    mEventQueue =
            std::make_unique<EventMessageQueue>(eventQueueDescriptor, true /* resetPointers */);

    // Create the EventFlag that is used to signal to the framework that sensor events have been
    // written to the Event FMQ
    if (EventFlag::createEventFlag(mEventQueue->getEventFlagWord(), &mEventQueueFlag) != OK) {
        result = Result::BAD_VALUE;
    }

    // Create the Wake Lock FMQ that is used by the framework to communicate whenever WAKE_UP
    // events have been successfully read and handled by the framework.
    mWakeLockQueue =
            std::make_unique<WakeLockMessageQueue>(wakeLockDescriptor, true /* resetPointers */);

    if (!mDynamicSensorsCallback || !mEventQueue || !mWakeLockQueue || mEventQueueFlag == nullptr) {
        result = Result::BAD_VALUE;
    }

    mPendingWritesThread = std::thread(startPendingWritesThread, this);
    // TODO: start threads to read wake locks.

    for (size_t i = 0; i < mSubHalList.size(); i++) {
        auto subHal = mSubHalList[i];
        const auto& subHalCallback = mSubHalCallbacks[i];
        Result currRes = subHal->initialize(subHalCallback);
        if (currRes != Result::OK) {
            result = currRes;
            ALOGE("Subhal '%s' failed to initialize.", subHal->getName().c_str());
            break;
        }
    }

    return result;
}

Return<Result> HalProxy::batch(int32_t sensorHandle, int64_t samplingPeriodNs,
                               int64_t maxReportLatencyNs) {
    return getSubHalForSensorHandle(sensorHandle)
            ->batch(clearSubHalIndex(sensorHandle), samplingPeriodNs, maxReportLatencyNs);
}

Return<Result> HalProxy::flush(int32_t sensorHandle) {
    return getSubHalForSensorHandle(sensorHandle)->flush(clearSubHalIndex(sensorHandle));
}

Return<Result> HalProxy::injectSensorData(const Event& event) {
    Result result = Result::OK;
    if (mCurrentOperationMode == OperationMode::NORMAL &&
        event.sensorType != V1_0::SensorType::ADDITIONAL_INFO) {
        ALOGE("An event with type != ADDITIONAL_INFO passed to injectSensorData while operation"
              " mode was NORMAL.");
        result = Result::BAD_VALUE;
    }
    if (result == Result::OK) {
        Event subHalEvent = event;
        subHalEvent.sensorHandle = clearSubHalIndex(event.sensorHandle);
        result = getSubHalForSensorHandle(event.sensorHandle)->injectSensorData(subHalEvent);
    }
    return result;
}

Return<void> HalProxy::registerDirectChannel(const SharedMemInfo& mem,
                                             registerDirectChannel_cb _hidl_cb) {
    if (mDirectChannelSubHal == nullptr) {
        _hidl_cb(Result::INVALID_OPERATION, -1 /* channelHandle */);
    } else {
        mDirectChannelSubHal->registerDirectChannel(mem, _hidl_cb);
    }
    return Return<void>();
}

Return<Result> HalProxy::unregisterDirectChannel(int32_t channelHandle) {
    Result result;
    if (mDirectChannelSubHal == nullptr) {
        result = Result::INVALID_OPERATION;
    } else {
        result = mDirectChannelSubHal->unregisterDirectChannel(channelHandle);
    }
    return result;
}

Return<void> HalProxy::configDirectReport(int32_t sensorHandle, int32_t channelHandle,
                                          RateLevel rate, configDirectReport_cb _hidl_cb) {
    if (mDirectChannelSubHal == nullptr) {
        _hidl_cb(Result::INVALID_OPERATION, -1 /* reportToken */);
    } else {
        mDirectChannelSubHal->configDirectReport(clearSubHalIndex(sensorHandle), channelHandle,
                                                 rate, _hidl_cb);
    }
    return Return<void>();
}

Return<void> HalProxy::debug(const hidl_handle& /* fd */, const hidl_vec<hidl_string>& /* args */) {
    // TODO: output debug information
    return Return<void>();
}

Return<void> HalProxy::onDynamicSensorsConnected(const hidl_vec<SensorInfo>& dynamicSensorsAdded,
                                                 int32_t subHalIndex) {
    std::vector<SensorInfo> sensors;
    {
        std::lock_guard<std::mutex> lock(mDynamicSensorsMutex);
        for (SensorInfo sensor : dynamicSensorsAdded) {
            if (!subHalIndexIsClear(sensor.sensorHandle)) {
                ALOGE("Dynamic sensor added %s had sensorHandle with first byte not 0.",
                      sensor.name.c_str());
            } else {
                sensor.sensorHandle = setSubHalIndex(sensor.sensorHandle, subHalIndex);
                mDynamicSensors[sensor.sensorHandle] = sensor;
                sensors.push_back(sensor);
            }
        }
    }
    mDynamicSensorsCallback->onDynamicSensorsConnected(sensors);
    return Return<void>();
}

Return<void> HalProxy::onDynamicSensorsDisconnected(
        const hidl_vec<int32_t>& dynamicSensorHandlesRemoved, int32_t subHalIndex) {
    // TODO: Block this call until all pending events are flushed from queue
    std::vector<int32_t> sensorHandles;
    {
        std::lock_guard<std::mutex> lock(mDynamicSensorsMutex);
        for (int32_t sensorHandle : dynamicSensorHandlesRemoved) {
            if (!subHalIndexIsClear(sensorHandle)) {
                ALOGE("Dynamic sensorHandle removed had first byte not 0.");
            } else {
                sensorHandle = setSubHalIndex(sensorHandle, subHalIndex);
                if (mDynamicSensors.find(sensorHandle) != mDynamicSensors.end()) {
                    mDynamicSensors.erase(sensorHandle);
                    sensorHandles.push_back(sensorHandle);
                }
            }
        }
    }
    mDynamicSensorsCallback->onDynamicSensorsDisconnected(sensorHandles);
    return Return<void>();
}

void HalProxy::initializeSubHalListFromConfigFile(const char* configFileName) {
    std::ifstream subHalConfigStream(configFileName);
    if (!subHalConfigStream) {
        ALOGE("Failed to load subHal config file: %s", configFileName);
    } else {
        std::string subHalLibraryFile;
        while (subHalConfigStream >> subHalLibraryFile) {
            void* handle = dlopen(subHalLibraryFile.c_str(), RTLD_NOW);
            if (handle == nullptr) {
                ALOGE("dlopen failed for library: %s", subHalLibraryFile.c_str());
            } else {
                SensorsHalGetSubHalFunc* sensorsHalGetSubHalPtr =
                        (SensorsHalGetSubHalFunc*)dlsym(handle, "sensorsHalGetSubHal");
                if (sensorsHalGetSubHalPtr == nullptr) {
                    ALOGE("Failed to locate sensorsHalGetSubHal function for library: %s",
                          subHalLibraryFile.c_str());
                } else {
                    std::function<SensorsHalGetSubHalFunc> sensorsHalGetSubHal =
                            *sensorsHalGetSubHalPtr;
                    uint32_t version;
                    ISensorsSubHal* subHal = sensorsHalGetSubHal(&version);
                    if (version != SUB_HAL_2_0_VERSION) {
                        ALOGE("SubHal version was not 2.0 for library: %s",
                              subHalLibraryFile.c_str());
                    } else {
                        ALOGV("Loaded SubHal from library: %s", subHalLibraryFile.c_str());
                        mSubHalList.push_back(subHal);
                    }
                }
            }
        }
    }
}

void HalProxy::initializeSubHalCallbacks() {
    for (size_t subHalIndex = 0; subHalIndex < mSubHalList.size(); subHalIndex++) {
        sp<IHalProxyCallback> callback = new HalProxyCallback(this, subHalIndex);
        mSubHalCallbacks.push_back(callback);
    }
}

void HalProxy::initializeSensorList() {
    for (size_t subHalIndex = 0; subHalIndex < mSubHalList.size(); subHalIndex++) {
        ISensorsSubHal* subHal = mSubHalList[subHalIndex];
        auto result = subHal->getSensorsList([&](const auto& list) {
            for (SensorInfo sensor : list) {
                if (!subHalIndexIsClear(sensor.sensorHandle)) {
                    ALOGE("SubHal sensorHandle's first byte was not 0");
                } else {
                    ALOGV("Loaded sensor: %s", sensor.name.c_str());
                    sensor.sensorHandle |= (subHalIndex << 24);
                    setDirectChannelFlags(&sensor, subHal);
                    mSensors[sensor.sensorHandle] = sensor;
                }
            }
        });
        if (!result.isOk()) {
            ALOGE("getSensorsList call failed for SubHal: %s", subHal->getName().c_str());
        }
    }
}

void HalProxy::initializeSubHalCallbacksAndSensorList() {
    initializeSubHalCallbacks();
    initializeSensorList();
}

void HalProxy::startPendingWritesThread(HalProxy* halProxy) {
    halProxy->handlePendingWrites();
}

void HalProxy::handlePendingWrites() {
    // TODO: Find a way to optimize locking strategy maybe using two mutexes instead of one.
    std::unique_lock<std::mutex> lock(mEventQueueWriteMutex);
    while (mPendingWritesRun) {
        mEventQueueWriteCV.wait(
                lock, [&] { return !mPendingWriteEventsQueue.empty() || !mPendingWritesRun; });
        if (!mPendingWriteEventsQueue.empty() && mPendingWritesRun) {
            std::vector<Event>& pendingWriteEvents = mPendingWriteEventsQueue.front();
            size_t eventQueueSize = mEventQueue->getQuantumCount();
            size_t numToWrite = std::min(pendingWriteEvents.size(), eventQueueSize);
            lock.unlock();
            // TODO: Find a way to interrup writeBlocking if the thread should exit
            // so we don't have to wait for timeout on framework restarts.
            if (!mEventQueue->writeBlocking(
                        pendingWriteEvents.data(), numToWrite,
                        static_cast<uint32_t>(EventQueueFlagBits::EVENTS_READ),
                        static_cast<uint32_t>(EventQueueFlagBits::READ_AND_PROCESS),
                        kWakelockTimeoutNs, mEventQueueFlag)) {
                ALOGE("Dropping %zu events after blockingWrite failed.", numToWrite);
            } else {
                mEventQueueFlag->wake(static_cast<uint32_t>(EventQueueFlagBits::READ_AND_PROCESS));
            }
            lock.lock();
            if (pendingWriteEvents.size() > eventQueueSize) {
                // TODO: Check if this erase operation is too inefficient. It will copy all the
                // events ahead of it down to fill gap off array at front after the erase.
                pendingWriteEvents.erase(pendingWriteEvents.begin(),
                                         pendingWriteEvents.begin() + eventQueueSize);
            } else {
                mPendingWriteEventsQueue.pop();
            }
        }
    }
}

void HalProxy::postEventsToMessageQueue(const std::vector<Event>& events) {
    size_t numToWrite = 0;
    std::lock_guard<std::mutex> lock(mEventQueueWriteMutex);
    if (mPendingWriteEventsQueue.empty()) {
        numToWrite = std::min(events.size(), mEventQueue->availableToWrite());
        if (numToWrite > 0) {
            if (mEventQueue->write(events.data(), numToWrite)) {
                // TODO: While loop if mEventQueue->avaiableToWrite > 0 to possibly fit in more
                // writes immediately
                mEventQueueFlag->wake(static_cast<uint32_t>(EventQueueFlagBits::READ_AND_PROCESS));
            } else {
                numToWrite = 0;
            }
        }
    }
    if (numToWrite < events.size()) {
        // TODO: Bound the mPendingWriteEventsQueue so that we do not trigger OOMs if framework
        // stalls
        mPendingWriteEventsQueue.push(
                std::vector<Event>(events.begin() + numToWrite, events.end()));
        mEventQueueWriteCV.notify_one();
    }
}

// TODO: Implement the wakelock timeout in these next two methods. Also pass in the subhal
// index for better tracking.

void HalProxy::incrementRefCountAndMaybeAcquireWakelock() {
    std::lock_guard<std::mutex> lockGuard(mWakelockRefCountMutex);
    if (mWakelockRefCount == 0) {
        acquire_wake_lock(PARTIAL_WAKE_LOCK, kWakeLockName);
    }
    mWakelockRefCount++;
}

void HalProxy::decrementRefCountAndMaybeReleaseWakelock() {
    std::lock_guard<std::mutex> lockGuard(mWakelockRefCountMutex);
    mWakelockRefCount--;
    if (mWakelockRefCount == 0) {
        release_wake_lock(kWakeLockName);
    }
}

void HalProxy::setDirectChannelFlags(SensorInfo* sensorInfo, ISensorsSubHal* subHal) {
    bool sensorSupportsDirectChannel =
            (sensorInfo->flags & (V1_0::SensorFlagBits::MASK_DIRECT_REPORT |
                                  V1_0::SensorFlagBits::MASK_DIRECT_CHANNEL)) != 0;
    if (mDirectChannelSubHal == nullptr && sensorSupportsDirectChannel) {
        mDirectChannelSubHal = subHal;
    } else if (mDirectChannelSubHal != nullptr && subHal != mDirectChannelSubHal) {
        // disable direct channel capability for sensors in subHals that are not
        // the only one we will enable
        sensorInfo->flags &= ~(V1_0::SensorFlagBits::MASK_DIRECT_REPORT |
                               V1_0::SensorFlagBits::MASK_DIRECT_CHANNEL);
    }
}

ISensorsSubHal* HalProxy::getSubHalForSensorHandle(uint32_t sensorHandle) {
    return mSubHalList[static_cast<size_t>(sensorHandle >> 24)];
}

uint32_t HalProxy::clearSubHalIndex(uint32_t sensorHandle) {
    return sensorHandle & (~kSensorHandleSubHalIndexMask);
}

bool HalProxy::subHalIndexIsClear(uint32_t sensorHandle) {
    return (sensorHandle & kSensorHandleSubHalIndexMask) == 0;
}

void HalProxyCallback::postEvents(const std::vector<Event>& events, ScopedWakelock wakelock) {
    (void)wakelock;
    size_t numWakeupEvents;
    std::vector<Event> processedEvents = processEvents(events, &numWakeupEvents);
    if (numWakeupEvents > 0) {
        ALOG_ASSERT(wakelock.isLocked(),
                    "Wakeup events posted while wakelock unlocked for subhal"
                    " w/ index %zu.",
                    mSubHalIndex);
    } else {
        ALOG_ASSERT(!wakelock.isLocked(),
                    "No Wakeup events posted but wakelock locked for subhal"
                    " w/ index %zu.",
                    mSubHalIndex);
    }

    mHalProxy->postEventsToMessageQueue(processedEvents);
}

ScopedWakelock HalProxyCallback::createScopedWakelock(bool lock) {
    ScopedWakelock wakelock(mHalProxy, lock);
    return wakelock;
}

std::vector<Event> HalProxyCallback::processEvents(const std::vector<Event>& events,
                                                   size_t* numWakeupEvents) const {
    std::vector<Event> eventsOut;
    *numWakeupEvents = 0;
    for (Event event : events) {
        event.sensorHandle = setSubHalIndex(event.sensorHandle, mSubHalIndex);
        eventsOut.push_back(event);
        if ((mHalProxy->getSensorInfo(event.sensorHandle).flags & V1_0::SensorFlagBits::WAKE_UP) !=
            0) {
            (*numWakeupEvents)++;
        }
    }
    return eventsOut;
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace sensors
}  // namespace hardware
}  // namespace android