/*
 * Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 * Adapted from WebRTC's common_video/h264/h264_bitstream_parser at
 * 17c6ed323acc09bd305318b557f9c50aa438a624. See
 * thirdparty/webrtc/LICENSE and thirdparty/webrtc/PATENTS.
 */

#ifndef MINIRTC_H264_BITSTREAM_PARSER_H_
#define MINIRTC_H264_BITSTREAM_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <optional>

namespace minirtc {

// Stateful H.264 Annex-B bitstream parser used to extract the base QP from
// the last VCL slice. SPS/PPS state is retained between frames.
class H264BitstreamParser {
 public:
  H264BitstreamParser() = default;

  void ParseBitstream(const uint8_t* bitstream, size_t length);
  std::optional<int> GetLastSliceQp() const;
  void Reset();

 private:
  enum class Result {
    kOk,
    kInvalidStream,
    kUnsupportedStream,
  };

  struct SpsState {
    uint32_t id = 0;
    uint32_t chroma_format_idc = 1;
    bool separate_colour_plane_flag = false;
    uint32_t log2_max_frame_num = 4;
    uint32_t pic_order_cnt_type = 0;
    uint32_t log2_max_pic_order_cnt_lsb = 4;
    bool delta_pic_order_always_zero_flag = false;
    bool frame_mbs_only_flag = true;
  };

  struct PpsState {
    uint32_t id = 0;
    uint32_t sps_id = 0;
    bool entropy_coding_mode_flag = false;
    bool bottom_field_pic_order_in_frame_present_flag = false;
    uint32_t num_ref_idx_l0_default_active_minus1 = 0;
    uint32_t num_ref_idx_l1_default_active_minus1 = 0;
    bool weighted_pred_flag = false;
    uint32_t weighted_bipred_idc = 0;
    int pic_init_qp_minus26 = 0;
    bool redundant_pic_cnt_present_flag = false;
  };

  void ParseNalu(const uint8_t* nalu, size_t length);
  std::optional<SpsState> ParseSps(const uint8_t* data, size_t length) const;
  std::optional<PpsState> ParsePps(const uint8_t* data, size_t length) const;
  Result ParseSlice(const uint8_t* data, size_t length, uint8_t nalu_type);

  std::optional<SpsState> sps_;
  std::optional<PpsState> pps_;
  std::optional<int32_t> last_slice_qp_delta_;
};

}  // namespace minirtc

#endif  // MINIRTC_H264_BITSTREAM_PARSER_H_
