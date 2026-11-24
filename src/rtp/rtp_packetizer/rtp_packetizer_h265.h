/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-24
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_PACKETIZER_H265_H_
#define _RTP_PACKETIZER_H265_H_

#include <mutex>

#include "rtp_packetizer.h"

namespace minirtc {

class RtpPacketizerH265 final : public RtpPacketizer {
 public:
  explicit RtpPacketizerH265(uint32_t ssrc) : ssrc_(ssrc) {}

  std::vector<std::unique_ptr<RtpPacket>> Build(uint8_t* frame, uint32_t size,
                                                uint32_t timestamp,
                                                bool to_send) override;
  std::vector<std::unique_ptr<RtpPacket>> BuildPadding(uint32_t size,
                                                       uint32_t timestamp,
                                                       bool to_send) override;

 private:
  using Packets = std::vector<std::unique_ptr<RtpPacket>>;
  bool AppendPacket(Packets& packets, uint32_t timestamp, bool to_send,
                    bool marker, bool padding, const uint8_t* prefix,
                    size_t prefix_size, const uint8_t* data, size_t size);
  std::mutex mutex_;
  uint32_t ssrc_;
  uint16_t sequence_ = 0;
};

}  // namespace minirtc

#endif
