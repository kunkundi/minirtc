#ifndef _RTP_FEC_H_
#define _RTP_FEC_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rtp_packet.h"

namespace minirtc {

struct FecConfig {
  bool enabled = false;
  bool protect_keyframes_only = true;
  double repair_ratio = 0.5;
  uint16_t max_source_symbols_per_block = 128;
  uint16_t symbol_size = 1200;
};

enum H264FecFlags : uint8_t {
  kH264FecFlagRepair = 1 << 0,
  kH264FecFlagKeyFrame = 1 << 1,
};

struct H264FecHeader {
  uint8_t version = 1;
  uint8_t flags = 0;
  uint16_t block_index = 0;
  uint16_t block_count = 1;
  uint16_t source_symbol_count = 0;
  uint16_t repair_symbol_count = 0;
  uint16_t symbol_size = 0;
  uint16_t symbol_id = 0;
  uint32_t frame_id = 0;
  uint32_t rtp_timestamp = 0;
  uint32_t original_frame_size = 0;
  uint32_t block_original_size = 0;
};

struct RtpFecPacket {
  H264FecHeader header;
  std::vector<uint8_t> symbol;
};

constexpr uint8_t kH264FecVersion = 1;
constexpr size_t kH264FecHeaderSize = 30;

std::vector<uint8_t> SerializeH264FecPayload(
    const H264FecHeader& header, const std::vector<uint8_t>& symbol);

bool ParseH264FecPayload(const uint8_t* payload, size_t payload_size,
                         RtpFecPacket* fec_packet);

bool ParseRtpFecPacket(RtpPacket& rtp_packet, RtpFecPacket* fec_packet);

bool IsH264KeyFramePayload(const uint8_t* payload, size_t payload_size);

}  // namespace minirtc

#endif
