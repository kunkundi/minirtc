/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-22
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_PACKETIZER_H_
#define _RTP_PACKETIZER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "rtp_packet.h"
#include "rtp_packet_to_send.h"

namespace minirtc {

class RtpPacketizer {
 public:
  static std::unique_ptr<RtpPacketizer> Create(uint32_t payload_type,
                                               uint32_t ssrc);

  virtual ~RtpPacketizer() = default;

  // rtp_timestamp is the final timestamp in the payload format's RTP clock
  // ticks. Packetizers serialize it unchanged into every packet of the frame.
  virtual std::vector<std::unique_ptr<RtpPacket>> Build(
      uint8_t* payload, uint32_t payload_size, uint32_t rtp_timestamp,
      bool use_rtp_packet_to_send) = 0;

  virtual std::vector<std::unique_ptr<RtpPacket>> BuildPadding(
      uint32_t payload_size, uint32_t rtp_timestamp,
      bool use_rtp_packet_to_send) = 0;

  virtual void SetIsKeyFrame(bool is_key_frame) {}

  void SetAbsoluteSendTimeExtensionId(
      std::optional<uint8_t> extension_id);

 protected:
  bool HasAbsoluteSendTimeExtension() const {
    return abs_send_time_ext_id_.has_value();
  }
  void AppendAbsoluteSendTimeExtension(
      std::vector<uint8_t>& rtp_packet_frame) const;

 private:
  std::optional<uint8_t> abs_send_time_ext_id_;
};
}  // namespace minirtc

#endif
