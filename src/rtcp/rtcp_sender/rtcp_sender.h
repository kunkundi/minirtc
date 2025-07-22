/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-10
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTCP_SENDER_H_
#define _RTCP_SENDER_H_

#define IP_PACKET_SIZE 1500

#include <functional>
#include <vector>

#include "log.h"

namespace minirtc {

class RtcpSender {
 public:
  RtcpSender(std::function<int(const uint8_t*, size_t)> callback,
             size_t max_packet_size)
      : callback_(callback), max_packet_size_(max_packet_size) {
    if (max_packet_size >= IP_PACKET_SIZE) {
      LOG_ERROR("max_packet_size must be less than IP_PACKET_SIZE");
    }
  }
  ~RtcpSender() {
    if (index_ != 0) {
      LOG_ERROR("Unsent rtcp packet");
    }
  }

  // Appends a packet to pending compound packet.
  // Sends rtcp packet if buffer is full and resets the buffer.
  void AppendPacket(const RtcpPacket& packet) {
    packet.Create(buffer_, &index_, max_packet_size_, callback_);
  }

  // Sends pending rtcp packet.
  void Send() {
    if (index_ > 0 && callback_) {
      callback_(buffer_, index_);
      index_ = 0;
    }
  }

 private:
  std::function<int(const uint8_t*, size_t)> callback_ = nullptr;
  const size_t max_packet_size_;
  size_t index_ = 0;
  uint8_t buffer_[IP_PACKET_SIZE];
};
}  // namespace minirtc

#endif