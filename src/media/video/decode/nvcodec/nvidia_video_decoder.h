/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-03
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _NVIDIA_VIDEO_DECODER_H_
#define _NVIDIA_VIDEO_DECODER_H_

#include <functional>
#include <memory>

#include "NvDecoder.h"
#include "media_codec.h"
#include "nvcodec_api.h"
#include "nvcodec_common.h"

namespace minirtc {

struct NvidiaVideoDecoderState;

class NvidiaVideoDecoder : public MediaCodec {
 public:
  NvidiaVideoDecoder(std::shared_ptr<SystemClock> clock,
                     bool native_video_output = false);
  virtual ~NvidiaVideoDecoder();

 public:
  int Init() override;

  int Decode(std::unique_ptr<ReceivedFrame> received_frame,
             std::function<void(const DecodedFrame*)> on_receive_decoded_frame)
      override;

  std::string GetDecoderName() const override { return "NvidiaH264"; }

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  bool native_video_output_ = false;
  std::shared_ptr<NvidiaVideoDecoderState> state_;
  CUdevice cuda_device_ = 0;
  DecodedFrame* decoded_frame_ = nullptr;
  bool get_first_keyframe_ = false;
  bool skip_frame_ = false;
  uint32_t frame_width_ = 1280;
  uint32_t frame_height_ = 720;

  FILE* file_h264_ = nullptr;
  FILE* file_nv12_ = nullptr;
  std::string h264_file_name_;
  std::string nv12_file_name_;
};
}  // namespace minirtc

#endif