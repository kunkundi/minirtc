/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-22
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_ENCODER_FACTORY_H_
#define _VIDEO_ENCODER_FACTORY_H_

#include <memory>

#include "media_codec.h"

namespace minirtc {

class VideoEncoderFactory {
 public:
  VideoEncoderFactory();
  ~VideoEncoderFactory();

  // Select an uninitialized backend. Sessions should use the initialized
  // factory below so a failed Init() advances to the next backend.
  static std::unique_ptr<MediaCodec> CreateVideoEncoder(
      std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
      VideoCodecType codec_type);

  static std::unique_ptr<MediaCodec> CreateInitializedVideoEncoder(
      std::shared_ptr<SystemClock> clock, const MediaCodecConfig& config,
      bool hardware_acceleration, VideoCodecType codec_type);

#ifdef _WIN32
  // Continue with the negotiated codec when neither NVENC input path is usable.
  static std::unique_ptr<MediaCodec> CreateWindowsFallbackEncoder(
      std::shared_ptr<SystemClock> clock, const MediaCodecConfig& config,
      VideoCodecType codec_type);
#endif

  static bool CheckIsHardwareAccelerationSupported(VideoCodecType codec_type);

 private:
  static std::unique_ptr<MediaCodec> TryInitEncoder(
      std::unique_ptr<MediaCodec> encoder, const MediaCodecConfig& config);
};
}  // namespace minirtc

#endif
