/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-22
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_DECODER_FACTORY_H_
#define _VIDEO_DECODER_FACTORY_H_

#include <memory>

#include "media_codec.h"

namespace minirtc {
class VideoDecoderFactory {
 public:
  VideoDecoderFactory();
  ~VideoDecoderFactory();

  static std::unique_ptr<MediaCodec> CreateVideoDecoder(
      std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
      VideoCodecType codec_type, bool native_video_output = false);

  // Use this entry point for sessions so initialization failures can advance
  // to the next backend. CreateVideoDecoder only selects an uninitialized one.
  static std::unique_ptr<MediaCodec> CreateInitializedVideoDecoder(
      std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
      VideoCodecType codec_type, bool native_video_output = false);

  static bool CheckIsHardwareAccelerationSupported(VideoCodecType codec_type);

 private:
  static std::unique_ptr<MediaCodec> TryInitDecoder(
      std::unique_ptr<MediaCodec> decoder);
};
}  // namespace minirtc

#endif
