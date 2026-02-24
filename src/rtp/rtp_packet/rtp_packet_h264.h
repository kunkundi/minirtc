/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-23
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_PACKET_H264_H_
#define _RTP_PACKET_H264_H_

#include "rtp_packet.h"

namespace minirtc {
class RtpPacketH264 : public RtpPacket {
 public:
  RtpPacketH264();
  virtual ~RtpPacketH264();

 public:
  bool GetFrameHeaderInfo();
  // NAL
  rtp::NAL_UNIT_TYPE NalUnitType() {
    return rtp::NAL_UNIT_TYPE(fu_indicator_.nal_unit_type);
  }
  bool FuAStart() { return fu_header_.start; }
  bool FuAEnd() { return fu_header_.end; }

  uint16_t GetOsn() { return osn_; }

  uint8_t ForbiddenBit() const { return fu_indicator_.forbidden_bit; }
  uint8_t NalRefIdc() const { return fu_indicator_.nal_reference_idc; }
  uint8_t FuNalUnitType() const { return fu_header_.nal_unit_type; }

 private:
  uint16_t osn_;
  rtp::FU_INDICATOR fu_indicator_;
  rtp::FU_HEADER fu_header_;
  bool fu_info_got_ = false;
};
}  // namespace minirtc

#endif
