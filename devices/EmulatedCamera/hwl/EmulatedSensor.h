/*
 * Copyright (C) 2012 The Android Open Source Project
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

/**
 * This class is a simple simulation of a typical CMOS cellphone imager chip,
 * which outputs 12-bit Bayer-mosaic raw images.
 *
 * Unlike most real image sensors, this one's native color space is linear sRGB.
 *
 * The sensor is abstracted as operating as a pipeline 3 stages deep;
 * conceptually, each frame to be captured goes through these three stages. The
 * processing step for the sensor is marked off by vertical sync signals, which
 * indicate the start of readout of the oldest frame. The interval between
 * processing steps depends on the frame duration of the frame currently being
 * captured. The stages are 1) configure, 2) capture, and 3) readout. During
 * configuration, the sensor's registers for settings such as exposure time,
 * frame duration, and gain are set for the next frame to be captured. In stage
 * 2, the image data for the frame is actually captured by the sensor. Finally,
 * in stage 3, the just-captured data is read out and sent to the rest of the
 * system.
 *
 * The sensor is assumed to be rolling-shutter, so low-numbered rows of the
 * sensor are exposed earlier in time than larger-numbered rows, with the time
 * offset between each row being equal to the row readout time.
 *
 * The characteristics of this sensor don't correspond to any actual sensor,
 * but are not far off typical sensors.
 *
 * Example timing diagram, with three frames:
 *  Frame 0-1: Frame duration 50 ms, exposure time 20 ms.
 *  Frame   2: Frame duration 75 ms, exposure time 65 ms.
 * Legend:
 *   C = update sensor registers for frame
 *   v = row in reset (vertical blanking interval)
 *   E = row capturing image data
 *   R = row being read out
 *   | = vertical sync signal
 *time(ms)|   0          55        105       155            230     270
 * Frame 0|   :configure : capture : readout :              :       :
 *  Row # | ..|CCCC______|_________|_________|              :       :
 *      0 |   :\          \vvvvvEEEER         \             :       :
 *    500 |   : \          \vvvvvEEEER         \            :       :
 *   1000 |   :  \          \vvvvvEEEER         \           :       :
 *   1500 |   :   \          \vvvvvEEEER         \          :       :
 *   2000 |   :    \__________\vvvvvEEEER_________\         :       :
 * Frame 1|   :           configure  capture      readout   :       :
 *  Row # |   :          |CCCC_____|_________|______________|       :
 *      0 |   :          :\         \vvvvvEEEER              \      :
 *    500 |   :          : \         \vvvvvEEEER              \     :
 *   1000 |   :          :  \         \vvvvvEEEER              \    :
 *   1500 |   :          :   \         \vvvvvEEEER              \   :
 *   2000 |   :          :    \_________\vvvvvEEEER______________\  :
 * Frame 2|   :          :          configure     capture    readout:
 *  Row # |   :          :         |CCCC_____|______________|_______|...
 *      0 |   :          :         :\         \vEEEEEEEEEEEEER       \
 *    500 |   :          :         : \         \vEEEEEEEEEEEEER       \
 *   1000 |   :          :         :  \         \vEEEEEEEEEEEEER       \
 *   1500 |   :          :         :   \         \vEEEEEEEEEEEEER       \
 *   2000 |   :          :         :    \_________\vEEEEEEEEEEEEER_______\
 */

#ifndef HW_EMULATOR_CAMERA2_SENSOR_H
#define HW_EMULATOR_CAMERA2_SENSOR_H

#include "utils/Mutex.h"
#include "utils/Thread.h"
#include "utils/Timers.h"

#include "Base.h"
#include "EmulatedScene.h"
#include "HandleImporter.h"

#include <hwl_types.h>

#include <functional>

namespace android {

using android::hardware::camera::common::V1_0::helper::HandleImporter;
using google_camera_hal::HwlPipelineCallback;
using google_camera_hal::HwlPipelineResult;

class EmulatedSensor : private Thread, public virtual RefBase {
public:
    EmulatedSensor();
    ~EmulatedSensor();

    struct SensorCharacteristics {
        size_t width, height;
        nsecs_t exposureTimeRange[2];
        nsecs_t frameDurationRange[2];
        int32_t sensitivityRange[2];
        camera_metadata_enum_android_sensor_info_color_filter_arrangement colorArangement;
        uint32_t maxRawValue;
        uint32_t blackLevelPattern[4];

        SensorCharacteristics() : width(0), height(0), exposureTimeRange{0},
                frameDurationRange{0}, sensitivityRange{0},
                colorArangement(ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB), maxRawValue(0),
                blackLevelPattern{0} {}
    };

    static bool areCharacteristicsSupported(const SensorCharacteristics& characteristics);

