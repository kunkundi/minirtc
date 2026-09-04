/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-24
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_AUDIO_SENDER_H_
#define _RTP_AUDIO_SENDER_H_

#include <functional>
#include <optional>

#include "clock/system_clock.h"
#include "io_statistics.h"
#include "receiver_report.h"
#include "ringbuffer.h"
#include "rtp_packet.h"
#include "sender_report.h"
#include "thread_base.h"

namespace minirtc {

class RtpAudioSender : public ThreadBase {
 public:
  RtpAudioSender();
  RtpAudioSender(std::shared_ptr<SystemClock> clock,
                 std::shared_ptr<IOStatistics> io_statistics);
  virtual ~RtpAudioSender();

 public:
  void Enqueue(std::vector<std::unique_ptr<RtpPacket>> &rtp_packets,
               int64_t media_time_us);
  void SetSendDataFunc(std::function<int(const char *, size_t)> data_send_func);
  uint32_t GetSsrc() { return ssrc_; }
  void SetAbsoluteSendTimeExtensionId(
      std::optional<uint8_t> extension_id) {
    abs_send_time_ext_id_ = extension_id;
  }
  void OnReceiverReport(const ReceiverReport &receiver_report) {}

 private:
  struct QueuedAudioPacket {
    std::unique_ptr<RtpPacket> packet;
    int64_t media_time_us = 0;
  };

  int SendRtpPacket(QueuedAudioPacket queued_packet);
  int SendRtcpSR(SenderReport &rtcp_sr);

 private:
  bool Process() override;

 private:
  std::function<int(const char *, size_t)> data_send_func_ = nullptr;
  RingBuffer<QueuedAudioPacket> rtp_packet_queue_;

 private:
  uint32_t ssrc_ = 0;
  std::shared_ptr<SystemClock> clock_ = nullptr;
  std::shared_ptr<IOStatistics> io_statistics_ = nullptr;
  uint32_t total_rtp_payload_sent_ = 0;
  uint32_t total_rtp_packets_sent_ = 0;
  int64_t last_sender_report_time_us_ = 0;
  std::optional<uint8_t> abs_send_time_ext_id_;
};
}  // namespace minirtc

#endif
