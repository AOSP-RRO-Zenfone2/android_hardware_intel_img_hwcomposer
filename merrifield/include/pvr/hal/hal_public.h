/* Copyright (c) Imagination Technologies Ltd.
 *
 * The contents of this file are subject to the MIT license as set out below.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef __HAL_PUBLIC_H
#define __HAL_PUBLIC_H

#define PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC

#include "img_gralloc_public.h"

#undef HAL_PIXEL_FORMAT_NV12

#ifdef ASUS_ZENFONE2_LP_BLOBS

typedef struct IMG_gralloc_module_t
{
        gralloc_module_t base;
        void *unused_psDisplayDevice;
        const void *(*unused_GetBufferFormats)(void);
        int (*unused_Blit)(struct IMG_gralloc_module_t const *module,
                                 buffer_handle_t src, buffer_handle_t dest,
                                 int w, int h, int x, int y,
                                 int transform,
                                 int async);

        int (*unused_Blit3)(struct IMG_gralloc_module_t const *module,
                                 unsigned long long ui64SrcStamp, int iSrcWidth,
                                 int iSrcHeight, int iSrcFormat, int eSrcRotation,
                                 buffer_handle_t dest, int eDestRotation);

#if defined(SUPPORT_ANDROID_MEMTRACK_HAL)
        int (*unused_GetMemTrackRecords)(struct IMG_gralloc_module_t const *module,
                                                          void **ppsRecords,
                                                          size_t *puNumRecords);
#endif /* defined(SUPPORT_ANDROID_MEMTRACK_HAL) */

        const void *(*unused_GetBufferFormat)(int iFormat);
        int (*GetBufferCPUAddresses)(gralloc_module_t const *module,
                                buffer_handle_t handle,
                                void **virt, uint32_t *size);
        int (*PutBufferCPUAddresses)(gralloc_module_t const *module,
                        buffer_handle_t handle);
        void *(*GetDisplayDevice)(struct IMG_gralloc_module_t *module);
}
IMG_gralloc_module_t;

#else

typedef struct _IMG_gralloc_module_
{
	IMG_gralloc_module_public_t base;
	void *(*GetDisplayDevice)(struct _IMG_gralloc_module_ *psGrallocModule);
	int (*GetDisplayStatus)(struct _IMG_gralloc_module_ *psGrallocModule,
							buffer_handle_t handle, uint32_t *pui32Status);
	int (*GetBufferCPUAddresses)(gralloc_module_t const* module,
								 buffer_handle_t buffer,
								 void **ppvCpuVirtAddr, size_t *puSize);
	int (*PutBufferCPUAddresses)(gralloc_module_t const* module,
								 buffer_handle_t buffer);
}
IMG_gralloc_module_t;

#endif

#define HAL_PIXEL_FORMAT_UYVY         0x107
#define HAL_PIXEL_FORMAT_INTEL_ZSL    0x109
#define HAL_PIXEL_FORMAT_NV12         0x3231564E
#define HAL_PIXEL_FORMAT_NV21         0x3132564E
#define HAL_PIXEL_FORMAT_I420         0x30323449
#define HAL_PIXEL_FORMAT_YUY2         0x32595559
#define HAL_PIXEL_FORMAT_NV12_VED     0x7FA00E00
#define HAL_PIXEL_FORMAT_NV12_VEDT    0x7FA00F00

#define GRALLOC_MODULE_GET_DISPLAY_STATUS_IMG 1001

#endif /* __HAL_PUBLIC_H */
