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
#include <cutils/log.h>

#include <VsyncEventObserver.h>
#include <DisplayDevice.h>

namespace android {
namespace intel {

VsyncEventObserver::VsyncEventObserver(DisplayDevice& disp,
                                          IVsyncControl& vsync)
    : mDisplayDevice(disp),
      mVsync(vsync),
      mEnabled(0)
{
    LOGV("VsyncEventObserver()");
}

VsyncEventObserver::~VsyncEventObserver()
{

}

void VsyncEventObserver::control(int enabled)
{
    LOGV("control: enabled %s", enabled ? "True" : "False");

    Mutex::Autolock _l(mLock);
    mEnabled = enabled;
    mCondition.signal();
}

bool VsyncEventObserver::threadLoop()
{
    { // scope for lock
        Mutex::Autolock _l(mLock);
        while (!mEnabled) {
            mCondition.wait(mLock);
        }
    }

    int64_t timestamp;
    bool ret = mVsync.wait(mDisplayDevice.getType(), timestamp);

    if (ret == false) {
        LOGW("threadLoop: failed to wait for vsync, check vsync enabling...");
        return true;
    }

    // notify device
    mDisplayDevice.onVsync(timestamp);
    return true;
}

status_t VsyncEventObserver::readyToRun()
{
    LOGV("VsyncEventObserver: readyToRun. disp %d", mDisplayDevice.getType());
    return NO_ERROR;
}

void VsyncEventObserver::onFirstRef()
{
    LOGV("VsyncEventObserver: onFirstRef. disp %d", mDisplayDevice.getType());
    run("VsyncEventObserver", PRIORITY_URGENT_DISPLAY);
}

} // namespace intel
} // namesapce android