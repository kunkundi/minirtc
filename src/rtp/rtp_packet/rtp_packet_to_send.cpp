/*
 *  Copyright (c) 2018 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "rtp_packet_to_send.h"

#include <cstdint>

namespace minirtc {
namespace webrtc {

RtpPacketToSend::RtpPacketToSend() {}
RtpPacketToSend::RtpPacketToSend(size_t capacity) : RtpPacket(capacity) {}
RtpPacketToSend::RtpPacketToSend(const RtpPacketToSend& packet) = default;
RtpPacketToSend::RtpPacketToSend(RtpPacketToSend&& packet) = default;

RtpPacketToSend& RtpPacketToSend::operator=(const RtpPacketToSend& packet) =
    default;
RtpPacketToSend& RtpPacketToSend::operator=(RtpPacketToSend&& packet) = default;

RtpPacketToSend::~RtpPacketToSend() = default;

void RtpPacketToSend::set_packet_type(RtpPacketMediaType type) {
  if (packet_type_ == RtpPacketMediaType::kAudio) {
    original_packet_type_ = OriginalType::kAudio;
  } else if (packet_type_ == RtpPacketMediaType::kVideo) {
    original_packet_type_ = OriginalType::kVideo;
  }
  packet_type_ = type;
}

void AddAbsSendTimeExtension(std::vector<uint8_t>& rtp_packet_frame) {
  uint16_t extension_profile = 0xBEDE;  // One-byte header extension
  uint8_t sub_extension_id = 3;         // ID for Absolute Send Time
  uint8_t sub_extension_length =
      2;  // Length of the extension data in bytes minus 1

  uint32_t abs_send_time =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  abs_send_time &= 0x00FFFFFF;  // Absolute Send Time is 24 bits

  // Add extension profile
  rtp_packet_frame.push_back((extension_profile >> 8) & 0xFF);
  rtp_packet_frame.push_back(extension_profile & 0xFF);

  // Add extension length (in 32-bit words, minus one)
  rtp_packet_frame.push_back(
      0x00);  // Placeholder for length, will be updated later
  rtp_packet_frame.push_back(0x01);  // One 32-bit word

  // Add Absolute Send Time extension
  rtp_packet_frame.push_back((sub_extension_id << 4) | sub_extension_length);
  rtp_packet_frame.push_back((abs_send_time >> 16) & 0xFF);
  rtp_packet_frame.push_back((abs_send_time >> 8) & 0xFF);
  rtp_packet_frame.push_back(abs_send_time & 0xFF);
}

bool RtpPacketToSend::BuildRtxPacket() {
  if (!retransmitted_sequence_number_.has_value()) {
    return false;
  }

  uint8_t version = Version();
  uint8_t has_padding = HasPadding();
  uint8_t has_extension = HasExtension();
  uint8_t csrc_count = Csrcs().size();
  bool marker = Marker();
  uint8_t payload_type = PayloadType();
  uint16_t sequence_number = SequenceNumber();
  uint32_t ssrc = Ssrc();
  std::vector<uint32_t> csrcs = Csrcs();

  uint32_t timestamp = Timestamp();

  if (!csrc_count) {
  }

  SetPayloadType(rtp::PAYLOAD_TYPE::RTX);

  rtp_packet_frame_.clear();
  rtp_packet_frame_.push_back((version << 6) | (has_padding << 5) |
                              (has_extension << 4) | csrc_count);
  rtp_packet_frame_.push_back((marker << 7) | payload_type);
  rtp_packet_frame_.push_back((sequence_number >> 8) & 0xFF);
  rtp_packet_frame_.push_back(sequence_number & 0xFF);
  rtp_packet_frame_.push_back((timestamp >> 24) & 0xFF);
  rtp_packet_frame_.push_back((timestamp >> 16) & 0xFF);
  rtp_packet_frame_.push_back((timestamp >> 8) & 0xFF);
  rtp_packet_frame_.push_back(timestamp & 0xFF);
  rtp_packet_frame_.push_back((ssrc >> 24) & 0xFF);
  rtp_packet_frame_.push_back((ssrc >> 16) & 0xFF);
  rtp_packet_frame_.push_back((ssrc >> 8) & 0xFF);
  rtp_packet_frame_.push_back(ssrc & 0xFF);

  for (uint32_t index = 0; index < csrc_count && !csrcs.empty(); index++) {
    rtp_packet_frame_.push_back((csrcs[index] >> 24) & 0xFF);
    rtp_packet_frame_.push_back((csrcs[index] >> 16) & 0xFF);
    rtp_packet_frame_.push_back((csrcs[index] >> 8) & 0xFF);
    rtp_packet_frame_.push_back(csrcs[index] & 0xFF);
  }

  if (has_extension) {
    std::vector<Extension> extensions = Extensions();
    AddAbsSendTimeExtension(rtp_packet_frame_);
  }

  rtp_packet_frame_.push_back((retransmitted_sequence_number_.value() >> 8) &
                              0xFF);
  rtp_packet_frame_.push_back(retransmitted_sequence_number_.value() & 0xFF);

  rtp_packet_frame_.insert(rtp_packet_frame_.end(), Payload(),
                           Payload() + PayloadSize());

  Build(rtp_packet_frame_.data(), rtp_packet_frame_.size());

  return true;
}

}  // namespace webrtc
}  // namespace minirtc