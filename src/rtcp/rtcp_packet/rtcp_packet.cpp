#include "rtcp_packet.h"

#include "log.h"

#define IP_PACKET_SIZE 1500  // we assume ethernet

namespace minirtc {

bool RtcpPacket::OnBufferFull(uint8_t* packet, size_t* index,
                              PacketReadyCallback callback) const {
  if (*index == 0) {
    return false;
  }
  callback(packet, *index);
  // The callback consumed the current datagram. Continue serializing the
  // remaining RTCP blocks at the beginning of a fresh buffer. Leaving the
  // write position unchanged makes fragmented feedback packets repeatedly
  // report a full buffer and spins forever.
  *index = 0;
  return true;
}

size_t RtcpPacket::HeaderLength() const {
  size_t length_in_bytes = BlockLength();

  if (length_in_bytes <= 0) {
    LOG_FATAL("length_in_bytes must be a positive value");
    return -1;
  }

  if (length_in_bytes % 4 != 0) {
    LOG_FATAL("Padding must be handled by each subclass, length_in_bytes [{}]",
              length_in_bytes);
    return -1;
  }
  // Length in 32-bit words without common header.
  return (length_in_bytes - kHeaderLength) / 4;
}

// From RFC 3550, RTP: A Transport Protocol for Real-Time Applications.
//
// RTP header format.
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P| RC/FMT  |      PT       |             length            |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
void RtcpPacket::CreateHeader(
    size_t count_or_format,  // Depends on packet type.
    uint8_t packet_type, size_t length, uint8_t* buffer, size_t* pos) {
  CreateHeader(count_or_format, packet_type, length, /*padding=*/false, buffer,
               pos);
}

void RtcpPacket::CreateHeader(
    size_t count_or_format,  // Depends on packet type.
    uint8_t packet_type, size_t length, bool padding, uint8_t* buffer,
    size_t* pos) {
  if (length >= 0xffffU) {
    LOG_FATAL("length must be less than 0xffffU");
  }
  if (count_or_format >= 0x1f) {
    LOG_FATAL("count_or_format must be less than 0x1f");
  }
  constexpr uint8_t kVersionBits = 2 << 6;
  uint8_t padding_bit = padding ? 1 << 5 : 0;
  buffer[*pos + 0] =
      kVersionBits | padding_bit | static_cast<uint8_t>(count_or_format);
  buffer[*pos + 1] = packet_type;
  buffer[*pos + 2] = (length >> 8) & 0xff;
  buffer[*pos + 3] = length & 0xff;
  *pos += kHeaderLength;
}
}
