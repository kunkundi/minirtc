/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-24
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_AUDIO_SENDER_H_
#define _RTP_AUDIO_SENDER_H_

#include <functional>

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
  RtpAudioSender(std::shared_ptr<IOStatistics> io_statistics);
  virtual ~RtpAudioSender();

 public:
  void Enqueue(std::vector<std::unique_ptr<RtpPacket>> &rtp_packets);
  void SetSendDataFunc(std::function<int(const char *, size_t)> data_send_func);
  uint32_t GetSsrc() { return ssrc_; }
  void OnReceiverReport(const ReceiverReport &receiver_report) {}

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
};
}  // namespace minirtc

#endif