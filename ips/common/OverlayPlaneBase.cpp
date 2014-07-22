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

#include <math.h>

#include <Drm.h>
#include <Hwcomposer.h>

#include <common/OverlayPlaneBase.h>
#include <common/TTMBufferMapper.h>
#include <common/GrallocSubBuffer.h>

// FIXME: remove it
#include <OMX_IVCommon.h>

namespace android {
namespace intel {

OverlayPlaneBase::OverlayPlaneBase(int index, int disp)
    : DisplayPlane(index, PLANE_OVERLAY, disp),
      mTTMBuffers(),
      mBackBuffer(0),
      mWsbm(0),
      mPipeConfig(0)
{
    LOGV("OverlayPlaneBase");
}

OverlayPlaneBase::~OverlayPlaneBase()
{
    LOGV("~OverlayPlaneBase");
    deinitialize();
}

bool OverlayPlaneBase::initialize(uint32_t bufferCount)
{
    Drm *drm = Hwcomposer::getInstance().getDrm();
    LOGV("OverlayPlaneBase::initialize");

    if (!drm) {
        LOGE("OverlayPlaneBase::initialize: failed to get drm");
        return false;
    }

    // NOTE: use overlay's data buffer count for a overlay plane
    mTTMBuffers.setCapacity(overlayDataBufferCount);

    // init wsbm
    mWsbm = new Wsbm(drm->getDrmFd());
    if (!mWsbm || !mWsbm->initialize()) {
        LOGE("failed to create wsbm\n");
        goto init_err;
    }

    // create overlay back buffer
    mBackBuffer = createBackBuffer();
    if (!mBackBuffer) {
        LOGE("failed to create overlay back buffer\n");
        goto init_err;
    }

    // reset back buffer
    resetBackBuffer();

    if (!DisplayPlane::initialize(overlayDataBufferCount)) {
        LOGE("failed to initialize display plane");
        goto init_err;
    }
    return true;
init_err:
    deinitialize();
    return false;
}

void OverlayPlaneBase::deinitialize()
{
    DisplayPlane::deinitialize();

    // delete back buffer
    deleteBackBuffer();

    // delete wsbm
    if (mWsbm) {
        delete mWsbm;
        mWsbm = 0;
    }
}

void OverlayPlaneBase::invalidateBufferCache()
{
    BufferMapper* mapper;

    // clear plane buffer cache
    DisplayPlane::invalidateBufferCache();

    // clear TTM buffer cache
    for (size_t i = 0; i < mTTMBuffers.size(); i++) {
        mapper = mTTMBuffers.valueAt(i);
        // put it
        if (mapper)
            putTTMMapper(mapper);
    }
}

bool OverlayPlaneBase::assignToDevice(int disp)
{
    uint32_t pipeConfig = 0;

    LOGV("OverlayPlaneBase::assignToDevice: disp = %d", disp);

    if (!initCheck()) {
        LOGE("OverlayPlaneBase:setPosition: plane hasn't been initialized");
        return false;
    }

    switch (disp) {
    case DisplayDevice::DEVICE_EXTERNAL:
        pipeConfig = (0x2 << 6);
        break;
    case DisplayDevice::DEVICE_PRIMARY:
    default:
        pipeConfig = 0;
        break;
    }

    // if pipe switching happened, then disable overlay first
    if (mPipeConfig != pipeConfig)
        disable();

    mPipeConfig = pipeConfig;

    return true;
}

void OverlayPlaneBase::setZOrderConfig(ZOrderConfig& config)
{
    LOGV("OverlayPlaneBase::setZOrderConfig");
}

bool OverlayPlaneBase::reset()
{
    LOGV("OverlayPlaneBase::reset");

    if (!initCheck()) {
        LOGE("OverlayPlaneBase:setPosition: plane hasn't been initialized");
        return false;
    }

    // reset back buffer
    resetBackBuffer();

    // flush
    flush(FLUSH_NEEDED);
    return true;
}

bool OverlayPlaneBase::enable()
{
    LOGV("OverlayPlaneBase::enable");

    if (!initCheck()) {
        LOGE("OverlayPlaneBase:setPosition: plane hasn't been initialized");
        return false;
    }

    OverlayBackBufferBlk *backBuffer = mBackBuffer->buf;
    if (!backBuffer)
        return false;

    if (backBuffer->OCMD & 0x1)
        return true;

    backBuffer->OCMD |= 0x1;

    // flush
    flush(FLUSH_NEEDED);
    return true;
}

bool OverlayPlaneBase::disable()
{
    LOGD("OverlayPlaneBase::disable");

    if (!initCheck()) {
        LOGE("OverlayPlaneBase:setPosition: plane hasn't been initialized");
        return false;
    }

    OverlayBackBufferBlk *backBuffer = mBackBuffer->buf;
    if (!backBuffer)
        return false;

    if (!(backBuffer->OCMD & 0x1))
        return true;

    backBuffer->OCMD &= ~0x1;

    // flush
    flush(FLUSH_NEEDED | WAIT_VBLANK);
    return true;
}


bool OverlayPlaneBase::flush(uint32_t flags)
{
    Drm *drm = Hwcomposer::getInstance().getDrm();
    LOGV("OverlayPlaneBase::flush: flags = 0x%x", flags);

    if (!initCheck()) {
        LOGE("OverlayPlaneBase:flush: plane hasn't been initialized");
        return false;
    }

    if (!drm) {
        LOGE("OverlayPlaneBase:flush: failed to get drm");
        return false;
    }

    if (!flags && !(flags & FLUSH_NEEDED))
        return false;

    struct drm_psb_register_rw_arg arg;

    memset(&arg, 0, sizeof(struct drm_psb_register_rw_arg));
    arg.overlay_write_mask = 1;
    arg.overlay_read_mask = 0;
    arg.overlay.b_wms = 0;
    arg.overlay.b_wait_vblank = (flags & WAIT_VBLANK) ? 1 : 0;
    arg.overlay.OVADD = (mBackBuffer->gttOffsetInPage << 12);

    // pipe select
    arg.overlay.OVADD |= mPipeConfig;
    if (flags & UPDATE_COEF)
        arg.overlay.OVADD |= 1;

    // issue ioctl
    bool ret = drm->writeReadIoctl(DRM_PSB_REGISTER_RW, &arg, sizeof(arg));
    if (ret == false) {
        LOGW("%s: overlay update failed with error code %d\n",
             __func__, ret);
        return false;
    }

    return true;
}

OverlayBackBuffer* OverlayPlaneBase::createBackBuffer()
{
    int size;
    int alignment;
    void *wsbmBufferObject;
    void *virtAddr;
    uint32_t gttOffsetInPage;
    uint32_t handle;
    OverlayBackBuffer *backBuffer;
    bool ret;

    LOGV("OverlayPlaneBase::createBackBuffer");

    size = sizeof(OverlayBackBufferBlk);
    alignment = 64 * 1024;
    wsbmBufferObject = 0;
    ret = mWsbm->allocateTTMBuffer(size, alignment, &wsbmBufferObject);
    if (ret == false) {
        LOGE("OverlayPlaneBase::createBackBuffer: failed to allocate buffer");
        return 0;
    }

    virtAddr = mWsbm->getCPUAddress(wsbmBufferObject);
    gttOffsetInPage = mWsbm->getGttOffset(wsbmBufferObject);

    // create back buffer
    backBuffer = (OverlayBackBuffer *)malloc(sizeof(OverlayBackBuffer));
    if (!backBuffer) {
        LOGE("OverlayPlaneBase::createBackBuffer: failed to allocate back buffer");
        goto alloc_err;
    }

    backBuffer->buf = (OverlayBackBufferBlk *)virtAddr;
    backBuffer->gttOffsetInPage = gttOffsetInPage;
    backBuffer->bufObject = (uint32_t)wsbmBufferObject;

    LOGV("OverlayPlaneBase::createBackBuffer: created back buffer. cpu %p, gtt %d",
          virtAddr, gttOffsetInPage);

    return backBuffer;
alloc_err:
    mWsbm->destroyTTMBuffer(wsbmBufferObject);
    return 0;
}

void OverlayPlaneBase::deleteBackBuffer()
{
    void *wsbmBufferObject;
    bool ret;

    if (!mBackBuffer)
        return;

    wsbmBufferObject = (void *)mBackBuffer->bufObject;
    ret = mWsbm->destroyTTMBuffer(wsbmBufferObject);
    if (ret == false)
        LOGW("OverlayPlaneBase::deleteBackBuffer: failed to delete back buffer");

    // free back buffer
    free(mBackBuffer);
    mBackBuffer = 0;
}

void OverlayPlaneBase::resetBackBuffer()
{
    OverlayBackBufferBlk *backBuffer;

    LOGV("OverlayPlaneBase::resetBackBuffer");

    if (!mBackBuffer)
        return;

    if (!mBackBuffer->buf)
        return;

    backBuffer = mBackBuffer->buf;

    memset(backBuffer, 0, sizeof(OverlayBackBufferBlk));

    /*reset overlay*/
    backBuffer->OCLRC0 = (OVERLAY_INIT_CONTRAST << 18) |
                         (OVERLAY_INIT_BRIGHTNESS & 0xff);
    backBuffer->OCLRC1 = OVERLAY_INIT_SATURATION;
    backBuffer->DCLRKV = OVERLAY_INIT_COLORKEY;
    backBuffer->DCLRKM = OVERLAY_INIT_COLORKEYMASK;
    backBuffer->OCONFIG = 0;
    backBuffer->OCONFIG |= (0x1 << 3);
    backBuffer->OCONFIG |= (0x1 << 27);
    backBuffer->SCHRKEN &= ~(0x7 << 24);
    backBuffer->SCHRKEN |= 0xff;
}

BufferMapper* OverlayPlaneBase::getTTMMapper(BufferMapper& grallocMapper)
{
    struct VideoPayloadBuffer *payload;
    uint32_t khandle;
    uint32_t w, h;
    uint32_t yStride, uvStride;
    stride_t stride;
    int srcX, srcY, srcW, srcH;
    int tmp;

    DataBuffer *buf;
    ssize_t index;
    TTMBufferMapper *mapper;
    bool ret;

    payload = (struct VideoPayloadBuffer *)grallocMapper.getCpuAddress(SUB_BUFFER1);
    if (!payload) {
        LOGE("OverlayPlaneBase::getTTMMapper: invalid payload buffer");
        return 0;
    }

    // init ttm buffer
    khandle = payload->rotated_buffer_handle;
    index = mTTMBuffers.indexOfKey(khandle);
    if (index < 0) {
        LOGV("OverlayPlaneBase::getTTMMapper: unmapped TTM buffer, will map it");
        buf = new DataBuffer(khandle);
        if (!buf) {
            LOGE("OverlayPlaneBase::getTTMMapper: failed to create buffer");
            return 0;
        }

        w = payload->rotated_width;
        h = payload->rotated_height;
        srcX = grallocMapper.getCrop().x;
        srcY = grallocMapper.getCrop().y;
        srcW = grallocMapper.getCrop().w;
        srcH = grallocMapper.getCrop().h;

        if (mTransform == PLANE_TRANSFORM_90 || mTransform == PLANE_TRANSFORM_270) {
            tmp = srcH;
            srcH = srcW;
            srcW = tmp;

            tmp = srcX;
            srcX = srcY;
            srcY = tmp;
        }

        // skip pading bytes in rotate buffer
        switch(mTransform) {
        case PLANE_TRANSFORM_90:
            srcX += ((srcW + 0xf) & ~0xf) - srcW;
            break;
        case PLANE_TRANSFORM_180:
            srcX += ((srcW + 0xf) & ~0xf) - srcW;
            srcY += ((srcH + 0xf) & ~0xf) - srcH;
            break;
        case PLANE_TRANSFORM_270:
            srcY += ((srcH + 0xf) & ~0xf) - srcH;
            break;
        default:
            break;
        }

        // calculate stride
        switch (grallocMapper.getFormat()) {
        case HAL_PIXEL_FORMAT_YV12:
        case HAL_PIXEL_FORMAT_I420:
            yStride = align_to(align_to(w, 32), 64);
            uvStride = align_to(yStride >> 1, 64);
            stride.yuv.yStride = yStride;
            stride.yuv.uvStride = uvStride;
            break;
        case OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar:
            yStride = align_to(align_to(w, 32), 64);
            uvStride = yStride;
            stride.yuv.yStride = yStride;
            stride.yuv.uvStride = uvStride;
            break;
        case HAL_PIXEL_FORMAT_YUY2:
        case HAL_PIXEL_FORMAT_UYVY:
            yStride = align_to((align_to(w, 32) << 1), 64);
            uvStride = 0;
            stride.yuv.yStride = yStride;
            stride.yuv.uvStride = uvStride;
            break;
        }

        // update buffer
        buf->setStride(stride);
        buf->setWidth(w);
        buf->setHeight(h);
        buf->setCrop(srcX, srcY, srcW, srcH);
        buf->setFormat(grallocMapper.getFormat());

        // create buffer mapper
        mapper = new TTMBufferMapper(*mWsbm, *buf);
        if (!mapper) {
            LOGE("OverlayPlaneBase::getTTMMapper: failed to allocate mapper");
            goto mapper_err;
        }
        // map ttm buffer
        ret = mapper->map();
        if (!ret) {
            LOGE("OverlayPlaneBase::getTTMMapper: failed to map");
            goto map_err;
        }

        // add mapper
        index = mTTMBuffers.add(khandle, mapper);
        if (index < 0) {
            LOGE("OverlayPlaneBase::failed to add TTMMapper");
            goto add_err;
        }
    } else {
        LOGV("OverlayPlaneBase::getTTMMapper: got mapper in saved ttm buffers");
        mapper = reinterpret_cast<TTMBufferMapper *>(mTTMBuffers.valueAt(index));
    }

    // sync rotated data buffer.
    // Well, I have to, video driver DOESN'T support sync this buffer
    ret = mapper->waitIdle();
    if (ret == false)
        LOGW("OverlayPlaneBase::getTTMMapper: failed to wait idle");

    LOGV("OverlayPlaneBase::getTTMMapper: got ttm mapper");

    return mapper;
add_err:
    mapper->unmap();
map_err:
    delete mapper;
    return 0;
mapper_err:
    delete buf;
    return 0;
}

void OverlayPlaneBase::putTTMMapper(BufferMapper* mapper)
{
    if (!mapper)
        return;

    // unmap it
    mapper->unmap();

    // remove it from recorded TTM buffers
    mTTMBuffers.removeItem(mapper->getKey());

    // destroy this mapper
    delete mapper;
}

bool OverlayPlaneBase::rotatedBufferReady(BufferMapper& mapper)
{
    struct VideoPayloadBuffer *payload;
    uint32_t format;

    // only NV12_VED has rotated buffer
    format = mapper.getFormat();
    if (format != OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar)
        return false;

    payload = (struct VideoPayloadBuffer *)mapper.getCpuAddress(SUB_BUFFER1);
    // check payload
    if (!payload) {
        LOGE("OverlayPlaneBase::rotatedBufferReady: no payload found");
        return false;
    }

    if (payload->force_output_method == FORCE_OUTPUT_GPU)
        return false;

    if (payload->client_transform != mTransform) {
        LOGW("OverlayPlaneBase::rotatedBufferReady: client is not ready");
        return false;
    }

    return true;
}

void OverlayPlaneBase::checkPosition(int& x, int& y, int& w, int& h)
{
    int outputIndex = -1;
    struct Output *output;
    drmModeModeInfoPtr mode;
    drmModeCrtcPtr drmCrtc;
    Drm *drm = Hwcomposer::getInstance().getDrm();

    switch (mPipeConfig) {
    case 0:
        outputIndex = Drm::OUTPUT_PRIMARY;
        break;
    case (0x2 << 6) :
        outputIndex = Drm::OUTPUT_EXTERNAL;
        break;
    }

    if (outputIndex < 0)
        return;

    if (!drm) {
        LOGE("OverlayPlaneBase::checkPosition: failed to get drm");
        return;
    }

    // get output
    output = drm->getOutput(outputIndex);
    if (!output) {
        LOGE("checkPosition(): failed to get output");
        return;
    }

    drmCrtc = output->crtc;
    if (!drmCrtc)
        return;

    mode = &drmCrtc->mode;
    if (!drmCrtc->mode_valid || !mode->hdisplay || !mode->vdisplay)
        return;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if ((x + w) > mode->hdisplay)
        w = mode->hdisplay - x;
    if ((y + h) > mode->vdisplay)
        h = mode->vdisplay - y;
}

bool OverlayPlaneBase::bufferOffsetSetup(BufferMapper& mapper)
{
    LOGV("OverlayPlaneBase::bufferOffsetSetup");

    OverlayBackBufferBlk *backBuffer = mBackBuffer->buf;
    if (!backBuffer) {
        LOGE("OverlayPlaneBase::bufferOffsetSetup: invalid back buffer");
        return false;
    }

    uint32_t format = mapper.getFormat();
    uint32_t gttOffsetInBytes = (mapper.getGttOffsetInPage(0) << 12);
    uint32_t yStride = mapper.getStride().yuv.yStride;
    uint32_t uvStride = mapper.getStride().yuv.uvStride;
    uint32_t w = mapper.getWidth();
    uint32_t h = mapper.getHeight();
    uint32_t srcX= mapper.getCrop().x;
    uint32_t srcY= mapper.getCrop().y;

    // clear original format setting
    backBuffer->OCMD &= ~(0xf << 10);

    // Y/U/V plane must be 4k bytes aligned.
    backBuffer->OSTART_0Y = gttOffsetInBytes;
    backBuffer->OSTART_0U = gttOffsetInBytes;
    backBuffer->OSTART_0V = gttOffsetInBytes;

    backBuffer->OSTART_1Y = backBuffer->OSTART_0Y;
    backBuffer->OSTART_1U = backBuffer->OSTART_0U;
    backBuffer->OSTART_1V = backBuffer->OSTART_0V;

    switch(format) {
    case HAL_PIXEL_FORMAT_YV12:    /*YV12*/
        backBuffer->OBUF_0Y = 0;
        backBuffer->OBUF_0V = yStride * h;
        backBuffer->OBUF_0U = backBuffer->OBUF_0V + (uvStride * (h / 2));
        backBuffer->OCMD |= OVERLAY_FORMAT_PLANAR_YUV420;
        break;
    case HAL_PIXEL_FORMAT_I420:    /*I420*/
        backBuffer->OBUF_0Y = 0;
        backBuffer->OBUF_0U = yStride * h;
        backBuffer->OBUF_0V = backBuffer->OBUF_0U + (uvStride * (h / 2));
        backBuffer->OCMD |= OVERLAY_FORMAT_PLANAR_YUV420;
        break;
    /**
     * NOTE: this is the decoded video format, align the height to 32B
     * as it's defined by video driver
     */
    case OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar:    /*NV12*/
        backBuffer->OBUF_0Y = 0;
        backBuffer->OBUF_0U = yStride * align_to(h, 32);
        backBuffer->OBUF_0V = 0;
        backBuffer->OCMD |= OVERLAY_FORMAT_PLANAR_NV12_2;
        break;
    case HAL_PIXEL_FORMAT_YUY2:    /*YUY2*/
        backBuffer->OBUF_0Y = 0;
        backBuffer->OBUF_0U = 0;
        backBuffer->OBUF_0V = 0;
        backBuffer->OCMD |= OVERLAY_FORMAT_PACKED_YUV422;
        backBuffer->OCMD |= OVERLAY_PACKED_ORDER_YUY2;
        break;
    case HAL_PIXEL_FORMAT_UYVY:    /*UYVY*/
        backBuffer->OBUF_0Y = 0;
        backBuffer->OBUF_0U = 0;
        backBuffer->OBUF_0V = 0;
        backBuffer->OCMD |= OVERLAY_FORMAT_PACKED_YUV422;
        backBuffer->OCMD |= OVERLAY_PACKED_ORDER_UYVY;
        break;
    default:
        LOGE("%s: unsupported format %d\n", __func__, format);
        return false;
    }

    backBuffer->OBUF_0Y += srcY * yStride + srcX;
    backBuffer->OBUF_0V += (srcY / 2) * uvStride + srcX;
    backBuffer->OBUF_0U += (srcY / 2) * uvStride + srcX;
    backBuffer->OBUF_1Y = backBuffer->OBUF_0Y;
    backBuffer->OBUF_1U = backBuffer->OBUF_0U;
    backBuffer->OBUF_1V = backBuffer->OBUF_0V;

    LOGV("OverlayPlaneBase::bufferOffsetSetup: done. offset (%d, %d, %d)\n",
          backBuffer->OBUF_0Y,
          backBuffer->OBUF_0U,
          backBuffer->OBUF_0V);
    return true;
}

uint32_t OverlayPlaneBase::calculateSWidthSW(uint32_t offset, uint32_t width)
{
    LOGV("OverlayPlaneBase::calculateSWidthSW: offset %d, width %d\n",
          offset,
          width);

    uint32_t swidth = ((offset + width + 0x3F) >> 6) - (offset >> 6);

    swidth <<= 1;
    swidth -= 1;

    return swidth;
}

bool OverlayPlaneBase::coordinateSetup(BufferMapper& mapper)
{
    LOGV("OverlayPlaneBase::coordinateSetup");

    OverlayBackBufferBlk *backBuffer = mBackBuffer->buf;
    if (!backBuffer) {
        LOGE("OverlayPlaneBase::bufferOffsetSetup: invalid back buffer");
        return false;
    }

    uint32_t swidthy = 0;
    uint32_t swidthuv = 0;
    uint32_t format = mapper.getFormat();
    uint32_t width = mapper.getCrop().w;
    uint32_t height = mapper.getCrop().h;
    uint32_t yStride = mapper.getStride().yuv.yStride;
    uint32_t uvStride = mapper.getStride().yuv.uvStride;
    uint32_t offsety = backBuffer->OBUF_0Y;
    uint32_t offsetu = backBuffer->OBUF_0U;

    switch (format) {
    case HAL_PIXEL_FORMAT_YV12:              /*YV12*/
    case HAL_PIXEL_FORMAT_I420:              /*I420*/
    case OMX_INTEL_COLOR_FormatYUV420PackedSemiPlanar:          /*NV12*/
        break;
    case HAL_PIXEL_FORMAT_YUY2:              /*YUY2*/
    case HAL_PIXEL_FORMAT_UYVY:              /*UYVY*/
        width <<= 1;
        break;
    default:
        LOGE("OverlayPlaneBase::bufferOffsetSetup: unsupported format %d",
              format);
        return false;
    }

    if (width <= 0 || height <= 0) {
        LOGE("OverlayPlaneBase::bufferOffsetSetup: invalid src dim");
        return false;
    }

    if (yStride <=0 && uvStride <= 0) {
        LOGE("OverlayPlaneBase::bufferOffsetSetup: invalid source stride");
        return false;
    }

    backBuffer->SWIDTH = width | ((width / 2) << 16);
    swidthy = calculateSWidthSW(offsety, width);
    swidthuv = calculateSWidthSW(offsetu, width / 2);
    backBuffer->SWIDTHSW = (swidthy << 2) | (swidthuv << 18);
    backBuffer->SHEIGHT = height | ((height / 2) << 16);
    backBuffer->OSTRIDE = (yStride & (~0x3f)) | ((uvStride & (~0x3f)) << 16);

    LOGV("OverlayPlaneBase::coordinateSetup: finished");

    return true;
}

bool OverlayPlaneBase::setCoeffRegs(double *coeff, int mantSize,
                                  coeffPtr pCoeff, int pos)
{
    int maxVal, icoeff, res;
    int sign;
    double c;

    sign = 0;
    maxVal = 1 << mantSize;
    c = *coeff;
    if (c < 0.0) {
        sign = 1;
        c = -c;
    }

    res = 12 - mantSize;
    if ((icoeff = (int)(c * 4 * maxVal + 0.5)) < maxVal) {
        pCoeff[pos].exponent = 3;
        pCoeff[pos].mantissa = icoeff << res;
        *coeff = (double)icoeff / (double)(4 * maxVal);
    } else if ((icoeff = (int)(c * 2 * maxVal + 0.5)) < maxVal) {
        pCoeff[pos].exponent = 2;
        pCoeff[pos].mantissa = icoeff << res;
        *coeff = (double)icoeff / (double)(2 * maxVal);
    } else if ((icoeff = (int)(c * maxVal + 0.5)) < maxVal) {
        pCoeff[pos].exponent = 1;
        pCoeff[pos].mantissa = icoeff << res;
        *coeff = (double)icoeff / (double)(maxVal);
    } else if ((icoeff = (int)(c * maxVal * 0.5 + 0.5)) < maxVal) {
        pCoeff[pos].exponent = 0;
        pCoeff[pos].mantissa = icoeff << res;
        *coeff = (double)icoeff / (double)(maxVal / 2);
    } else {
        /* Coeff out of range */
        return false;
    }

    pCoeff[pos].sign = sign;
    if (sign)
        *coeff = -(*coeff);
    return true;
}

void OverlayPlaneBase::updateCoeff(int taps, double fCutoff,
                                 bool isHoriz, bool isY,
                                 coeffPtr pCoeff)
{
    int i, j, j1, num, pos, mantSize;
    double pi = 3.1415926535, val, sinc, window, sum;
    double rawCoeff[MAX_TAPS * 32], coeffs[N_PHASES][MAX_TAPS];
    double diff;
    int tapAdjust[MAX_TAPS], tap2Fix;
    bool isVertAndUV;

    if (isHoriz)
        mantSize = 7;
    else
        mantSize = 6;

    isVertAndUV = !isHoriz && !isY;
    num = taps * 16;
    for (i = 0; i < num  * 2; i++) {
        val = (1.0 / fCutoff) * taps * pi * (i - num) / (2 * num);
        if (val == 0.0)
            sinc = 1.0;
        else
            sinc = sin(val) / val;

        /* Hamming window */
        window = (0.54 - 0.46 * cos(2 * i * pi / (2 * num - 1)));
        rawCoeff[i] = sinc * window;
    }

    for (i = 0; i < N_PHASES; i++) {
        /* Normalise the coefficients. */
        sum = 0.0;
        for (j = 0; j < taps; j++) {
            pos = i + j * 32;
            sum += rawCoeff[pos];
        }
        for (j = 0; j < taps; j++) {
            pos = i + j * 32;
            coeffs[i][j] = rawCoeff[pos] / sum;
        }

        /* Set the register values. */
        for (j = 0; j < taps; j++) {
            pos = j + i * taps;
            if ((j == (taps - 1) / 2) && !isVertAndUV)
                setCoeffRegs(&coeffs[i][j], mantSize + 2, pCoeff, pos);
            else
                setCoeffRegs(&coeffs[i][j], mantSize, pCoeff, pos);
        }

        tapAdjust[0] = (taps - 1) / 2;
        for (j = 1, j1 = 1; j <= tapAdjust[0]; j++, j1++) {
            tapAdjust[j1] = tapAdjust[0] - j;
            tapAdjust[++j1] = tapAdjust[0] + j;
        }

        /* Adjust the coefficients. */
        sum = 0.0;
        for (j = 0; j < taps; j++)
            sum += coeffs[i][j];
        if (sum != 1.0) {
            for (j1 = 0; j1 < taps; j1++) {
                tap2Fix = tapAdjust[j1];
                diff = 1.0 - sum;
                coeffs[i][tap2Fix] += diff;
                pos = tap2Fix + i * taps;
                if ((tap2Fix == (taps - 1) / 2) && !isVertAndUV)
                    setCoeffRegs(&coeffs[i][tap2Fix], mantSize + 2, pCoeff, pos);
                else
                    setCoeffRegs(&coeffs[i][tap2Fix], mantSize, pCoeff, pos);

                sum = 0.0;
                for (j = 0; j < taps; j++)
                    sum += coeffs[i][j];
                if (sum == 1.0)
                    break;
            }
        }
    }
}

bool OverlayPlaneBase::scalingSetup(BufferMapper& mapper)
{
    int xscaleInt, xscaleFract, yscaleInt, yscaleFract;
    int xscaleIntUV, xscaleFractUV;
    int yscaleIntUV, yscaleFractUV;
    int deinterlace_factor = 1;
    /* UV is half the size of Y -- YUV420 */
    int uvratio = 2;
    uint32_t newval;
    coeffRec xcoeffY[N_HORIZ_Y_TAPS * N_PHASES];
    coeffRec xcoeffUV[N_HORIZ_UV_TAPS * N_PHASES];
    int i, j, pos;
    bool scaleChanged = false;
    int x, y, w, h;

    OverlayBackBufferBlk *backBuffer = mBackBuffer->buf;
    if (!backBuffer) {
        LOGE("OverlayPlaneBase::bufferOffsetSetup: invalid back buffer");
        return false;
    }

    x = mPosition.x;
    y = mPosition.y;
    w = mPosition.w;
    h = mPosition.h;

    // check position
    checkPosition(x, y, w, h);
    LOGV("OverlayPlaneBase::scalingSetup: Final position (%d, %d, %d, %d)",
          x, y, w, h);

    if ((w <= 0) || (h <= 0)) {
         LOGE("OverlayPlaneBase::scalingSetup: Invalid dst width/height");
         return false;
    }

    // setup dst position
    backBuffer->DWINPOS = (y << 16) | x;
    backBuffer->DWINSZ = (h << 16) | w;

    uint32_t srcWidth = mapper.getCrop().w;
    uint32_t srcHeight = mapper.getCrop().h;
    uint32_t dstWidth = w;
    uint32_t dstHeight = h;

    LOGV("OverlayPlaneBase::scalingSetup: src (%dx%d) v.s. (%dx%d)",
          srcWidth, srcHeight,
          dstWidth, dstHeight);
    /*
     * Y down-scale factor as a multiple of 4096.
     */
    if (srcWidth == dstWidth && srcHeight == dstHeight) {
        xscaleFract = (1 << 12);
        yscaleFract = (1 << 12)/deinterlace_factor;
    } else {
        xscaleFract = ((srcWidth - 1) << 12) / dstWidth;
        yscaleFract = ((srcHeight - 1) << 12) / (dstHeight * deinterlace_factor);
    }

    /* Calculate the UV scaling factor. */
    xscaleFractUV = xscaleFract / uvratio;
    yscaleFractUV = yscaleFract / uvratio;

    /*
     * To keep the relative Y and UV ratios exact, round the Y scales
     * to a multiple of the Y/UV ratio.
     */
    xscaleFract = xscaleFractUV * uvratio;
    yscaleFract = yscaleFractUV * uvratio;

    /* Integer (un-multiplied) values. */
    xscaleInt = xscaleFract >> 12;
    yscaleInt = yscaleFract >> 12;

    xscaleIntUV = xscaleFractUV >> 12;
    yscaleIntUV = yscaleFractUV >> 12;

    /* Check scaling ratio */
    if (xscaleInt > INTEL_OVERLAY_MAX_SCALING_RATIO) {
        LOGE("OverlayPlaneBase:scalingSetup:xscaleInt > %d",
              INTEL_OVERLAY_MAX_SCALING_RATIO);
        return false;
    }

    /* shouldn't get here */
    if (xscaleIntUV > INTEL_OVERLAY_MAX_SCALING_RATIO) {
        LOGE("OverlayPlaneBase:scalingSetup: xscaleIntUV > %d",
             INTEL_OVERLAY_MAX_SCALING_RATIO);
        return false;
    }

    newval = (xscaleInt << 15) |
    ((xscaleFract & 0xFFF) << 3) | ((yscaleFract & 0xFFF) << 20);
    if (newval != backBuffer->YRGBSCALE) {
        scaleChanged = true;
        backBuffer->YRGBSCALE = newval;
    }

    newval = (xscaleIntUV << 15) | ((xscaleFractUV & 0xFFF) << 3) |
    ((yscaleFractUV & 0xFFF) << 20);
    if (newval != backBuffer->UVSCALE) {
        scaleChanged = true;
        backBuffer->UVSCALE = newval;
    }

    newval = yscaleInt << 16 | yscaleIntUV;
    if (newval != backBuffer->UVSCALEV) {
        scaleChanged = true;
        backBuffer->UVSCALEV = newval;
    }

    /* Recalculate coefficients if the scaling changed. */
    /*
     * Only Horizontal coefficients so far.
     */
    if (scaleChanged) {
        double fCutoffY;
        double fCutoffUV;

        fCutoffY = xscaleFract / 4096.0;
        fCutoffUV = xscaleFractUV / 4096.0;

        /* Limit to between 1.0 and 3.0. */
        if (fCutoffY < MIN_CUTOFF_FREQ)
            fCutoffY = MIN_CUTOFF_FREQ;
        if (fCutoffY > MAX_CUTOFF_FREQ)
            fCutoffY = MAX_CUTOFF_FREQ;
        if (fCutoffUV < MIN_CUTOFF_FREQ)
            fCutoffUV = MIN_CUTOFF_FREQ;
        if (fCutoffUV > MAX_CUTOFF_FREQ)
            fCutoffUV = MAX_CUTOFF_FREQ;

        updateCoeff(N_HORIZ_Y_TAPS, fCutoffY, true, true, xcoeffY);
        updateCoeff(N_HORIZ_UV_TAPS, fCutoffUV, true, false, xcoeffUV);

        for (i = 0; i < N_PHASES; i++) {
            for (j = 0; j < N_HORIZ_Y_TAPS; j++) {
                pos = i * N_HORIZ_Y_TAPS + j;
                backBuffer->Y_HCOEFS[pos] =
                        (xcoeffY[pos].sign << 15 |
                         xcoeffY[pos].exponent << 12 |
                         xcoeffY[pos].mantissa);
            }
        }
        for (i = 0; i < N_PHASES; i++) {
            for (j = 0; j < N_HORIZ_UV_TAPS; j++) {
                pos = i * N_HORIZ_UV_TAPS + j;
                backBuffer->UV_HCOEFS[pos] =
                         (xcoeffUV[pos].sign << 15 |
                          xcoeffUV[pos].exponent << 12 |
                          xcoeffUV[pos].mantissa);
            }
        }
    }

    LOGV("OverlayPlaneBase::scalingSetup: finished");

    return true;
}

bool OverlayPlaneBase::setDataBuffer(BufferMapper& grallocMapper)
{
    BufferMapper *mapper;
    bool ret;

    LOGV("OverlayPlaneBase::setDataBuffer: handle = %d");

    if (!initCheck()) {
        LOGE("OverlayPlaneBase::setDataBuffer: overlay wasn't initialized");
        return false;
    }

    // get gralloc mapper
    mapper = &grallocMapper;
    // check transform when overlay is attached to primary device
    if (mTransform && !mPipeConfig) {
        if (!rotatedBufferReady(grallocMapper)) {
            LOGW("OverlayPlaneBase::setDataBuffer: rotated buffer is not ready");
            return false;
        }

        // get rotated data buffer mapper
        mapper = getTTMMapper(grallocMapper);
        if (!mapper) {
            LOGE("OverlayPlaneBase::setDataBuffer: failed to get rotated buffer");
            return false;
        }
    }

    OverlayBackBufferBlk *backBuffer = mBackBuffer->buf;
    if (!backBuffer) {
        LOGE("OverlayPlaneBase::bufferOffsetSetup: invalid back buffer");
        return false;
    }

    ret = bufferOffsetSetup(*mapper);
    if (ret == false) {
        LOGE("OverlayPlaneBase::setDataBuffer: failed to set up buffer offsets");
        return false;
    }

    ret = coordinateSetup(*mapper);
    if (ret == false) {
        LOGE("OverlayPlaneBase::setDataBuffer: failed to set up overlay coordinates");
        return false;
    }

    ret = scalingSetup(*mapper);
    if (ret == false) {
        LOGE("OverlayPlaneBase::setDataBuffer: failed to set up scaling parameters");
        return false;
    }

    backBuffer->OCMD |= 0x1;
    return true;
}

} // namespace intel
} // namespace android
