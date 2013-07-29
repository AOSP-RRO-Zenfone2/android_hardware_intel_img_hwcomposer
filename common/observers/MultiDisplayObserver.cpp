/*
 * Copyright © 2012 Intel Corporation
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jackie Li <yaodong.li@intel.com>
 *
 */
#ifdef TARGET_HAS_MULTIPLE_DISPLAY
#include <HwcTrace.h>
#include <binder/IServiceManager.h>
#include <Hwcomposer.h>
#include <DisplayAnalyzer.h>
#include <ExternalDevice.h>
#endif

#include <MultiDisplayObserver.h>

namespace android {
namespace intel {

#ifdef TARGET_HAS_MULTIPLE_DISPLAY

MultiDisplayCallback::MultiDisplayCallback(MultiDisplayObserver *dispObserver)
    : mDispObserver(dispObserver),
      mPhoneState(MDS_PHONE_STATE_OFF),
      mVideoState(MDS_VIDEO_STATE_UNKNOWN)
{
}

MultiDisplayCallback::~MultiDisplayCallback()
{
    CTRACE();
    mDispObserver = NULL;
}

status_t MultiDisplayCallback::setPhoneState(MDS_PHONE_STATE state)
{
    mPhoneState = state;
    ITRACE("state: %d", state);
    mDispObserver->setPhoneState(state);
    return NO_ERROR;
}

status_t MultiDisplayCallback::setVideoState(int sessionNum, int sessionId, MDS_VIDEO_STATE state)
{
    mVideoState = state;
    // TODO: check why setVideoState is called multiple times during startup
    ITRACE("state: %d", state);
    mDispObserver->setVideoState(sessionNum, sessionId, state);
    return NO_ERROR;
}

status_t MultiDisplayCallback::setDisplayTiming(
        MDS_DISPLAY_ID dpyId, MDSDisplayTiming *timing)
{
    mDispObserver->setDisplayTiming(dpyId, timing);
    return NO_ERROR;
}

status_t MultiDisplayCallback::setDisplayState(
        MDS_DISPLAY_ID dpyId, MDS_DISPLAY_STATE state)
{
    ITRACE("id: %d state: %d", dpyId, state);
    return NO_ERROR;
}

status_t MultiDisplayCallback::setScalingType(
        MDS_DISPLAY_ID dpyId, MDS_SCALING_TYPE type)
{
    ITRACE("id: %d state: %d", dpyId, type);
    // Merrifield doesn't implement this API
    return INVALID_OPERATION;
}

status_t MultiDisplayCallback::setOverscan(
        MDS_DISPLAY_ID dpyId, int hValue, int vValue)
{
    ITRACE("id: %d h: %d v: %d", dpyId, hValue, vValue);
    // Merrifield doesn't implement this API
    return INVALID_OPERATION;
}

MultiDisplayObserver::MultiDisplayObserver()
    : mMDSClient(NULL),
      mMDSCallback(),
      mLock(),
      mCondition(),
      mThreadLoopCount(0),
      mDeviceConnected(false),
      mInitialized(false)
{
    CTRACE();
}

MultiDisplayObserver::~MultiDisplayObserver()
{
    WARN_IF_NOT_DEINIT();
}

bool MultiDisplayObserver::isMDSRunning()
{
    // Check if Multi Display service is running
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == NULL) {
        ETRACE("fail to get service manager!");
        return false;
    }

    sp<IBinder> service = sm->checkService(String16("MultiDisplay"));
    if (service == NULL) {
        VTRACE("fail to get MultiDisplay service!");
        return false;
    }

