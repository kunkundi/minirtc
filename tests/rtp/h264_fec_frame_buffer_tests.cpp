#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "h264_fec_frame_buffer.h"
#include "rtp_fec.h"
#include "rtp_packetizer_h264.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
  }
}

std::vector<uint8_t> MakeFrame(size_t size) {
  std::vector<uint8_t> frame(size);
  frame[0] = 0x65;
  for (size_t i = 1; i < frame.size(); ++i) {
    frame[i] = static_cast<uint8_t>((i * 31 + 11) & 0xFF);
  }
  return frame;
}

std::vector<minirtc::RtpFecPacket> BuildFecPackets(
    const std::vector<uint8_t>& frame) {
  minirtc::RtpPacketizerH264 packetizer(0x55667788);
  minirtc::FecConfig fec_config;
  fec_config.enabled = true;
  fec_config.protect_keyframes_only = false;
  fec_config.repair_ratio = 0.5;
  fec_config.symbol_size = 128;
  packetizer.SetFecConfig(fec_config);

  auto rtp_packets = packetizer.Build(
      const_cast<uint8_t*>(frame.data()), static_cast<uint32_t>(frame.size()),
      12345, false);

  std::vector<minirtc::RtpFecPacket> fec_packets;
  for (auto& packet : rtp_packets) {
    minirtc::RtpFecPacket fec_packet;
    Expect(minirtc::ParseRtpFecPacket(*packet, &fec_packet),
           "packetizer output should parse as FEC packet");
    fec_packets.push_back(std::move(fec_packet));
  }
  return fec_packets;
}

void RecoversFrameWithOneMissingSourceSymbol() {
  std::vector<uint8_t> frame = MakeFrame(777);
  std::vector<minirtc::RtpFecPacket> packets = BuildFecPackets(frame);

  minirtc::H264FecFrameBuffer frame_buffer;
  std::vector<uint8_t> recovered;
  bool complete = false;

  for (const auto& packet : packets) {
    if ((packet.header.flags & minirtc::kH264FecFlagRepair) == 0 &&
        packet.header.symbol_id == 2) {
      continue;
    }
    complete = frame_buffer.InsertPacket(packet, &recovered);
    if (complete) {
      break;
    }
  }

  Expect(complete, "frame should recover with one missing source symbol");
  Expect(recovered == frame, "recovered frame should match original bytes");
}

void DoesNotEmitFrameWhenRepairIsInsufficient() {
  std::vector<uint8_t> frame = MakeFrame(777);
  std::vector<minirtc::RtpFecPacket> packets = BuildFecPackets(frame);

  minirtc::H264FecFrameBuffer frame_buffer;
  std::vector<uint8_t> recovered;
  bool complete = false;

  for (const auto& packet : packets) {
    if (packet.header.symbol_id == 1 || packet.header.symbol_id == 2 ||
        (packet.header.flags & minirtc::kH264FecFlagRepair) != 0) {
      continue;
    }
    complete = frame_buffer.InsertPacket(packet, &recovered);
  }

  Expect(!complete, "frame should not recover without enough repair symbols");
  Expect(recovered.empty(), "incomplete FEC should not emit partial frame");
}

void ExpiredIncompleteFrameIsRemoved() {
  std::vector<uint8_t> frame = MakeFrame(777);
  std::vector<minirtc::RtpFecPacket> packets = BuildFecPackets(frame);

  minirtc::H264FecFrameBuffer frame_buffer;
  std::vector<uint8_t> recovered;

  frame_buffer.InsertPacket(packets[0], &recovered, 1000);
  frame_buffer.RemoveExpired(1201, 200);

  bool complete = false;
  for (size_t i = 1; i < packets.size(); ++i) {
    complete = frame_buffer.InsertPacket(packets[i], &recovered, 1201);
  }

  Expect(!complete, "expired frame should not recover from late symbols");
  Expect(recovered.empty(), "expired frame should not emit stale data");
}

}  // namespace

int main() {
  RecoversFrameWithOneMissingSourceSymbol();
  DoesNotEmitFrameWhenRepairIsInsufficient();
  ExpiredIncompleteFrameIsRemoved();
  std::cout << "h264_fec_frame_buffer_tests passed" << std::endl;
  return 0;
}
