/*
 * @Author: DI JUNKUN
 * @Date: 2024-12-18
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_FEEDBACK_H_
#define _RTP_FEEDBACK_H_

#include <stddef.h>
#include <stdint.h>

#include "rtcp_common_header.h"
#include "rtcp_packet.h"

namespace minirtc {

// RTPFB: Transport layer feedback message.
// RFC4585, Section 6.2
class RtpFeedback : public RtcpPacket {
 public:
  static constexpr uint8_t kPacketType = 205;

  RtpFeedback() = default;
  ~RtpFeedback() override = default;

  void SetMediaSsrc(uint32_t ssrc) { media_ssrc_ = ssrc; }

  uint32_t media_ssrc() const { return media_ssrc_; }

 protected:
  static constexpr size_t kCommonFeedbackLength = 8;
  void ParseCommonFeedback(const uint8_t* payload);
  void CreateCommonFeedback(uint8_t* payload) const;

 private:
  uint32_t media_ssrc_ = 0;
};
}  // namespace minirtc

#endif