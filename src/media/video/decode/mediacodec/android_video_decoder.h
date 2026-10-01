/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-02
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ANDROID_VIDEO_DECODER_H_
#define _ANDROID_VIDEO_DECODER_H_

#include "android_media_codec.h"
#include "media_codec.h"

namespace minirtc {

class AndroidVideoDecoder final : public MediaCodec {
 public:
  AndroidVideoDecoder(std::shared_ptr<SystemClock> clock, VideoCodecType type,
                      bool native_video_output = false);
  int Init() override;
  int Decode(std::unique_ptr<ReceivedFrame> frame,
             std::function<void(const DecodedFrame*)> callback) override;
  int GetResolution(int* width, int* height) const override;
  std::string GetDecoderName() const override;

 private:
  int Fallback(const char* reason);
  std::shared_ptr<SystemClock> clock_;
  VideoCodecType type_;
  bool native_video_output_;
  mutable std::mutex mutex_;
  bool initialized_ = false;
  std::unique_ptr<MediaCodec> software_;
  // Destroy the hardware backend (and join its output thread) first.
  AndroidMediaCodec hardware_;
};

}  // namespace minirtc

#endif