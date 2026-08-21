/*
 * @Author: DI JUNKUN
 * @Date: 2025-05-20
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_TOOLBOX_ENCODER_H_
#define _VIDEO_TOOLBOX_ENCODER_H_

#include <VideoToolbox/VideoToolbox.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "media_codec.h"

namespace minirtc {

class VideoToolboxEncoder : public MediaCodec {
 public:
  explicit VideoToolboxEncoder(std::shared_ptr<SystemClock> clock);
  virtual ~VideoToolboxEncoder();

  int Init(const MediaCodecConfig& config) override;

  int Encode(const RawFrame& raw_frame,
             std::function<int(const EncodedFrame&)> on_encoded_image) override;

  int ForceIdr() override;

  int SetTargetBitrate(int bitrate) override;

  bool SupportsDynamicEncodingSpeedPriority() const override { return true; }

  int SetPrioritizeEncodingSpeedOverQuality(
      bool prioritize_speed) override;

  int GetResolution(int* width, int* height) const override;

  std::string GetEncoderName() const override;

 private:
  static void OutputCallback(void* outputCallbackRefCon,
                             void* sourceFrameRefCon, OSStatus status,
                             VTEncodeInfoFlags infoFlags,
                             CMSampleBufferRef sampleBuffer);

 private:
  std::shared_ptr<SystemClock> clock_;
  VTCompressionSessionRef encoding_session_ = nullptr;
  std::function<int(const EncodedFrame&)> on_encoded_image_cb_;

  int frame_width_ = 2880;
  int frame_height_ = 1800;
  int max_fps_ = 60;
  int target_bitrate_ = MINIRTC_AVERAGE_BITRATE;
  int key_frame_interval_ = 3000;

  bool force_idr_ = false;
  int64_t frame_count_ = 0;
  unsigned int seq_ = 0;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace minirtc

#endif
