/*
 * @Author: DI JUNKUN
 * @Date: 2024-03-04
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DAV1D_AV1_DECODER_H_
#define _DAV1D_AV1_DECODER_H_

#include <functional>

#include "dav1d/dav1d.h"
#include "media_codec.h"

namespace minirtc {

#if defined(_WIN32)
class NativeNv12FramePool;
#endif

class Dav1dAv1Decoder : public MediaCodec {
 public:
  Dav1dAv1Decoder(std::shared_ptr<SystemClock> clock,
                  bool native_video_output = false);
  virtual ~Dav1dAv1Decoder();

 public:
  int Init() override;

  int Decode(std::unique_ptr<ReceivedFrame> received_frame,
             std::function<void(const DecodedFrame*)> on_receive_decoded_frame)
      override;

  std::string GetDecoderName() const override { return "Dav1dAv1"; }

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  bool native_video_output_ = false;
#if defined(_WIN32)
  std::shared_ptr<NativeNv12FramePool> native_frame_pool_;
#endif
  DecodedFrame* decoded_frame_ = nullptr;
  unsigned char* nv12_frame_ = 0;
  size_t nv12_frame_capacity_ = 0;
  size_t nv12_frame_size_ = 0;

  uint32_t frame_width_ = 0;
  uint32_t frame_height_ = 0;

  FILE* file_av1_ = nullptr;
  FILE* file_nv12_ = nullptr;
  std::string av1_file_name_;
  std::string nv12_file_name_;

  bool first_ = false;

  // dav1d
  Dav1dContext* context_ = nullptr;
};
}  // namespace minirtc

#endif
