#include "rtp_fec.h"

#include <algorithm>

namespace minirtc {
namespace {

void WriteU16(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void WriteU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

uint16_t ReadU16(const uint8_t* data) {
  return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t ReadU32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

bool IsStartCode3(const uint8_t* data) {
  return data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01;
}

bool IsStartCode4(const uint8_t* data) {
  return data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 &&
         data[3] == 0x01;
}

bool IsKeyNalType(uint8_t nal_type) {
  return nal_type == 5 || nal_type == 7;
}

}  // namespace

std::vector<uint8_t> SerializeH264FecPayload(
    const H264FecHeader& header, const std::vector<uint8_t>& symbol) {
  std::vector<uint8_t> payload;
  payload.reserve(kH264FecHeaderSize + symbol.size());

  payload.push_back(header.version);
  payload.push_back(header.flags);
  WriteU16(payload, header.block_index);
  WriteU16(payload, header.block_count);
  WriteU16(payload, header.source_symbol_count);
  WriteU16(payload, header.repair_symbol_count);
  WriteU16(payload, header.symbol_size);
  WriteU16(payload, header.symbol_id);
  WriteU32(payload, header.frame_id);
  WriteU32(payload, header.rtp_timestamp);
  WriteU32(payload, header.original_frame_size);
  WriteU32(payload, header.block_original_size);
  payload.insert(payload.end(), symbol.begin(), symbol.end());

  return payload;
}

bool ParseH264FecPayload(const uint8_t* payload, size_t payload_size,
                         RtpFecPacket* fec_packet) {
  if (!payload || !fec_packet || payload_size < kH264FecHeaderSize) {
    return false;
  }

  H264FecHeader header;
  header.version = payload[0];
  header.flags = payload[1];
  header.block_index = ReadU16(payload + 2);
  header.block_count = ReadU16(payload + 4);
  header.source_symbol_count = ReadU16(payload + 6);
  header.repair_symbol_count = ReadU16(payload + 8);
  header.symbol_size = ReadU16(payload + 10);
  header.symbol_id = ReadU16(payload + 12);
  header.frame_id = ReadU32(payload + 14);
  header.rtp_timestamp = ReadU32(payload + 18);
  header.original_frame_size = ReadU32(payload + 22);
  header.block_original_size = ReadU32(payload + 26);

  if (header.version != kH264FecVersion || header.symbol_size == 0 ||
      header.block_original_size == 0 ||
      payload_size - kH264FecHeaderSize != header.symbol_size) {
    return false;
  }

  fec_packet->header = header;
  fec_packet->symbol.assign(payload + kH264FecHeaderSize,
                            payload + payload_size);
  return true;
}

bool ParseRtpFecPacket(RtpPacket& rtp_packet, RtpFecPacket* fec_packet) {
  if (rtp_packet.PayloadType() != rtp::PAYLOAD_TYPE::H264_FEC_SOURCE &&
      rtp_packet.PayloadType() != rtp::PAYLOAD_TYPE::H264_FEC_REPAIR) {
    return false;
  }

  CopyOnWriteBuffer buffer = rtp_packet.Buffer();
  if (rtp_packet.HeaderSize() > buffer.size()) {
    return false;
  }

  return ParseH264FecPayload(buffer.data() + rtp_packet.HeaderSize(),
                             rtp_packet.PayloadSize(), fec_packet);
}

bool IsH264KeyFramePayload(const uint8_t* payload, size_t payload_size) {
  if (!payload || payload_size == 0) {
    return false;
  }

  if (IsKeyNalType(payload[0] & 0x1F)) {
    return true;
  }

  for (size_t i = 0; i + 4 < payload_size; ++i) {
    size_t nal_offset = 0;
    if (i + 3 < payload_size && IsStartCode3(payload + i)) {
      nal_offset = i + 3;
    } else if (i + 4 < payload_size && IsStartCode4(payload + i)) {
      nal_offset = i + 4;
    } else {
      continue;
    }

    if (nal_offset < payload_size &&
        IsKeyNalType(payload[nal_offset] & 0x1F)) {
      return true;
    }
  }

  return false;
}

}  // namespace minirtc
