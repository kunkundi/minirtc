#include "rtp_packetizer_generic.h"

namespace minirtc {

RtpPacketizerGeneric::RtpPacketizerGeneric(uint32_t ssrc, uint32_t payload_type)
    : version_(kRtpVersion),
      has_padding_(false),
      has_extension_(false),
      csrc_count_(0),
      marker_(false),
      payload_type_(payload_type),
      sequence_number_(0),
      timestamp_(0),
      ssrc_(ssrc),
      profile_(0),
      extension_profile_(0),
      extension_len_(0),
      extension_data_(nullptr) {}

RtpPacketizerGeneric::~RtpPacketizerGeneric() {}

std::vector<std::unique_ptr<RtpPacket>> RtpPacketizerGeneric::Build(
    uint8_t* payload, uint32_t payload_size, uint32_t rtp_timestamp,
    bool use_rtp_packet_to_send) {
  uint32_t last_packet_size = payload_size % MAX_NALU_LEN;
  uint32_t packet_num =
      payload_size / MAX_NALU_LEN + (last_packet_size ? 1 : 0);

  std::vector<std::unique_ptr<RtpPacket>> rtp_packets;

  for (uint32_t index = 0; index < packet_num; index++) {
    version_ = kRtpVersion;
    has_padding_ = false;
    has_extension_ = HasAbsoluteSendTimeExtension();
    csrc_count_ = 0;
    marker_ = index == packet_num - 1 ? 1 : 0;
    payload_type_ = rtp::PAYLOAD_TYPE(payload_type_);
    sequence_number_++;
    timestamp_ = rtp_timestamp;
    // ssrc_ = ssrc_;

    if (!csrc_count_) {
    }

    rtp_packet_frame_.clear();
    rtp_packet_frame_.push_back((version_ << 6) | (has_padding_ << 5) |
                                (has_extension_ << 4) | csrc_count_);
    rtp_packet_frame_.push_back((marker_ << 7) | payload_type_);
    rtp_packet_frame_.push_back((sequence_number_ >> 8) & 0xFF);
    rtp_packet_frame_.push_back(sequence_number_ & 0xFF);
    rtp_packet_frame_.push_back((timestamp_ >> 24) & 0xFF);
    rtp_packet_frame_.push_back((timestamp_ >> 16) & 0xFF);
    rtp_packet_frame_.push_back((timestamp_ >> 8) & 0xFF);
    rtp_packet_frame_.push_back(timestamp_ & 0xFF);
    rtp_packet_frame_.push_back((ssrc_ >> 24) & 0xFF);
    rtp_packet_frame_.push_back((ssrc_ >> 16) & 0xFF);
    rtp_packet_frame_.push_back((ssrc_ >> 8) & 0xFF);
    rtp_packet_frame_.push_back(ssrc_ & 0xFF);

    for (uint32_t index = 0; index < csrc_count_ && !csrcs_.empty(); index++) {
      rtp_packet_frame_.push_back((csrcs_[index] >> 24) & 0xFF);
      rtp_packet_frame_.push_back((csrcs_[index] >> 16) & 0xFF);
      rtp_packet_frame_.push_back((csrcs_[index] >> 8) & 0xFF);
      rtp_packet_frame_.push_back(csrcs_[index] & 0xFF);
    }

    if (has_extension_) {
      AppendAbsoluteSendTimeExtension(rtp_packet_frame_);
    }

    if (index == packet_num - 1 && last_packet_size > 0) {
      rtp_packet_frame_.insert(rtp_packet_frame_.end(), payload,
                               payload + last_packet_size);
    } else {
      rtp_packet_frame_.insert(rtp_packet_frame_.end(), payload,
                               payload + MAX_NALU_LEN);
    }

    if (use_rtp_packet_to_send) {
      std::unique_ptr<webrtc::RtpPacketToSend> rtp_packet =
          std::make_unique<webrtc::RtpPacketToSend>();
      rtp_packet->Build(rtp_packet_frame_.data(), rtp_packet_frame_.size());
      rtp_packets.emplace_back(std::move(rtp_packet));
    } else {
      std::unique_ptr<RtpPacket> rtp_packet = std::make_unique<RtpPacket>();
      rtp_packet->Build(rtp_packet_frame_.data(), rtp_packet_frame_.size());
      rtp_packets.emplace_back(std::move(rtp_packet));
    }
  }

  return rtp_packets;
}
}