    /*
     * Power control
     */

    status_t startUp(SensorCharacteristics characteristics);
    status_t shutDown();

    /*
     * Settings control
     */

    struct SensorSettings {
        HwlPipelineCallback notifyCallback;
        uint32_t pipelineId;
        nsecs_t exposureTime, frameDuration;
        uint32_t gain;
        uint32_t frameNumber;
        SensorSettings () :
                notifyCallback {nullptr, nullptr},
                pipelineId(0),
                exposureTime(0),
                frameDuration(0),
                gain(0),
                frameNumber(0) {}

        SensorSettings (HwlPipelineCallback notifyCallback, uint32_t pipelineId,
                nsecs_t exposureTime, nsecs_t frameDuration, uint32_t gain,
                uint32_t frameNumber) : notifyCallback(notifyCallback),
                pipelineId(pipelineId), exposureTime(exposureTime), frameDuration(frameDuration),
                gain(gain), frameNumber(frameNumber) {}
    };

    void setCurrentRequest(SensorSettings settings, std::unique_ptr<HwlPipelineResult> result,
            std::unique_ptr<Buffers> outputBuffers);

    /*
     * Synchronizing with sensor operation (vertical sync)
     */

    // Wait until the sensor outputs its next vertical sync signal, meaning it
    // is starting readout of its latest frame of data. Returns true if vertical
    // sync is signaled, false if the wait timed out.
    bool waitForVSync(nsecs_t reltime);

    // Wait until a new frame has been read out, and then return the time
    // capture started.  May return immediately if a new frame has been pushed
    // since the last wait for a new frame. Returns true if new frame is
    // returned, false if timed out.
    bool waitForNewFrame(nsecs_t reltime, nsecs_t *captureTime);

    static const nsecs_t kSupportedExposureTimeRange[2];
    static const nsecs_t kSupportedFrameDurationRange[2];
    static const int32_t kSupportedSensitivityRange[2];
    static const uint8_t kSupportedColorFilterArrangement;
    static const uint32_t kDefaultMaxRawValue;
    static const uint32_t kDefaultBlackLevelPattern[4];

private:
    /**
     * Sensor characteristics
     */
    SensorCharacteristics mChars;

    float kBaseGainFactor;

    // While each row has to read out, reset, and then expose, the (reset +
    // expose) sequence can be overlapped by other row readouts, so the final
    // minimum frame duration is purely a function of row readout time, at least
    // if there's a reasonable number of rows.
    nsecs_t mRowReadoutTime;


    static const nsecs_t kMinVerticalBlank;

    // Sensor sensitivity, approximate

    static const float kSaturationVoltage;
    static const uint32_t kSaturationElectrons;
    static const float kVoltsPerLuxSecond;
    static const float kElectronsPerLuxSecond;

    static const float kReadNoiseStddevBeforeGain;  // In electrons
    static const float kReadNoiseStddevAfterGain;   // In raw digital units
    static const float kReadNoiseVarBeforeGain;
    static const float kReadNoiseVarAfterGain;

    static const int32_t kDefaultSensitivity;
    Mutex mControlMutex;  // Lock before accessing control parameters
    // Start of control parameters
    Condition mVSync;
    bool mGotVSync;
    SensorSettings mCurrentSettings;
    std::unique_ptr<HwlPipelineResult> mCurrentResult;
    std::unique_ptr<Buffers> mCurrentOutputBuffers;
    HandleImporter mImporter;

    // End of control parameters

    Mutex mReadoutMutex;  // Lock before accessing readout variables
    // Start of readout variables
    Condition mReadoutAvailable;
    Condition mReadoutComplete;
    Buffers *mCapturedBuffers;
    nsecs_t mCaptureTime;
    // End of readout variables

    // Time of sensor startup, used for simulation zero-time point
    nsecs_t mStartupTime;

    /**
     * Inherited Thread virtual overrides, and members only used by the
     * processing thread
     */
    virtual status_t readyToRun();

    virtual bool threadLoop();

    nsecs_t mNextCaptureTime;
    std::unique_ptr<Buffers> mNextCapturedBuffers;

    std::unique_ptr<EmulatedScene> mScene;

    void captureRaw(uint8_t *img, uint32_t gain, uint32_t stride);
    void captureRGBA(uint8_t *img, uint32_t gain, uint32_t stride);
    void captureRGB(uint8_t *img, uint32_t gain, uint32_t stride);
    void captureNV21(YCbCrPlanes yuvLayout, uint32_t gain);
    void captureDepth(uint8_t *img, uint32_t gain, uint32_t stride);
};

}  // namespace android

#endif  // HW_EMULATOR_CAMERA2_SENSOR_H