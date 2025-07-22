/*
 * @Author: DI JUNKUN
 * @Date: 2025-02-26
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTCP_COMMON_HEADER_H_
#define _RTCP_COMMON_HEADER_H_

#include <stddef.h>
#include <stdint.h>

#include "rtcp_typedef.h"

namespace minirtc {

class RtcpCommonHeader {
 public:
  static constexpr size_t kHeaderSizeBytes = 4;

  RtcpCommonHeader() {}
  RtcpCommonHeader(const RtcpCommonHeader&) = default;
  RtcpCommonHeader& operator=(const RtcpCommonHeader&) = default;

  int Create(uint8_t version, uint8_t has_padding, uint8_t count_or_format,
             uint8_t payload_type, uint16_t length, uint8_t* buffer);
  bool Parse(const uint8_t* buffer, size_t size_bytes);

  uint8_t type() const { return packet_type_; }
  // Depending on packet type same header field can be used either as count or
  // as feedback message type (fmt). Caller expected to know how it is used.
  uint8_t fmt() const { return count_or_format_; }
  uint8_t count() const { return count_or_format_; }
  size_t payload_size_bytes() const { return payload_size_; }
  const uint8_t* payload() const { return payload_; }
  size_t packet_size() const {
    return kHeaderSizeBytes + payload_size_ + padding_size_;
  }
  // Returns pointer to the next RTCP packet in compound packet.
  const uint8_t* NextPacket() const {
    return payload_ + payload_size_ + padding_size_;
  }

 private:
  uint8_t packet_type_ = 0;
  uint8_t count_or_format_ = 0;
  uint8_t padding_size_ = 0;
  uint32_t payload_size_ = 0;
  const uint8_t* payload_ = nullptr;
};
}  // namespace minirtc

#endif