/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-04
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _BITSTREAM_PARSER_H_
#define _BITSTREAM_PARSER_H_

#include <cstdint>

class BitstreamReader {
 public:
  BitstreamReader(const uint8_t* data, size_t size)
      : data_(data), size_(size), bit_offset_(0) {}

  uint32_t ReadBits(int n) {
    uint32_t result = 0;
    for (int i = 0; i < n; ++i) {
      if (bit_offset_ / 8 >= size_) break;
      result = (result << 1) |
               ((data_[bit_offset_ / 8] >> (7 - (bit_offset_ % 8))) & 1);
      ++bit_offset_;
    }
    return result;
  }

  uint32_t ReadUE() {
    int zeros = 0;
    while (ReadBits(1) == 0 && bit_offset_ / 8 < size_) ++zeros;
    return (1u << zeros) - 1 + ReadBits(zeros);
  }

  int32_t ReadSE() {
    uint32_t ueVal = ReadUE();
    return (ueVal & 1) ? (int32_t)((ueVal + 1) >> 1) : -(int32_t)(ueVal >> 1);
  }

  void SkipUE(int n) {
    while (n--) ReadUE();
  }
  void SkipSE(int n) {
    while (n--) ReadSE();
  }
  void SkipBits(int n) { bit_offset_ += n; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t bit_offset_;
};

bool ParseSPSResolution(const uint8_t* sps, size_t sps_size, int& width,
                        int& height) {
  BitstreamReader br(sps, sps_size);
  br.ReadBits(8);  // NAL header

  uint32_t profile_idc = br.ReadBits(8);
  br.ReadBits(8);  // constraint flags + reserved
  br.ReadBits(8);  // level_idc
  br.ReadUE();     // seq_parameter_set_id

  if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
      profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
      profile_idc == 86 || profile_idc == 118 || profile_idc == 128 ||
      profile_idc == 138 || profile_idc == 144) {
    uint32_t chroma_format_idc = br.ReadUE();
    if (chroma_format_idc == 3) br.ReadBits(1);
    br.SkipUE(2);          // bit_depth_luma_minus8, bit_depth_chroma_minus8
    br.ReadBits(1);        // qpprime_y_zero_transform_bypass_flag
    if (br.ReadBits(1)) {  // seq_scaling_matrix_present_flag
      for (int i = 0; i < 8; i++) {
        if (br.ReadBits(1)) { /* skip scaling list */
        }
      }
    }
  }

  br.SkipUE(1);  // log2_max_frame_num_minus4
  uint32_t pic_order_cnt_type = br.ReadUE();
  if (pic_order_cnt_type == 0) {
    br.SkipUE(1);  // log2_max_pic_order_cnt_lsb_minus4
  } else if (pic_order_cnt_type == 1) {
    br.ReadBits(1);
    br.SkipSE(2);
    uint32_t num_ref_frames_in_pic_order_cnt_cycle = br.ReadUE();
    br.SkipSE(num_ref_frames_in_pic_order_cnt_cycle);
  }

  br.SkipUE(1);    // max_num_ref_frames
  br.ReadBits(1);  // gaps_in_frame_num_value_allowed_flag
  uint32_t pic_width_in_mbs_minus1 = br.ReadUE();
  uint32_t pic_height_in_map_units_minus1 = br.ReadUE();
  uint32_t frame_mbs_only_flag = br.ReadBits(1);

  if (!frame_mbs_only_flag) br.ReadBits(1);
  br.ReadBits(1);  // direct_8x8_inference_flag

  uint32_t frame_cropping_flag = br.ReadBits(1);
  uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
  if (frame_cropping_flag) {
    crop_left = br.ReadUE();
    crop_right = br.ReadUE();
    crop_top = br.ReadUE();
    crop_bottom = br.ReadUE();
  }

  int width_in_mbs = pic_width_in_mbs_minus1 + 1;
  int height_in_map_units = pic_height_in_map_units_minus1 + 1;
  int frame_height = (2 - frame_mbs_only_flag) * height_in_map_units;

  width = width_in_mbs * 16 - (crop_left + crop_right) * 2;
  height = frame_height * 16 - (crop_top + crop_bottom) * 2;

  return true;
}

#endif