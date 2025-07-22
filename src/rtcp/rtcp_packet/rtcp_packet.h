/*
 * @Author: DI JUNKUN
 * @Date: 2024-12-11
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTCP_PACKET_H_
#define _RTCP_PACKET_H_

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <vector>

namespace minirtc {

class RtcpPacket {
 public:
  typedef enum { SR = 200, RR = 201, TCC = 11, NACK = 1 } RtcpPayloadType;
  // Callback used to signal that an RTCP packet is ready. Note that this may
  // not contain all data in this RtcpPacket; if a packet cannot fit in
  // max_length bytes, it will be fragmented and multiple calls to this
  // callback will be made.
  using PacketReadyCallback =
      std::function<void(const uint8_t* packet, size_t size)>;

  virtual ~RtcpPacket() = default;

  void SetSenderSsrc(uint32_t ssrc) { sender_ssrc_ = ssrc; }
  uint32_t sender_ssrc() const { return sender_ssrc_; }

  // Size of this packet in bytes (including headers).
  virtual size_t BlockLength() const = 0;

  // Creates packet in the given buffer at the given position.
  // Calls PacketReadyCallback::OnPacketReady if remaining buffer is too small
  // and assume buffer can be reused after OnPacketReady returns.
  virtual bool Create(uint8_t* packet, size_t* position, size_t max_length,
                      PacketReadyCallback callback) const = 0;

 protected:
  // Size of the rtcp common header.
  static constexpr size_t kHeaderLength = 4;
  RtcpPacket() {}

  static void CreateHeader(size_t count_or_format, uint8_t packet_type,
                           size_t block_length,  // Payload size in 32bit words.
                           uint8_t* buffer, size_t* pos);

  static void CreateHeader(size_t count_or_format, uint8_t packet_type,
                           size_t block_length,  // Payload size in 32bit words.
                           bool padding,  // True if there are padding bytes.
                           uint8_t* buffer, size_t* pos);

  bool OnBufferFull(uint8_t* packet, size_t* index,
                    PacketReadyCallback callback) const;
  // Size of the rtcp packet as written in header.
  size_t HeaderLength() const;

 private:
  uint32_t sender_ssrc_ = 0;
};
}  // namespace minirtc

#endif