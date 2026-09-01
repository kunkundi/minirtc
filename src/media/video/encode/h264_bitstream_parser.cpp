/*
 * Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 * Adapted from WebRTC's common_video/h264/h264_bitstream_parser,
 * h264_common, sps_parser, and pps_parser at
 * 17c6ed323acc09bd305318b557f9c50aa438a624. The parsing flow and bounds
 * follow WebRTC while the implementation is self-contained and uses the
 * C++17 standard library. See thirdparty/webrtc/LICENSE and
 * thirdparty/webrtc/PATENTS.
 */

#include "h264_bitstream_parser.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <vector>

namespace minirtc {
namespace {

constexpr uint8_t kNaluTypeMask = 0x1f;
constexpr uint8_t kNaluSlice = 1;
constexpr uint8_t kNaluIdr = 5;
constexpr uint8_t kNaluSei = 6;
constexpr uint8_t kNaluSps = 7;
constexpr uint8_t kNaluPps = 8;
constexpr uint8_t kNaluAud = 9;
constexpr uint8_t kNaluFiller = 12;
constexpr uint8_t kNaluPrefix = 14;

constexpr uint32_t kSliceP = 0;
constexpr uint32_t kSliceB = 1;
constexpr uint32_t kSliceI = 2;
constexpr uint32_t kSliceSp = 3;
constexpr uint32_t kSliceSi = 4;

constexpr uint32_t kMaxSpsId = 31;
constexpr uint32_t kMaxPpsId = 255;
constexpr uint32_t kMaxReferenceIndex = 31;
constexpr uint32_t kMaxLog2Minus4 = 12;
constexpr uint32_t kMaxPicOrderCycleLength = 255;
constexpr int kMinPicInitQpMinus26 = -26;
constexpr int kMaxPicInitQpMinus26 = 25;
constexpr int kMaxAbsQpDelta = 51;
constexpr int kMinQp = 0;
constexpr int kMaxQp = 51;

class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size)
      : data_(data), size_bits_(size <= std::numeric_limits<size_t>::max() / 8
                                   ? size * 8
                                   : 0),
        ok_(data != nullptr || size == 0) {}

  bool Ok() const { return ok_; }

  bool ReadBit(bool* value) {
    uint32_t bit = 0;
    if (!ReadBits(1, &bit)) {
      return false;
    }
    *value = bit != 0;
    return true;
  }

  bool ReadBits(size_t count, uint32_t* value) {
    if (!value || count > 32 || !ok_ || count > BitsRemaining()) {
      ok_ = false;
      return false;
    }

    uint32_t result = 0;
    for (size_t i = 0; i < count; ++i) {
      result = static_cast<uint32_t>(
          (result << 1) |
          ((data_[bit_offset_ / 8] >> (7 - bit_offset_ % 8)) & 1));
      ++bit_offset_;
    }
    *value = result;
    return true;
  }

  bool SkipBits(size_t count) {
    if (!ok_ || count > BitsRemaining()) {
      ok_ = false;
      return false;
    }
    bit_offset_ += count;
    return true;
  }

  bool ReadUnsignedGolomb(uint32_t* value) {
    if (!value || !ok_) {
      ok_ = false;
      return false;
    }

    size_t leading_zeros = 0;
    bool bit = false;
    while (true) {
      if (!ReadBit(&bit)) {
        return false;
      }
      if (bit) {
        break;
      }
      ++leading_zeros;
      if (leading_zeros > 31) {
        ok_ = false;
        return false;
      }
    }

    uint32_t suffix = 0;
    if (leading_zeros > 0 && !ReadBits(leading_zeros, &suffix)) {
      return false;
    }
    const uint64_t code_num =
        ((uint64_t{1} << leading_zeros) - 1) + suffix;
    if (code_num > std::numeric_limits<uint32_t>::max()) {
      ok_ = false;
      return false;
    }
    *value = static_cast<uint32_t>(code_num);
    return true;
  }

  bool ReadSignedGolomb(int32_t* value) {
    uint32_t code_num = 0;
    if (!value || !ReadUnsignedGolomb(&code_num)) {
      return false;
    }

    const int64_t magnitude = (static_cast<int64_t>(code_num) + 1) / 2;
    const int64_t signed_value = code_num & 1 ? magnitude : -magnitude;
    if (signed_value < std::numeric_limits<int32_t>::min() ||
        signed_value > std::numeric_limits<int32_t>::max()) {
      ok_ = false;
      return false;
    }
    *value = static_cast<int32_t>(signed_value);
    return true;
  }

