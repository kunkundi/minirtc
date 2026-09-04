#include "rtp_packetizer_av1.h"

#include "obu_parser.h"

namespace minirtc {
namespace {
using namespace obu;
}  // namespace

RtpPacketizerAv1::RtpPacketizerAv1(uint32_t ssrc)
    : version_(kRtpVersion),
      has_padding_(false),
      has_extension_(false),
      csrc_count_(0),
      marker_(false),
      payload_type_(rtp::PAYLOAD_TYPE::AV1),
      sequence_number_(0),
      timestamp_(0),
      ssrc_(ssrc),
      profile_(0),
      extension_profile_(0),
      extension_len_(0),
      extension_data_(nullptr) {}

RtpPacketizerAv1::~RtpPacketizerAv1() {}

std::vector<std::unique_ptr<RtpPacket>> RtpPacketizerAv1::Build(
    uint8_t* payload, uint32_t payload_size, uint32_t rtp_timestamp,
    bool use_rtp_packet_to_send) {
  std::vector<std::unique_ptr<RtpPacket>> rtp_packets;
  std::vector<Obu> obus = ParseObus(payload, payload_size);
  has_extension_ = HasAbsoluteSendTimeExtension();

  auto BuildRtpHeader = [&](bool marker) {
    rtp_packet_frame_.clear();
    rtp_packet_frame_.push_back((kRtpVersion << 6) | (0 << 5) |
                                (has_extension_ << 4) | 0);  // V, P, X, CC
    rtp_packet_frame_.push_back((marker << 7) |
                                rtp::PAYLOAD_TYPE(payload_type_));
    rtp_packet_frame_.push_back((sequence_number_ >> 8) & 0xFF);
    rtp_packet_frame_.push_back(sequence_number_ & 0xFF);
    rtp_packet_frame_.push_back((rtp_timestamp >> 24) & 0xFF);
    rtp_packet_frame_.push_back((rtp_timestamp >> 16) & 0xFF);
    rtp_packet_frame_.push_back((rtp_timestamp >> 8) & 0xFF);
    rtp_packet_frame_.push_back(rtp_timestamp & 0xFF);
    rtp_packet_frame_.push_back((ssrc_ >> 24) & 0xFF);
    rtp_packet_frame_.push_back((ssrc_ >> 16) & 0xFF);
    rtp_packet_frame_.push_back((ssrc_ >> 8) & 0xFF);
    rtp_packet_frame_.push_back(ssrc_ & 0xFF);
  };

  auto AppendCsrcsAndExtensions = [&]() {
    for (uint32_t i = 0; i < csrc_count_ && i < csrcs_.size(); ++i) {
      rtp_packet_frame_.push_back((csrcs_[i] >> 24) & 0xFF);
      rtp_packet_frame_.push_back((csrcs_[i] >> 16) & 0xFF);
      rtp_packet_frame_.push_back((csrcs_[i] >> 8) & 0xFF);
      rtp_packet_frame_.push_back(csrcs_[i] & 0xFF);
    }
    if (has_extension_) {
      AppendAbsoluteSendTimeExtension(rtp_packet_frame_);
    }
  };

  auto CreateAndPushRtpPacket = [&](const uint8_t* data, size_t size) {
    if (use_rtp_packet_to_send) {
      auto pkt = std::make_unique<webrtc::RtpPacketToSend>();
      pkt->Build(data, size);
      rtp_packets.emplace_back(std::move(pkt));
    } else {
      auto pkt = std::make_unique<RtpPacket>();
      pkt->Build(data, size);
      rtp_packets.emplace_back(std::move(pkt));
    }
  };

  for (size_t i = 0; i < obus.size(); ++i) {
    const auto& obu = obus[i];
    if (obu.size <= MAX_NALU_LEN) {
      ++sequence_number_;
      bool is_last = (i == (obus.size() - 1));
      int z = (i > 0) ? 1 : 0;
      int y = (!is_last) ? 1 : 0;
      int w = 1;
      // The receiver needs an explicit recovery point. A sequence-header OBU
      // alone is not a reliable keyframe signal, so mark the first packet from
      // the encoder-declared keyframe instead.
      int n = (current_frame_is_key_frame_ && i == 0) ? 1 : 0;
      SetAv1AggrHeader(z, y, w, n);

      BuildRtpHeader(is_last);
      AppendCsrcsAndExtensions();
      rtp_packet_frame_.push_back(av1_aggr_header_);
      rtp_packet_frame_.insert(rtp_packet_frame_.end(), obu.payload.begin(),
                               obu.payload.end());

      CreateAndPushRtpPacket(rtp_packet_frame_.data(),
                             rtp_packet_frame_.size());
    } else {
      size_t packet_num = (obu.size + MAX_NALU_LEN - 1) / MAX_NALU_LEN;

      for (size_t j = 0; j < packet_num; ++j) {
        ++sequence_number_;
        bool is_last = (i == (obus.size() - 1)) && (j == (packet_num - 1));
        size_t offset = j * MAX_NALU_LEN;
        size_t size =
            j == (packet_num - 1) ? (obu.size - offset) : MAX_NALU_LEN;

        int z = (i > 0 || j > 0) ? 1 : 0;
        int y = (!is_last) ? 1 : 0;
        int w = 1;
        int n =
            (current_frame_is_key_frame_ && i == 0 && j == 0) ? 1 : 0;
        SetAv1AggrHeader(z, y, w, n);

        BuildRtpHeader(is_last);
        AppendCsrcsAndExtensions();
        rtp_packet_frame_.push_back(av1_aggr_header_);
        rtp_packet_frame_.insert(rtp_packet_frame_.end(),
                                 obu.payload.begin() + offset,
                                 obu.payload.begin() + offset + size);

        CreateAndPushRtpPacket(rtp_packet_frame_.data(),
                               rtp_packet_frame_.size());
      }
    }
  }

  return rtp_packets;
}

}  // namespace minirtc
