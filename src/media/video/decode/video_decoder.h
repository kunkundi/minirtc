/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-24
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_DECODER_H_
#define _VIDEO_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "clock/system_clock.h"
#include "decoded_frame.h"
#include "media_codec.h"
#include "received_frame.h"

namespace minirtc {
class VideoDecoder : public MediaCodec {
 public:
  virtual int Init(const MediaCodecConfig& config) = 0;

  virtual int Decode(
      std::unique_ptr<ReceivedFrame> received_frame,
      std::function<void(const DecodedFrame*)> on_receive_decoded_frame) = 0;

  virtual std::string GetDecoderName() = 0;

  VideoDecoder() = default;
  virtual ~VideoDecoder() {}
};
}  // namespace minirtc

#endif