 private:
  size_t BitsRemaining() const {
    return bit_offset_ <= size_bits_ ? size_bits_ - bit_offset_ : 0;
  }

  const uint8_t* data_ = nullptr;
  size_t size_bits_ = 0;
  size_t bit_offset_ = 0;
  bool ok_ = true;
};

struct NaluIndex {
  size_t payload_offset = 0;
  size_t payload_size = 0;
};

bool FindStartCode(const uint8_t* data, size_t size, size_t from,
                   size_t* offset, size_t* length) {
  if (!data || !offset || !length || from >= size) {
    return false;
  }
  for (size_t i = from; i + 2 < size; ++i) {
    if (data[i] != 0 || data[i + 1] != 0) {
      continue;
    }
    if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
      *offset = i;
      *length = 4;
      return true;
    }
    if (data[i + 2] == 1) {
      *offset = i;
      *length = 3;
      return true;
    }
  }
  return false;
}

std::vector<NaluIndex> FindNaluIndices(const uint8_t* data, size_t size) {
  std::vector<NaluIndex> indices;
  size_t start_offset = 0;
  size_t start_size = 0;
  if (!FindStartCode(data, size, 0, &start_offset, &start_size)) {
    return indices;
  }

  while (true) {
    const size_t payload_offset = start_offset + start_size;
    size_t next_offset = 0;
    size_t next_size = 0;
    const bool has_next =
        payload_offset < size &&
        FindStartCode(data, size, payload_offset, &next_offset, &next_size);
    const size_t payload_end = has_next ? next_offset : size;
    if (payload_end > payload_offset) {
      indices.push_back({payload_offset, payload_end - payload_offset});
    }
    if (!has_next) {
      break;
    }
    start_offset = next_offset;
    start_size = next_size;
  }
  return indices;
}

std::vector<uint8_t> ParseRbsp(const uint8_t* data, size_t size) {
  std::vector<uint8_t> rbsp;
  rbsp.reserve(size);
  for (size_t i = 0; i < size;) {
    if (size - i >= 3 && data[i] == 0 && data[i + 1] == 0 &&
        data[i + 2] == 3) {
      rbsp.push_back(data[i++]);
      rbsp.push_back(data[i++]);
      ++i;
    } else {
      rbsp.push_back(data[i++]);
    }
  }
  return rbsp;
}

bool IsHighProfile(uint32_t profile_idc) {
  switch (profile_idc) {
    case 44:
    case 83:
    case 86:
    case 100:
    case 110:
    case 118:
    case 122:
    case 128:
    case 134:
    case 138:
    case 139:
    case 244:
      return true;
    default:
      return false;
  }
}

uint32_t CeilLog2(uint32_t value) {
  uint32_t bits = 0;
  uint32_t limit = 1;
  while (limit < value && bits < 32) {
    limit <<= 1;
    ++bits;
  }
  return bits;
}

}  // namespace

void H264BitstreamParser::Reset() {
  sps_.reset();
  pps_.reset();
  last_slice_qp_delta_.reset();
}

void H264BitstreamParser::ParseBitstream(const uint8_t* bitstream,
                                         size_t length) {
  last_slice_qp_delta_.reset();
  if (!bitstream || length == 0) {
    return;
  }
  for (const NaluIndex& index : FindNaluIndices(bitstream, length)) {
    ParseNalu(bitstream + index.payload_offset, index.payload_size);
  }
}

std::optional<int> H264BitstreamParser::GetLastSliceQp() const {
  if (!pps_ || !last_slice_qp_delta_) {
    return std::nullopt;
  }
  const int qp = 26 + pps_->pic_init_qp_minus26 + *last_slice_qp_delta_;
  if (qp < kMinQp || qp > kMaxQp) {
    return std::nullopt;
  }
  return qp;
}

void H264BitstreamParser::ParseNalu(const uint8_t* nalu, size_t length) {
  if (!nalu || length == 0) {
    return;
  }

  const uint8_t nalu_type = nalu[0] & kNaluTypeMask;
  switch (nalu_type) {
    case kNaluSps:
      sps_ = ParseSps(nalu + 1, length - 1);
      break;
    case kNaluPps:
      pps_ = ParsePps(nalu + 1, length - 1);
      break;
    case kNaluAud:
    case kNaluFiller:
    case kNaluSei:
    case kNaluPrefix:
      break;
    case kNaluSlice:
    case kNaluIdr:
      ParseSlice(nalu, length, nalu_type);
      break;
    default:
      break;
  }
}