    return true;
}

bool MultiDisplayObserver::initMDSClient()
{
    mMDSClient = new MultiDisplayClient();
    if (mMDSClient == NULL) {
        ETRACE("failed to create MultiDisplayClient");
        return false;
    }

    mMDSCallback = new MultiDisplayCallback(this);
    if (mMDSCallback.get() == NULL) {
        ETRACE("failed to create MultiDisplayCallback");
        deinitMDSClient();
        return false;
    }

    status_t ret = mMDSClient->registerCallback(mMDSCallback);
    if (ret != NO_ERROR) {
        ETRACE("failed to register callback");
        deinitMDSClient();
        return false;
    }

    Drm *drm = Hwcomposer::getInstance().getDrm();
    mDeviceConnected = drm->isConnected(IDisplayDevice::DEVICE_EXTERNAL);
    ITRACE("MDS client is initialized");
    return true;
}

void MultiDisplayObserver::deinitMDSClient()
{
    if (mMDSCallback.get() && mMDSClient) {
        mMDSClient->unregisterCallback();
    }

    if (mMDSClient) {
        delete mMDSClient;
        mMDSClient = NULL;
    }

    mDeviceConnected = false;
    mMDSCallback = NULL;
}

bool MultiDisplayObserver::initMDSClientAsync()
{
    if (mThread.get()) {
        WTRACE("working thread has been already created.");
        return true;
    }

    mThread = new MDSClientInitThread(this);
    if (mThread.get() == NULL) {
        ETRACE("failed to create MDS client init thread");
        return false;
    }
    mThreadLoopCount = 0;
    // TODO: check return value
    mThread->run("MDSClientInitThread", PRIORITY_URGENT_DISPLAY);
    return true;
}

bool MultiDisplayObserver::initialize()
{
    bool ret = true;
    Mutex::Autolock _l(mLock);

    if (mInitialized) {
        WTRACE("display observer has been initialized");
        return true;
    }

    // initialize MDS client once. This should succeed if MDS service starts
    // before surfaceflinger service is started.
    // if surface flinger runs first, MDS client will be initialized asynchronously in
    // a working thread
    if (isMDSRunning()) {
        if (!initMDSClient()) {
            ETRACE("failed to initialize MDS client");
            // FIXME: NOT a common case for system server crash.
            // Start a working thread to initialize MDS client if exception happens
            ret = initMDSClientAsync();
        }
    } else {
        ret = initMDSClientAsync();
    }

    mInitialized = true;
    return ret;
}

void MultiDisplayObserver::deinitialize()
{
    sp<MDSClientInitThread> detachedThread;
    do {
        Mutex::Autolock _l(mLock);

        if (mThread.get()) {
            mCondition.signal();
            detachedThread = mThread;
            mThread = NULL;
        }
        mThreadLoopCount = 0;
        deinitMDSClient();
        mInitialized = false;
    } while (0);

    if (detachedThread.get()) {
        detachedThread->requestExitAndWait();
        detachedThread = NULL;
    }
}

status_t MultiDisplayObserver::notifyHotPlug(int disp, bool connected)
{
    Mutex::Autolock _l(mLock);
    if (!mMDSClient) {
        return NO_INIT;
    }

    if (connected == mDeviceConnected)
        return NO_ERROR;

    mDeviceConnected = connected;
    return mMDSClient->notifyHotPlug((MDS_DISPLAY_ID)disp, connected);
}

bool MultiDisplayObserver::threadLoop()
{
    Mutex::Autolock _l(mLock);

    // try to create MDS client in the working thread
    // multiple delayed attempts are made until MDS service starts.

    // Return false if MDS service is running or loop limit is reached
    // such that thread becomes inactive.
    if (isMDSRunning()) {
        if (!initMDSClient()) {
            ETRACE("failed to initialize MDS client");
        }
        return false;
    }

    if (mThreadLoopCount++ > THREAD_LOOP_BOUND) {
        ETRACE("failed to initialize MDS client, loop limit reached");
        return false;
    }

    status_t err = mCondition.waitRelative(mLock, milliseconds(THREAD_LOOP_DELAY));
    if (err != -ETIMEDOUT) {
        ITRACE("thread is interrupted");
        return false;
    }

    return true; // keep trying
}


status_t MultiDisplayObserver::setPhoneState(MDS_PHONE_STATE state)
{
    // blank secondary display if phone call is active
    bool blank = (state == MDS_PHONE_STATE_ON);
    Hwcomposer::getInstance().getDisplayAnalyzer()->postBlankEvent(blank);
    return 0;
}

status_t MultiDisplayObserver::setVideoState(int sessionNum, int sessionId, MDS_VIDEO_STATE state)
{
    bool preparing = (state == MDS_VIDEO_PREPARING || state == MDS_VIDEO_UNPREPARING);
    bool playing = (state != MDS_VIDEO_UNPREPARED);
    ITRACE("video state: preparing %d, playing %d", preparing, playing);
    Hwcomposer::getInstance().getDisplayAnalyzer()->postVideoEvent(
        sessionNum, sessionId, preparing, playing);
    return 0;
}

status_t MultiDisplayObserver::getVideoSourceInfo(int sessionID, MDSVideoSourceInfo* info)
{
    Mutex::Autolock _l(mLock);
    if (!mMDSClient) {
        return NO_INIT;
    }
    if (info == NULL)
        return UNKNOWN_ERROR;
    status_t ret = mMDSClient->getVideoSourceInfo(sessionID, info);
    if (ret == NO_ERROR)
        ITRACE("Video Session[%d] source info: %dx%d@%d", sessionID,
                info->displayW, info->displayH, info->frameRate);
    return ret;
}

status_t MultiDisplayObserver::setDisplayTiming(
        MDS_DISPLAY_ID dpyId, MDSDisplayTiming *timing)
{
    if ((int)dpyId != (int)IDisplayDevice::DEVICE_EXTERNAL) {
        ETRACE("invalid display id %d", dpyId);
        return INVALID_OPERATION;
    }

    drmModeModeInfo mode;
    mode.hdisplay = timing->width;
    mode.vdisplay = timing->height;
    mode.vrefresh = timing->refresh;
    // TODO: interlace needs to be replaced by flags
    mode.flags = timing->interlace ? DRM_MODE_FLAG_INTERLACE : 0;
    ITRACE("timing to set: %dx%d@%dHz", timing->width, timing->height, timing->refresh);
    ExternalDevice *dev =
        (ExternalDevice *)Hwcomposer::getInstance().getDisplayDevice(dpyId);
    if (dev) {
        dev->setDrmMode(mode);
    }
    return 0;
}

#endif //TARGET_HAS_MULTIPLE_DISPLAY

} // namespace intel
} // namespace android
