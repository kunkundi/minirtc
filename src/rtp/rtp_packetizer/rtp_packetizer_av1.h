/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-23
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_PACKETIZER_AV1_H_
#define _RTP_PACKETIZER_AV1_H_

#include "rtp_packetizer.h"

namespace minirtc {

class RtpPacketizerAv1 : public RtpPacketizer {
 public:
  RtpPacketizerAv1(uint32_t ssrc);

  virtual ~RtpPacketizerAv1();

  std::vector<std::unique_ptr<RtpPacket>> Build(
      uint8_t* payload, uint32_t payload_size, uint32_t rtp_timestamp,
      bool use_rtp_packet_to_send) override;

  std::vector<std::unique_ptr<RtpPacket>> BuildPadding(
      uint32_t payload_size, uint32_t rtp_timestamp,
      bool use_rtp_packet_to_send) override {
    return std::vector<std::unique_ptr<RtpPacket>>{};
  };

 private:
  void AddAbsSendTimeExtension(std::vector<uint8_t>& rtp_packet_frame);
  void SetAv1AggrHeader(int z, int y, int w, int n) {
    av1_aggr_header_ = 0;
    if (z) av1_aggr_header_ |= (1 << 7);
    if (y) av1_aggr_header_ |= (1 << 6);
    if (w) av1_aggr_header_ |= w << 4;
    if (n) av1_aggr_header_ |= (1 << 3);
  }

 private:
  uint8_t version_;
  bool has_padding_;
  bool has_extension_;
  uint32_t csrc_count_;
  bool marker_;
  uint32_t payload_type_;
  uint16_t sequence_number_;
  uint32_t timestamp_;
  uint32_t ssrc_;
  std::vector<uint32_t> csrcs_;
  uint16_t profile_;
  uint16_t extension_profile_;
  uint16_t extension_len_;
  uint8_t* extension_data_;

  uint8_t av1_aggr_header_ = 0;

 private:
  std::vector<uint8_t> rtp_packet_frame_;
};
}  // namespace minirtc

#endif