std::optional<H264BitstreamParser::SpsState>
H264BitstreamParser::ParseSps(const uint8_t* data, size_t length) const {
  const std::vector<uint8_t> rbsp = ParseRbsp(data, length);
  BitReader reader(rbsp.data(), rbsp.size());
  SpsState sps;

  uint32_t profile_idc = 0;
  uint32_t value = 0;
  if (!reader.ReadBits(8, &profile_idc) || !reader.SkipBits(16) ||
      !reader.ReadUnsignedGolomb(&sps.id) || sps.id > kMaxSpsId) {
    return std::nullopt;
  }

  if (IsHighProfile(profile_idc)) {
    if (!reader.ReadUnsignedGolomb(&sps.chroma_format_idc) ||
        sps.chroma_format_idc > 3) {
      return std::nullopt;
    }
    if (sps.chroma_format_idc == 3 &&
        !reader.ReadBit(&sps.separate_colour_plane_flag)) {
      return std::nullopt;
    }
    if (!reader.ReadUnsignedGolomb(&value) ||
        !reader.ReadUnsignedGolomb(&value) || !reader.SkipBits(1)) {
      return std::nullopt;
    }

    bool scaling_matrix_present = false;
    if (!reader.ReadBit(&scaling_matrix_present)) {
      return std::nullopt;
    }
    if (scaling_matrix_present) {
      const int scaling_list_count = sps.chroma_format_idc == 3 ? 12 : 8;
      for (int i = 0; i < scaling_list_count; ++i) {
        bool scaling_list_present = false;
        if (!reader.ReadBit(&scaling_list_present)) {
          return std::nullopt;
        }
        if (!scaling_list_present) {
          continue;
        }
        int last_scale = 8;
        int next_scale = 8;
        const int scaling_list_size = i < 6 ? 16 : 64;
        for (int j = 0; j < scaling_list_size; ++j) {
          if (next_scale != 0) {
            int32_t delta_scale = 0;
            if (!reader.ReadSignedGolomb(&delta_scale) || delta_scale < -128 ||
                delta_scale > 127) {
              return std::nullopt;
            }
            next_scale = (last_scale + delta_scale + 256) % 256;
          }
          if (next_scale != 0) {
            last_scale = next_scale;
          }
        }
      }
    }
  }

  uint32_t log2_max_frame_num_minus4 = 0;
  if (!reader.ReadUnsignedGolomb(&log2_max_frame_num_minus4) ||
      log2_max_frame_num_minus4 > kMaxLog2Minus4) {
    return std::nullopt;
  }
  sps.log2_max_frame_num = log2_max_frame_num_minus4 + 4;

  if (!reader.ReadUnsignedGolomb(&sps.pic_order_cnt_type) ||
      sps.pic_order_cnt_type > 2) {
    return std::nullopt;
  }
  if (sps.pic_order_cnt_type == 0) {
    uint32_t log2_max_pic_order_cnt_lsb_minus4 = 0;
    if (!reader.ReadUnsignedGolomb(&log2_max_pic_order_cnt_lsb_minus4) ||
        log2_max_pic_order_cnt_lsb_minus4 > kMaxLog2Minus4) {
      return std::nullopt;
    }
    sps.log2_max_pic_order_cnt_lsb =
        log2_max_pic_order_cnt_lsb_minus4 + 4;
  } else if (sps.pic_order_cnt_type == 1) {
    if (!reader.ReadBit(&sps.delta_pic_order_always_zero_flag)) {
      return std::nullopt;
    }
    int32_t signed_value = 0;
    if (!reader.ReadSignedGolomb(&signed_value) ||
        !reader.ReadSignedGolomb(&signed_value)) {
      return std::nullopt;
    }
    uint32_t cycle_length = 0;
    if (!reader.ReadUnsignedGolomb(&cycle_length) ||
        cycle_length > kMaxPicOrderCycleLength) {
      return std::nullopt;
    }
    for (uint32_t i = 0; i < cycle_length; ++i) {
      if (!reader.ReadSignedGolomb(&signed_value)) {
        return std::nullopt;
      }
    }
  }

  if (!reader.ReadUnsignedGolomb(&value) ||  // max_num_ref_frames
      !reader.SkipBits(1) ||                 // gaps_in_frame_num_value_allowed
      !reader.ReadUnsignedGolomb(&value) ||  // pic_width_in_mbs_minus1
      !reader.ReadUnsignedGolomb(&value) ||  // pic_height_in_map_units_minus1
      !reader.ReadBit(&sps.frame_mbs_only_flag)) {
    return std::nullopt;
  }
  return sps;
}

