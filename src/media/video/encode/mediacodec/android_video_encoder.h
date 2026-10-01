/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-02
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ANDROID_VIDEO_ENCODER_H_
#define _ANDROID_VIDEO_ENCODER_H_

#include "android_media_codec.h"
#include "media_codec.h"

namespace minirtc {

class AndroidVideoEncoder final : public MediaCodec {
 public:
  AndroidVideoEncoder(std::shared_ptr<SystemClock> clock, VideoCodecType type);
  int Init(const MediaCodecConfig& config) override;
  int Encode(const RawFrame& frame,
             std::function<int(const EncodedFrame&)> callback) override;
  int ForceIdr() override;
  int SetTargetBitrate(int bitrate) override;
  int GetResolution(int* width, int* height) const override;
  std::string GetEncoderName() const override;

 private:
  int Fallback(const char* reason);
  std::shared_ptr<SystemClock> clock_;
  VideoCodecType type_;
  MediaCodecConfig config_;
  mutable std::mutex mutex_;
  bool initialized_ = false;
  std::unique_ptr<MediaCodec> software_;
  // Destroy the hardware backend (and join its output thread) first.
  AndroidMediaCodec hardware_;
};

}  // namespace minirtc

#endif