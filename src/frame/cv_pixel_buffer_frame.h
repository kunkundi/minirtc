/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-28
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CV_PIXEL_BUFFER_FRAME_H_
#define _CV_PIXEL_BUFFER_FRAME_H_

#include <CoreVideo/CoreVideo.h>

#include "minirtc.h"
#include "sdr_color.h"

namespace minirtc {

SdrColor PixelColor(CVPixelBufferRef buffer);
int CopyPixelBufferToNv12(void* owner, uint8_t* destination, size_t destination_size);
// The caller owns the pixel buffer. Consumers retain the returned descriptor's owner.
MiniRtcNativeVideoFrame DescribePixelBuffer(CVPixelBufferRef pixel_buffer);

}  // namespace minirtc

#endif
