/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-23
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_PACKET_AV1_H_
#define _RTP_PACKET_AV1_H_

#include "rtp_packet.h"

class RtpPacketAv1 : public RtpPacket {
 public:
  RtpPacketAv1();
  virtual ~RtpPacketAv1();

 public:
  bool GetFrameHeaderInfo();

  // 帧起始条件
  bool Av1FrameStart() {
    // 完整帧，或分片的第一个 packet
    return (z_ == 0) || (z_ == 1 && y_ == 1);
  }

  // 帧结束条件
  bool Av1FrameEnd() {
    // 完整帧，或分片的最后一个 packet
    return (z_ == 0) || (z_ == 1 && y_ == 0);
  }

 private:
  uint8_t av1_aggr_header_ = 0;
  uint8_t z_ = 0;
  uint8_t y_ = 0;
  uint8_t w_ = 0;
  uint8_t n_ = 0;
};

#endif