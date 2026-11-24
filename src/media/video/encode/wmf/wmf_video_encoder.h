/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WMF_VIDEO_ENCODER_H_
#define _WMF_VIDEO_ENCODER_H_

#include "media_codec.h"

namespace minirtc {

// Uses only hardware MFTs registered by the graphics driver. No vendor SDK or
// CUDA runtime is required. Input is packed NV12; output is Annex-B H.264/HEVC.
class WmfVideoEncoder final : public MediaCodec {
 public:
  WmfVideoEncoder(std::shared_ptr<SystemClock> clock, VideoCodecType codec);
  ~WmfVideoEncoder() override;
  static bool IsSupported(VideoCodecType codec);
  int Init(const MediaCodecConfig& config) override;
  int Encode(const RawFrame& frame,
             std::function<int(const EncodedFrame&)> callback) override;
  int ForceIdr() override;
  int SetTargetBitrate(int bitrate) override;
  int SetTargetFrameRate(int frame_rate) override;
  bool SupportsInputFrameRateUpdates() const override { return true; }
  int SetInputFrameRate(int frame_rate) override;
  int GetResolution(int* width, int* height) const override;
  std::string GetEncoderName() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace minirtc
#endif
