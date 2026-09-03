/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-03
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _OPENH264_DECODER_H_
#define _OPENH264_DECODER_H_

#include <wels/codec_api.h>
#include <wels/codec_app_def.h>
#include <wels/codec_def.h>
#include <wels/codec_ver.h>

#include <functional>
#include <memory>
#include <vector>

#include "media_codec.h"

namespace minirtc {

#if defined(_WIN32)
class NativeNv12FramePool;
#endif

class OpenH264Decoder : public MediaCodec {
 public:
  OpenH264Decoder(std::shared_ptr<SystemClock> clock,
                  bool native_video_output = false);
  virtual ~OpenH264Decoder();

 public:
  int Init() override;

  int Decode(std::unique_ptr<ReceivedFrame> received_frame,
             std::function<void(const DecodedFrame*)> on_receive_decoded_frame)
      override;

  std::string GetDecoderName() const override { return "OpenH264"; }

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  bool native_video_output_ = false;
#if defined(_WIN32)
  std::shared_ptr<NativeNv12FramePool> native_frame_pool_;
#endif
  ISVCDecoder* openh264_decoder_ = nullptr;
  DecodedFrame* decoded_frame_ = nullptr;
  bool get_first_keyframe_ = false;
  bool skip_frame_ = false;
  FILE* file_nv12_ = nullptr;
  FILE* file_h264_ = nullptr;
  std::string h264_file_name_;
  std::string nv12_file_name_;
  uint32_t frame_width_ = 1280;
  uint32_t frame_height_ = 720;

  unsigned char* yuv420p_planes_[3] = {nullptr, nullptr, nullptr};
  std::vector<unsigned char> yuv420p_frame_;
  std::vector<unsigned char> nv12_frame_;
  size_t frame_size_ = 0;
};
}  // namespace minirtc

#endif
