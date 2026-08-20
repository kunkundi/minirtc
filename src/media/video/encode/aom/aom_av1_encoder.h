/*
 * @Author: DI JUNKUN
 * @Date: 2024-03-01
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _AOM_AV1_ENCODER_H_
#define _AOM_AV1_ENCODER_H_

#include <functional>
#include <vector>

#include "aom/aom_codec.h"
#include "aom/aom_encoder.h"
#include "aom/aomcx.h"
#include "media_codec.h"

namespace minirtc {

typedef struct {
  uint64_t obu_header_size;
  unsigned obu_type;
  uint64_t
      obu_size;  // leb128(), contains the size in bytes of the OBU not
                 // including the bytes within obu_header or the obu_size syntax
  int extension_flag;
  int has_size_field;

  // extension_flag == 1
  int temporal_id;
  int spatial_id;
} OBU_t;

class AomAv1Encoder : public MediaCodec {
 public:
  AomAv1Encoder(std::shared_ptr<SystemClock> clock);
  virtual ~AomAv1Encoder();

 public:
  int Init(const MediaCodecConfig& config) override;

  int Encode(const RawFrame& raw_frame,
             std::function<int(const EncodedFrame& encoded_frame)>
                 on_encoded_image) override;

  int ForceIdr() override;

  int SetTargetBitrate(int bitrate) override;

  int GetResolution(int* width, int* height) const override {
    if (seq_ == 0) {
      return -1;
    }

    *width = frame_width_;
    *height = frame_height_;
    return 0;
  }

  std::string GetEncoderName() const override { return "AomAV1"; }

 private:
  template <typename P>
  bool SetEncoderControlParameters(int param_id, P param_value);
  int NumberOfThreads(int width, int height, int number_of_cores);
  int GetCpuSpeed(int width, int height);

  int ResetEncodeResolution(unsigned int width, unsigned int height);

  int Release();

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  uint32_t frame_width_ = 1280;
  uint32_t frame_height_ = 720;
  int key_frame_interval_ = 3000;
  int target_bitrate_ = MINIRTC_AVERAGE_BITRATE / 1000;
  int max_bitrate_ = kDefaultMaxEncoderBitrateBps;
  int max_payload_size_ = 1150;
  int max_fps_ = 30;
  int number_of_cores_ = 4;

  std::vector<std::vector<uint8_t>> encoded_packets_;
  unsigned char* encoded_image_ = nullptr;
  FILE* file_av1_ = nullptr;
  FILE* file_nv12_ = nullptr;
  std::string av1_file_name_;
  std::string nv12_file_name_;
  unsigned char* nv12_data_ = nullptr;
  unsigned int seq_ = 0;

  // aom av1 encoder
  aom_image_t* frame_for_encode_ = nullptr;
  aom_codec_ctx_t aom_av1_encoder_ctx_;
  aom_codec_enc_cfg_t aom_av1_encoder_config_;
  bool disable_frame_dropping_ = true;
  bool inited_ = false;
  int64_t timestamp_ = 0;
  aom_enc_frame_flags_t force_i_frame_flags_ = 0;
  uint8_t* encoded_frame_ = nullptr;
  size_t encoded_frame_capacity_ = 0;
  size_t encoded_frame_size_ = 0;
  bool force_i_frame_ = false;
};
}  // namespace minirtc

#endif
