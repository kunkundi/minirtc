/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_RTP_RTCP_SOURCE_RTCP_PACKET_FIR_H_
#define MODULES_RTP_RTCP_SOURCE_RTCP_PACKET_FIR_H_

#include <vector>

#include "rtcp_common_header.h"
#include "rtp_feedback.h"

namespace minirtc {
namespace webrtc {
namespace rtcp {
// Full intra request (FIR) (RFC 5104).
class Fir : public RtpFeedback {
 public:
  // FIR is payload-specific feedback (PSFB), RFC 5104 section 4.3.1.
  // RtpFeedback is retained for the common sender/media SSRC serializer, but
  // the wire packet type must be 206 rather than RTPFB's 205.
  static constexpr uint8_t kPacketType = 206;
  static constexpr uint8_t kFeedbackMessageType = 4;
  struct Request {
    Request() : ssrc(0), seq_nr(0) {}
    Request(uint32_t ssrc, uint8_t seq_nr) : ssrc(ssrc), seq_nr(seq_nr) {}
    uint32_t ssrc;
    uint8_t seq_nr;
  };

  Fir();
  Fir(const Fir& fir);
  ~Fir() override;

  // Parse assumes header is already parsed and validated.
  bool Parse(const RtcpCommonHeader& packet);

  void AddRequestTo(uint32_t ssrc, uint8_t seq_num) {
    items_.emplace_back(ssrc, seq_num);
  }
  const std::vector<Request>& requests() const { return items_; }

  size_t BlockLength() const override;

  bool Create(uint8_t* packet, size_t* index, size_t max_length,
              PacketReadyCallback callback) const override;

 private:
  static constexpr size_t kFciLength = 8;

  // SSRC of media source is not used in FIR packet. Shadow base functions.
  void SetMediaSsrc(uint32_t ssrc);
  uint32_t media_ssrc() const;

  std::vector<Request> items_;
};
}  // namespace rtcp
}  // namespace webrtc
}  // namespace minirtc
#endif  // MODULES_RTP_RTCP_SOURCE_RTCP_PACKET_FIR_H_
