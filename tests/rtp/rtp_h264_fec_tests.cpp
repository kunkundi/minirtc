#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "rtp_fec.h"
#include "rtp_packetizer_h264.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
  }
}

std::vector<uint8_t> MakeH264Idr(size_t size) {
  std::vector<uint8_t> frame(size);
  frame[0] = 0x65;
  for (size_t i = 1; i < frame.size(); ++i) {
    frame[i] = static_cast<uint8_t>((i * 29 + 7) & 0xFF);
  }
  return frame;
}

std::vector<uint8_t> MakeH264Delta(size_t size) {
  std::vector<uint8_t> frame = MakeH264Idr(size);
  frame[0] = 0x41;
  return frame;
}

void DisabledFecProducesOnlyMediaPackets() {
  minirtc::RtpPacketizerH264 packetizer(0x11223344);
  std::vector<uint8_t> frame = MakeH264Idr(2048);

  auto packets = packetizer.Build(frame.data(), static_cast<uint32_t>(frame.size()),
                                  9000, false);

  Expect(!packets.empty(), "packetizer should produce media packets");
  for (const auto& packet : packets) {
    Expect(packet->PayloadType() == minirtc::rtp::PAYLOAD_TYPE::H264,
           "FEC disabled should produce only H264 packets");
  }
}

void EnabledFecKeyframeProducesSourceAndRepairPackets() {
  minirtc::RtpPacketizerH264 packetizer(0x11223344);
  minirtc::FecConfig fec_config;
  fec_config.enabled = true;
  fec_config.protect_keyframes_only = true;
  fec_config.repair_ratio = 0.5;
  fec_config.symbol_size = 256;
  packetizer.SetFecConfig(fec_config);

  std::vector<uint8_t> frame = MakeH264Idr(2048);
  auto packets = packetizer.Build(frame.data(), static_cast<uint32_t>(frame.size()),
                                  9000, false);

  int source_count = 0;
  int repair_count = 0;
  bool saw_header = false;

  for (const auto& packet : packets) {
    if (packet->PayloadType() == minirtc::rtp::PAYLOAD_TYPE::H264_FEC_SOURCE ||
        packet->PayloadType() == minirtc::rtp::PAYLOAD_TYPE::H264_FEC_REPAIR) {
      minirtc::RtpFecPacket fec_packet;
      Expect(minirtc::ParseRtpFecPacket(*packet, &fec_packet),
             "FEC RTP payload should parse");
      Expect(fec_packet.header.version == 1, "FEC header version should be 1");
      Expect(fec_packet.header.rtp_timestamp == 9000,
             "FEC header should preserve frame RTP timestamp");
      Expect(fec_packet.header.original_frame_size == frame.size(),
             "FEC header should preserve original frame size");
      Expect(fec_packet.symbol.size() == fec_config.symbol_size,
             "FEC symbol payload should match configured symbol size");
      saw_header = true;
    }

    if (packet->PayloadType() == minirtc::rtp::PAYLOAD_TYPE::H264_FEC_SOURCE) {
      ++source_count;
    } else if (packet->PayloadType() ==
               minirtc::rtp::PAYLOAD_TYPE::H264_FEC_REPAIR) {
      ++repair_count;
    }
  }

  Expect(saw_header, "FEC enabled keyframe should produce FEC packets");
  Expect(source_count == 8, "2048 bytes / 256 byte symbols should produce 8 source packets");
  Expect(repair_count == 4, "0.5 repair ratio should produce ceil(8 * 0.5) repair packets");
}

void EnabledFecDoesNotProtectDeltaFrameByDefault() {
  minirtc::RtpPacketizerH264 packetizer(0x11223344);
  minirtc::FecConfig fec_config;
  fec_config.enabled = true;
  fec_config.protect_keyframes_only = true;
  fec_config.repair_ratio = 0.5;
  fec_config.symbol_size = 256;
  packetizer.SetFecConfig(fec_config);

  std::vector<uint8_t> frame = MakeH264Delta(2048);
  auto packets = packetizer.Build(frame.data(), static_cast<uint32_t>(frame.size()),
                                  9000, false);

  Expect(!packets.empty(), "delta frame should still produce RTP packets");
  for (const auto& packet : packets) {
    Expect(packet->PayloadType() == minirtc::rtp::PAYLOAD_TYPE::H264,
           "keyframe-only FEC should not protect delta frames");
  }
}

void LargeFecFrameSplitsIntoMultipleBlocks() {
  minirtc::RtpPacketizerH264 packetizer(0x11223344);
  minirtc::FecConfig fec_config;
  fec_config.enabled = true;
  fec_config.protect_keyframes_only = false;
  fec_config.repair_ratio = 0.5;
  fec_config.symbol_size = 64;
  fec_config.max_source_symbols_per_block = 4;
  packetizer.SetFecConfig(fec_config);

  std::vector<uint8_t> frame = MakeH264Idr(9 * fec_config.symbol_size);
  auto packets = packetizer.Build(frame.data(), static_cast<uint32_t>(frame.size()),
                                  9000, false);

  bool saw_block_zero = false;
  bool saw_block_one = false;
  bool saw_block_two = false;
  for (const auto& packet : packets) {
    minirtc::RtpFecPacket fec_packet;
    Expect(minirtc::ParseRtpFecPacket(*packet, &fec_packet),
           "split FEC packet should parse");
    Expect(fec_packet.header.block_count == 3,
           "9 source symbols with max 4 per block should make 3 blocks");
    saw_block_zero |= fec_packet.header.block_index == 0;
    saw_block_one |= fec_packet.header.block_index == 1;
    saw_block_two |= fec_packet.header.block_index == 2;
  }

  Expect(saw_block_zero && saw_block_one && saw_block_two,
         "split FEC frame should emit every block index");
}

}  // namespace

int main() {
  DisabledFecProducesOnlyMediaPackets();
  EnabledFecKeyframeProducesSourceAndRepairPackets();
  EnabledFecDoesNotProtectDeltaFrameByDefault();
  LargeFecFrameSplitsIntoMultipleBlocks();
  std::cout << "rtp_h264_fec_tests passed" << std::endl;
  return 0;
}
