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
  const CopyOnWriteBuffer original_buffer = Buffer();
  const size_t original_header_size = HeaderSize();

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
    const size_t extension_offset = kFixedHeaderSize + csrc_count * 4;
    if (extension_offset > original_header_size ||
        original_header_size > original_buffer.size()) {
      return false;
    }
    rtp_packet_frame_.insert(
        rtp_packet_frame_.end(), original_buffer.data() + extension_offset,
        original_buffer.data() + original_header_size);
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