std::optional<H264BitstreamParser::PpsState>
H264BitstreamParser::ParsePps(const uint8_t* data, size_t length) const {
  const std::vector<uint8_t> rbsp = ParseRbsp(data, length);
  BitReader reader(rbsp.data(), rbsp.size());
  PpsState pps;

  if (!reader.ReadUnsignedGolomb(&pps.id) ||
      !reader.ReadUnsignedGolomb(&pps.sps_id) || pps.id > kMaxPpsId ||
      pps.sps_id > kMaxSpsId ||
      !reader.ReadBit(&pps.entropy_coding_mode_flag) ||
      !reader.ReadBit(&pps.bottom_field_pic_order_in_frame_present_flag)) {
    return std::nullopt;
  }

  uint32_t num_slice_groups_minus1 = 0;
  if (!reader.ReadUnsignedGolomb(&num_slice_groups_minus1) ||
      num_slice_groups_minus1 > 7) {
    return std::nullopt;
  }
  if (num_slice_groups_minus1 > 0) {
    uint32_t map_type = 0;
    if (!reader.ReadUnsignedGolomb(&map_type) || map_type > 6) {
      return std::nullopt;
    }
    uint32_t value = 0;
    if (map_type == 0) {
      for (uint32_t i = 0; i <= num_slice_groups_minus1; ++i) {
        if (!reader.ReadUnsignedGolomb(&value)) {
          return std::nullopt;
        }
      }
    } else if (map_type == 2) {
      for (uint32_t i = 0; i < num_slice_groups_minus1; ++i) {
        if (!reader.ReadUnsignedGolomb(&value) ||
            !reader.ReadUnsignedGolomb(&value)) {
          return std::nullopt;
        }
      }
    } else if (map_type >= 3 && map_type <= 5) {
      if (!reader.SkipBits(1) || !reader.ReadUnsignedGolomb(&value)) {
        return std::nullopt;
      }
    } else if (map_type == 6) {
      uint32_t pic_size_in_map_units_minus1 = 0;
      if (!reader.ReadUnsignedGolomb(&pic_size_in_map_units_minus1)) {
        return std::nullopt;
      }
      const uint64_t pic_size =
          static_cast<uint64_t>(pic_size_in_map_units_minus1) + 1;
      const uint64_t bits_to_skip =
          pic_size * CeilLog2(num_slice_groups_minus1 + 1);
      if (bits_to_skip > std::numeric_limits<size_t>::max() ||
          !reader.SkipBits(static_cast<size_t>(bits_to_skip))) {
        return std::nullopt;
      }
    }
  }

  if (!reader.ReadUnsignedGolomb(
          &pps.num_ref_idx_l0_default_active_minus1) ||
      !reader.ReadUnsignedGolomb(
          &pps.num_ref_idx_l1_default_active_minus1) ||
      pps.num_ref_idx_l0_default_active_minus1 > kMaxReferenceIndex ||
      pps.num_ref_idx_l1_default_active_minus1 > kMaxReferenceIndex ||
      !reader.ReadBit(&pps.weighted_pred_flag) ||
      !reader.ReadBits(2, &pps.weighted_bipred_idc)) {
    return std::nullopt;
  }

  int32_t signed_value = 0;
  if (!reader.ReadSignedGolomb(&signed_value) ||
      signed_value < kMinPicInitQpMinus26 ||
      signed_value > kMaxPicInitQpMinus26) {
    return std::nullopt;
  }
  pps.pic_init_qp_minus26 = signed_value;
  if (!reader.ReadSignedGolomb(&signed_value) ||  // pic_init_qs_minus26
      !reader.ReadSignedGolomb(&signed_value) ||  // chroma_qp_index_offset
      !reader.SkipBits(2) ||
      !reader.ReadBit(&pps.redundant_pic_cnt_present_flag)) {
    return std::nullopt;
  }
  return pps;
}

