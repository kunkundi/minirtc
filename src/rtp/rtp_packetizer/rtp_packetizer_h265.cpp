#include "rtp_packetizer_h265.h"

#include <algorithm>

#include "byte_io.h"

namespace minirtc {
namespace {

// Locate either Annex B start-code length. NVENC supplies one access unit.
const uint8_t* FindNal(const uint8_t* p, const uint8_t* end,
                       const uint8_t** start_code) {
  for (; end - p >= 3; ++p) {
    if (p[0] || p[1]) continue;
    if (p[2] == 1) {
      *start_code = p;
      return p + 3;
    }
    if (end - p >= 4 && p[2] == 0 && p[3] == 1) {
      *start_code = p;
      return p + 4;
    }
  }
  *start_code = end;
  return end;
}

}  // namespace

bool RtpPacketizerH265::AppendPacket(Packets& packets, uint32_t timestamp,
                                     bool to_send, bool marker, bool padding,
                                     const uint8_t* prefix, size_t prefix_size,
                                     const uint8_t* data, size_t size) {
  std::vector<uint8_t> wire(kFixedHeaderSize);
  wire.reserve(kFixedHeaderSize + 8 + prefix_size + size);
  wire[0] =
      0x80 | (padding ? 0x20 : 0) | (HasAbsoluteSendTimeExtension() ? 0x10 : 0);
  wire[1] = (marker ? 0x80 : 0) | (padding ? rtp::H265 - 1 : rtp::H265);
  ByteWriter<uint16_t>::WriteBigEndian(wire.data() + 2, ++sequence_);
  ByteWriter<uint32_t>::WriteBigEndian(wire.data() + 4, timestamp);
  ByteWriter<uint32_t>::WriteBigEndian(wire.data() + 8, ssrc_);
  AppendAbsoluteSendTimeExtension(wire);
  if (prefix_size) wire.insert(wire.end(), prefix, prefix + prefix_size);
  if (padding) {
    wire.resize(wire.size() + size, 0);
    wire.back() = 1;
  } else {
    wire.insert(wire.end(), data, data + size);
  }
  std::unique_ptr<RtpPacket> packet;
  if (to_send)
    packet = std::make_unique<webrtc::RtpPacketToSend>();
  else
    packet = std::make_unique<RtpPacket>();
  if (!packet->Build(wire.data(), wire.size())) return false;
  packets.push_back(std::move(packet));
  return true;
}

std::vector<std::unique_ptr<RtpPacket>> RtpPacketizerH265::Build(
    uint8_t* frame, uint32_t size, uint32_t timestamp, bool to_send) {
  std::lock_guard<std::mutex> lock(mutex_);
  Packets packets;
  if (!frame || !size || max_payload_size_ <= 3) return packets;
  const uint8_t* end = frame + size;
  const uint8_t* start_code;
  const uint8_t* nal = FindNal(frame, end, &start_code);
  while (nal < end) {
    const uint8_t* next = FindNal(nal, end, &start_code);
    const size_t length = start_code - nal;
    if (length < 3 || (nal[0] & 0x80) || !(nal[1] & 7)) return {};
    const uint8_t type = (nal[0] >> 1) & 63;
    if (type >= 48) return {};
    // An AUD makes the start unambiguous even if the previous frame was lost
    // or the marker packet arrives before the parameter sets.
    if (packets.empty() && type != 35) {
      constexpr uint8_t aud[] = {35 << 1, 1, 0x50};
      if (!AppendPacket(packets, timestamp, to_send, false, false, nullptr, 0,
                        aud, sizeof(aud)))
        return {};
    }
    // Some producers place their AUD after the parameter sets. The synthetic
    // leading AUD already delimits that access unit on the wire.
    if (type == 35 && !packets.empty()) {
      if (next == end) return {};
      nal = next;
      continue;
    }
    if (length <= max_payload_size_) {
      if (!AppendPacket(packets, timestamp, to_send, next == end, false,
                        nullptr, 0, nal, length))
        return {};
    } else {
      for (size_t offset = 2; offset < length;) {
        const size_t bytes =
            std::min<size_t>(length - offset, max_payload_size_ - 3);
        const bool last = offset + bytes == length;
        const uint8_t fu[] = {static_cast<uint8_t>((nal[0] & 0x81) | (49 << 1)),
                              nal[1],
                              static_cast<uint8_t>((offset == 2 ? 0x80 : 0) |
                                                   (last ? 0x40 : 0) | type)};
        if (!AppendPacket(packets, timestamp, to_send, last && next == end,
                          false, fu, sizeof(fu), nal + offset, bytes))
          return {};
        offset += bytes;
      }
    }
    nal = next;
  }
  return packets;
}

std::vector<std::unique_ptr<RtpPacket>> RtpPacketizerH265::BuildPadding(
    uint32_t size, uint32_t timestamp, bool to_send) {
  std::lock_guard<std::mutex> lock(mutex_);
  Packets packets;
  while (size) {
    const uint32_t bytes = std::min(size, max_payload_size_);
    if (!AppendPacket(packets, timestamp, to_send, false, true, nullptr, 0,
                      nullptr, bytes))
      return {};
    size -= bytes;
  }
  return packets;
}

}  // namespace minirtc
