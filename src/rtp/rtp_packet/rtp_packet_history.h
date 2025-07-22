/*
 * @Author: DI JUNKUN
 * @Date: 2025-02-14
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_PACKET_HISTORY_H_
#define _RTP_PACKET_HISTORY_H_

#include <deque>
#include <memory>

#include "api/clock/clock.h"
#include "clock/system_clock.h"
#include "rtp_packet_to_send.h"

namespace minirtc {
class RtpPacketHistory {
 public:
  static constexpr size_t kMaxCapacity = 600;
  // Maximum number of entries in prioritized queue of padding packets.
  static constexpr size_t kMaxPaddingHistory = 63;
  // Don't remove packets within max(1 second, 3x RTT).
  static constexpr webrtc::TimeDelta kMinPacketDuration =
      webrtc::TimeDelta::Seconds(1);
  static constexpr int kMinPacketDurationRtt = 3;
  // With kStoreAndCull, always remove packets after 3x max(1000ms, 3x rtt).
  static constexpr int kPacketCullingDelayFactor = 3;

 public:
  RtpPacketHistory(std::shared_ptr<SystemClock> clock);
  ~RtpPacketHistory();

 public:
  void SetRtt(webrtc::TimeDelta rtt);
  void PutRtpPacket(std::unique_ptr<webrtc::RtpPacketToSend> rtp_packet,
                    int64_t send_time);
  void MarkPacketAsSent(uint16_t sequence_number);

  std::unique_ptr<webrtc::RtpPacketToSend> GetPacketAndMarkAsPending(
      uint16_t sequence_number);

  std::unique_ptr<webrtc::RtpPacketToSend> GetPacketAndMarkAsPending(
      uint16_t sequence_number,
      std::function<std::unique_ptr<webrtc::RtpPacketToSend>(
          const webrtc::RtpPacketToSend&)>
          encapsulate);

 private:
  std::unique_ptr<webrtc::RtpPacketToSend> RemovePacket(int packet_index);
  int GetPacketIndex(uint16_t sequence_number) const;

 private:
  class StoredPacket {
   public:
    StoredPacket() = default;
    StoredPacket(std::unique_ptr<webrtc::RtpPacketToSend> packet,
                 webrtc::Timestamp send_time, uint64_t insert_order);
    StoredPacket(StoredPacket&&);
    StoredPacket& operator=(StoredPacket&&);
    ~StoredPacket();

    uint64_t insert_order() const { return insert_order_; }
    size_t times_retransmitted() const { return times_retransmitted_; }
    void IncrementTimesRetransmitted();

    // The time of last transmission, including retransmissions.
    webrtc::Timestamp send_time() const { return send_time_; }
    void set_send_time(webrtc::Timestamp value) { send_time_ = value; }

    // The actual packet.
    std::unique_ptr<webrtc::RtpPacketToSend> packet_;

    // True if the packet is currently in the pacer queue pending transmission.
    bool pending_transmission_;

   private:
    webrtc::Timestamp send_time_ = webrtc::Timestamp::Zero();

    // Unique number per StoredPacket, incremented by one for each added
    // packet. Used to sort on insert order.
    uint64_t insert_order_;

    // Number of times RE-transmitted, ie excluding the first transmission.
    size_t times_retransmitted_;
  };

  void RemoveDeadPackets();
  bool VerifyRtt(const StoredPacket& packet) const;
  StoredPacket* GetStoredPacket(uint16_t sequence_number);

 private:
  std::shared_ptr<webrtc::Clock> clock_;
  std::deque<StoredPacket> packet_history_;
  uint64_t packets_inserted_;
  webrtc::TimeDelta rtt_;
  size_t number_to_store_;
};
}  // namespace minirtc
#endif