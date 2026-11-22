/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-23
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _H264_HIGH10_H_
#define _H264_HIGH10_H_

#include "media_codec.h"

namespace minirtc {

std::unique_ptr<MediaCodec> MakeHigh10Encoder(std::unique_ptr<MediaCodec> sdr,
                                              std::shared_ptr<SystemClock> clock,
                                              const MediaCodecConfig& config);
std::unique_ptr<MediaCodec> MakeHigh10Decoder(std::unique_ptr<MediaCodec> sdr,
                                              std::shared_ptr<SystemClock> clock,
                                              bool native_output);

}  // namespace minirtc

#endif
