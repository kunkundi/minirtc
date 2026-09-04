#include "rtp_packetizer.h"

#include "rtp_packetizer_av1.h"
#include "rtp_packetizer_generic.h"
#include "rtp_packetizer_h264.h"
#include "rtp_header_extension.h"

namespace minirtc {

std::unique_ptr<RtpPacketizer> RtpPacketizer::Create(uint32_t payload_type,
                                                     uint32_t ssrc) {
  switch (payload_type) {
    case rtp::PAYLOAD_TYPE::H264:
      return std::make_unique<RtpPacketizerH264>(ssrc);
    case rtp::PAYLOAD_TYPE::AV1:
      return std::make_unique<RtpPacketizerAv1>(ssrc);
    default:
      return std::make_unique<RtpPacketizerGeneric>(ssrc, payload_type);
  }
}

void RtpPacketizer::SetAbsoluteSendTimeExtensionId(
    std::optional<uint8_t> extension_id) {
  if (extension_id.has_value() &&
      !rtp::IsValidOneByteExtensionId(*extension_id)) {
    abs_send_time_ext_id_.reset();
    return;
  }
  abs_send_time_ext_id_ = extension_id;
}

void RtpPacketizer::AppendAbsoluteSendTimeExtension(
    std::vector<uint8_t>& rtp_packet_frame) const {
  if (!abs_send_time_ext_id_.has_value()) {
    return;
  }

  rtp_packet_frame.push_back(
      static_cast<uint8_t>(kOneByteExtensionProfileId >> 8));
  rtp_packet_frame.push_back(
      static_cast<uint8_t>(kOneByteExtensionProfileId));
  rtp_packet_frame.push_back(0);
  rtp_packet_frame.push_back(1);

  constexpr uint8_t kAbsoluteSendTimeLengthMinusOne = 2;
  rtp_packet_frame.push_back(
      static_cast<uint8_t>((*abs_send_time_ext_id_ << 4) |
                           kAbsoluteSendTimeLengthMinusOne));
  // Reserve the wire bytes now so pacing sees the final packet size. The
  // actual Q6.18 value is written immediately before transport send.
  rtp_packet_frame.insert(rtp_packet_frame.end(), 3, 0);
}
}  // namespace minirtc
