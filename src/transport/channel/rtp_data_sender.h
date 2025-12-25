/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-24
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_DATA_SENDER_H_
#define _RTP_DATA_SENDER_H_

#include <functional>

#include "io_statistics.h"
#include "receiver_report.h"
#include "ringbuffer.h"
#include "rtp_packet.h"
#include "sender_report.h"
#include "thread_base.h"
#include <chrono>
#include <atomic>

namespace minirtc {

class RtpDataSender : public ThreadBase {
 public:
  RtpDataSender();
  RtpDataSender(std::shared_ptr<IOStatistics> io_statistics);
  virtual ~RtpDataSender();

 public:
  void Enqueue(std::vector<std::unique_ptr<RtpPacket>> &rtp_packets);
  void SetSendDataFunc(std::function<int(const char *, size_t)> data_send_func);
  void SetTargetBitrate(int64_t target_bitrate_bps);
  uint32_t GetSsrc() { return ssrc_; }
  void OnReceiverReport(const ReceiverReport &receiver_report) {}

 private:
 private:
  int SendRtpPacket(std::unique_ptr<RtpPacket> rtp_packet);
  int SendRtcpSR(SenderReport &rtcp_sr);

  bool CheckIsTimeSendSR();

 private:
  bool Process() override;

 private:
  std::function<int(const char *, size_t)> data_send_func_ = nullptr;
  RingBuffer<std::unique_ptr<RtpPacket>> rtp_packet_queue_;

 private:
  uint32_t ssrc_ = 0;
  std::shared_ptr<IOStatistics> io_statistics_ = nullptr;
  uint32_t last_send_bytes_ = 0;
  uint32_t total_rtp_payload_sent_ = 0;
  uint32_t total_rtp_packets_sent_ = 0;
  uint32_t last_send_rtcp_sr_packet_ts_ = 0;
  
  // Bandwidth limiting
  std::atomic<int64_t> target_bitrate_bps_{0};  // 0 means no limit
  int64_t bytes_remaining_ = 0;
  std::chrono::steady_clock::time_point last_update_time_;
  static constexpr int64_t kWindowMs = 500;  // 500ms window for bandwidth budget
};
}  // namespace minirtc

#endif