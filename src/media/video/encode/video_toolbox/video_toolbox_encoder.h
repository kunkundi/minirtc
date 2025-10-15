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

  int Init();

  int Encode(const RawFrame& raw_frame,
             std::function<int(const EncodedFrame&)> on_encoded_image);

  int ForceIdr();

  int SetTargetBitrate(int bitrate);

  int GetResolution(int* width, int* height);

  std::string GetEncoderName();

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
  int target_bitrate_ = 10000000;
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