H264BitstreamParser::Result H264BitstreamParser::ParseSlice(
    const uint8_t* data, size_t length, uint8_t nalu_type) {
  if (!sps_ || !pps_ || !data || length <= 1) {
    return Result::kInvalidStream;
  }
  last_slice_qp_delta_.reset();

  const std::vector<uint8_t> rbsp = ParseRbsp(data, length);
  BitReader reader(rbsp.data(), rbsp.size());
  if (!reader.SkipBits(8)) {
    return Result::kInvalidStream;
  }

  const bool is_idr = nalu_type == kNaluIdr;
  const uint8_t nal_ref_idc = (data[0] >> 5) & 0x03;
  uint32_t value = 0;
  uint32_t slice_type = 0;
  uint32_t pps_id = 0;
  if (!reader.ReadUnsignedGolomb(&value) ||  // first_mb_in_slice
      !reader.ReadUnsignedGolomb(&slice_type) ||
      !reader.ReadUnsignedGolomb(&pps_id) || pps_id != pps_->id ||
      pps_->sps_id != sps_->id) {
    return Result::kInvalidStream;
  }
  slice_type %= 5;

  if (sps_->separate_colour_plane_flag && !reader.SkipBits(2)) {
    return Result::kInvalidStream;
  }
  if (!reader.SkipBits(sps_->log2_max_frame_num)) {
    return Result::kInvalidStream;
  }

  bool field_pic_flag = false;
  if (!sps_->frame_mbs_only_flag) {
    if (!reader.ReadBit(&field_pic_flag)) {
      return Result::kInvalidStream;
    }
    if (field_pic_flag && !reader.SkipBits(1)) {
      return Result::kInvalidStream;
    }
  }
  if (is_idr && !reader.ReadUnsignedGolomb(&value)) {
    return Result::kInvalidStream;
  }

  int32_t signed_value = 0;
  if (sps_->pic_order_cnt_type == 0) {
    if (!reader.SkipBits(sps_->log2_max_pic_order_cnt_lsb)) {
      return Result::kInvalidStream;
    }
    if (pps_->bottom_field_pic_order_in_frame_present_flag &&
        !field_pic_flag && !reader.ReadSignedGolomb(&signed_value)) {
      return Result::kInvalidStream;
    }
  } else if (sps_->pic_order_cnt_type == 1 &&
             !sps_->delta_pic_order_always_zero_flag) {
    if (!reader.ReadSignedGolomb(&signed_value)) {
      return Result::kInvalidStream;
    }
    if (pps_->bottom_field_pic_order_in_frame_present_flag &&
        !field_pic_flag && !reader.ReadSignedGolomb(&signed_value)) {
      return Result::kInvalidStream;
    }
  }

  if (pps_->redundant_pic_cnt_present_flag &&
      !reader.ReadUnsignedGolomb(&value)) {
    return Result::kInvalidStream;
  }
  if (slice_type == kSliceB && !reader.SkipBits(1)) {
    return Result::kInvalidStream;
  }

  uint32_t num_ref_idx_l0_active_minus1 =
      pps_->num_ref_idx_l0_default_active_minus1;
  uint32_t num_ref_idx_l1_active_minus1 =
      pps_->num_ref_idx_l1_default_active_minus1;
  if (slice_type == kSliceP || slice_type == kSliceB ||
      slice_type == kSliceSp) {
    bool override_flag = false;
    if (!reader.ReadBit(&override_flag)) {
      return Result::kInvalidStream;
    }
    if (override_flag) {
      if (!reader.ReadUnsignedGolomb(&num_ref_idx_l0_active_minus1) ||
          num_ref_idx_l0_active_minus1 > kMaxReferenceIndex) {
        return Result::kInvalidStream;
      }
      if (slice_type == kSliceB &&
          (!reader.ReadUnsignedGolomb(&num_ref_idx_l1_active_minus1) ||
           num_ref_idx_l1_active_minus1 > kMaxReferenceIndex)) {
        return Result::kInvalidStream;
      }
    }
  }

  if (nalu_type == 20 || nalu_type == 21) {
    return Result::kUnsupportedStream;
  }

  if (slice_type != kSliceI && slice_type != kSliceSi) {
    bool modification_flag = false;
    if (!reader.ReadBit(&modification_flag)) {
      return Result::kInvalidStream;
    }
    if (modification_flag) {
      uint32_t modification_idc = 0;
      do {
        if (!reader.ReadUnsignedGolomb(&modification_idc)) {
          return Result::kInvalidStream;
        }
        if ((modification_idc == 0 || modification_idc == 1) &&
            !reader.ReadUnsignedGolomb(&value)) {
          return Result::kInvalidStream;
        }
        if (modification_idc == 2 &&
            !reader.ReadUnsignedGolomb(&value)) {
          return Result::kInvalidStream;
        }
        if (modification_idc > 3) {
          return Result::kInvalidStream;
        }
      } while (modification_idc != 3);
    }
  }
  if (slice_type == kSliceB) {
    bool modification_flag = false;
    if (!reader.ReadBit(&modification_flag)) {
      return Result::kInvalidStream;
    }
    if (modification_flag) {
      uint32_t modification_idc = 0;
      do {
        if (!reader.ReadUnsignedGolomb(&modification_idc)) {
          return Result::kInvalidStream;
        }
        if ((modification_idc == 0 || modification_idc == 1) &&
            !reader.ReadUnsignedGolomb(&value)) {
          return Result::kInvalidStream;
        }
        if (modification_idc == 2 &&
            !reader.ReadUnsignedGolomb(&value)) {
          return Result::kInvalidStream;
        }
        if (modification_idc > 3) {
          return Result::kInvalidStream;
        }
      } while (modification_idc != 3);
    }
  }

  const bool has_pred_weight_table =
      (pps_->weighted_pred_flag &&
       (slice_type == kSliceP || slice_type == kSliceSp)) ||
      (pps_->weighted_bipred_idc == 1 && slice_type == kSliceB);
  if (has_pred_weight_table) {
    if (!reader.ReadUnsignedGolomb(&value)) {  // luma_log2_weight_denom
      return Result::kInvalidStream;
    }
    const uint8_t chroma_array_type = sps_->separate_colour_plane_flag
                                          ? 0
                                          : sps_->chroma_format_idc;
    if (chroma_array_type != 0 &&
        !reader.ReadUnsignedGolomb(&value)) {  // chroma_log2_weight_denom
      return Result::kInvalidStream;
    }

    const auto skip_weight_table = [&](uint32_t max_index) {
      for (uint32_t i = 0; i <= max_index; ++i) {
        bool flag = false;
        if (!reader.ReadBit(&flag)) {
          return false;
        }
        if (flag && (!reader.ReadSignedGolomb(&signed_value) ||
                     !reader.ReadSignedGolomb(&signed_value))) {
          return false;
        }
        if (chroma_array_type != 0) {
          if (!reader.ReadBit(&flag)) {
            return false;
          }
          if (flag) {
            for (int component = 0; component < 2; ++component) {
              if (!reader.ReadSignedGolomb(&signed_value) ||
                  !reader.ReadSignedGolomb(&signed_value)) {
                return false;
              }
            }
          }
        }
      }
      return true;
    };

    if (!skip_weight_table(num_ref_idx_l0_active_minus1) ||
        (slice_type == kSliceB &&
         !skip_weight_table(num_ref_idx_l1_active_minus1))) {
      return Result::kInvalidStream;
    }
  }

  if (nal_ref_idc != 0) {
    if (is_idr) {
      if (!reader.SkipBits(2)) {
        return Result::kInvalidStream;
      }
    } else {
      bool adaptive_marking = false;
      if (!reader.ReadBit(&adaptive_marking)) {
        return Result::kInvalidStream;
      }
      if (adaptive_marking) {
        uint32_t operation = 0;
        do {
          if (!reader.ReadUnsignedGolomb(&operation) || operation > 6) {
            return Result::kInvalidStream;
          }
          if ((operation == 1 || operation == 3) &&
              !reader.ReadUnsignedGolomb(&value)) {
            return Result::kInvalidStream;
          }
          if (operation == 2 && !reader.ReadUnsignedGolomb(&value)) {
            return Result::kInvalidStream;
          }
          if ((operation == 3 || operation == 6) &&
              !reader.ReadUnsignedGolomb(&value)) {
            return Result::kInvalidStream;
          }
          if (operation == 4 && !reader.ReadUnsignedGolomb(&value)) {
            return Result::kInvalidStream;
          }
        } while (operation != 0);
      }
    }
  }

  if (pps_->entropy_coding_mode_flag && slice_type != kSliceI &&
      slice_type != kSliceSi && !reader.ReadUnsignedGolomb(&value)) {
    return Result::kInvalidStream;
  }

  int32_t slice_qp_delta = 0;
  if (!reader.ReadSignedGolomb(&slice_qp_delta) ||
      std::abs(slice_qp_delta) > kMaxAbsQpDelta) {
    return Result::kInvalidStream;
  }
  last_slice_qp_delta_ = slice_qp_delta;
  return Result::kOk;
}

}  // namespace minirtc
