/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-06
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SVT_AV1_ENCODER_H_
#define _SVT_AV1_ENCODER_H_

#include <functional>
#include <vector>

#include "svt-av1/EbSvtAv1.h"
#include "svt-av1/EbSvtAv1Enc.h"
#include "svt-av1/EbSvtAv1Metadata.h"
#include "video_encoder.h"

namespace minirtc {

class SvtAv1Encoder : public MediaCodec {
 public:
  SvtAv1Encoder(std::shared_ptr<SystemClock> clock);
  ~SvtAv1Encoder();

  int Init(const MediaCodecConfig& config) override;
  int Encode(const RawFrame& raw_frame,
             std::function<int(const EncodedFrame& encoded_frame)>
                 on_encoded_image) override;
  int ForceIdr() override;
  int SetTargetBitrate(int bitrate) override;
  int GetResolution(int* width, int* height) const override;
  std::string GetEncoderName() const override;

  int ResetEncodeResolution(unsigned int width, unsigned int height);

 private:
  int Reconfigure(uint32_t frame_width, uint32_t frame_height);
  void Release();

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  EbComponentType* svt_av1_encoder_ = nullptr;
  EbSvtAv1EncConfiguration enc_config_ = {};
  EbBufferHeaderType* stream_header_buffer_ = nullptr;
  uint32_t frame_width_ = 1280;
  uint32_t frame_height_ = 720;
  int key_frame_interval_ = 3000;
  int target_bitrate_ = 1000;
  int max_bitrate_ = 2500000;
  int max_payload_size_ = 1400;
  int max_fps_ = 60;
  bool force_idr_ = false;
  unsigned int seq_ = 0;

  FILE* file_av1_ = nullptr;
  FILE* file_nv12_ = nullptr;
  std::string av1_file_name_;
  std::string nv12_file_name_;
  unsigned char* yuv420p_frame_ = nullptr;
  size_t yuv420p_frame_capacity_ = 0;
};
}  // namespace minirtc

#endif