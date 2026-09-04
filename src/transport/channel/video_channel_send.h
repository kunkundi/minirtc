/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-03
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_CHANNEL_SEND_H_
#define _VIDEO_CHANNEL_SEND_H_

#include <atomic>
#include <mutex>
#include <unordered_set>

#include "api/transport/network_types.h"
#include "api/units/timestamp.h"
#include "clock/system_clock.h"
#include "congestion_control.h"
#include "congestion_control_feedback.h"
#include "encoded_frame.h"
#include "ice_agent.h"
#include "media_channel.h"
#include "paced_sender.h"
#include "rtp_packet_history.h"
#include "rtp_packetizer.h"
#include "rtp_timestamp.h"
#include "rtp_video_sender.h"
#include "task_queue.h"
#include "transport_feedback_adapter.h"

namespace minirtc {

class VideoChannelSend : public MediaChannel {
 public:
  VideoChannelSend(const std::string& channel_name,
                   std::shared_ptr<SystemClock> clock,
                   std::shared_ptr<IceAgent> ice_agent,
                   std::shared_ptr<IOStatistics> ice_io_statistics);
  ~VideoChannelSend() override;

  void OnSentRtpPacket(
      std::unique_ptr<webrtc::RtpPacketToSend> packet) override;

  void OnRtpPacketSendFailed(
      const webrtc::RtpPacketToSend& packet) override;

  void OnReceiveNack(
      const std::vector<uint16_t>& nack_sequence_numbers) override;

  void OnRttUpdate(int64_t rtt_ms) override;

  std::vector<std::unique_ptr<RtpPacket>> GeneratePadding(
      uint32_t payload_size, int64_t padding_time_us) override;

  bool CanGeneratePadding() const override {
    return padding_packetizer_ != nullptr;
  }

 public:
  void Initialize(rtp::PAYLOAD_TYPE payload_type,
                  std::shared_ptr<PacedSender> packet_sender) override;
  void Initialize(rtp::PAYLOAD_TYPE payload_type,
                  std::shared_ptr<PacedSender> packet_sender,
                  bool rtx_enabled);
  void Destroy() override;

  uint32_t GetSsrc() override { return ssrc_; }

  uint32_t GetRtxSsrc() override { return rtx_ssrc_; }

  int SendVideo(const EncodedFrame& encoded_frame) override;

  void OnReceiverReport(const ReceiverReport& receiver_report) override {
    std::vector<RtcpReportBlock> reports = receiver_report.GetReportBlocks();
    for (auto r : reports) {
      LOG_WARN(
          "r_ssrc [{}], f_lost [{}], c_lost [{}], h_seq [{}], jitter [{}], "
          "lsr [{}], dlsr [{}] ",
          r.SourceSsrc(), r.FractionLost() / 255.0, r.CumulativeLost(),
          r.ExtendedHighSeqNum(), r.Jitter(), r.LastSr(), r.DelaySinceLastSr());
    }
  }

 private:
  int32_t ReSendPacket(uint16_t packet_id);
  void ProcessPendingNacks();

 private:
  std::string channel_name_;
  std::shared_ptr<PacedSender> paced_sender_ = nullptr;
  std::shared_ptr<IceAgent> ice_agent_ = nullptr;
  std::shared_ptr<IOStatistics> ice_io_statistics_ = nullptr;
  std::unique_ptr<RtpPacketizer> rtp_packetizer_ = nullptr;
  std::unique_ptr<RtpPacketizer> padding_packetizer_ = nullptr;

 private:
  uint32_t ssrc_ = 0;
  uint32_t rtx_ssrc_ = 0;
  bool rtx_enabled_ = false;
  std::shared_ptr<SystemClock> clock_;
  RtpTimestampGenerator rtp_timestamp_generator_;
  RtpPacketHistory rtp_packet_history_;
  std::mutex pending_nacks_mtx_;
  std::unordered_set<uint16_t> pending_nack_sequence_numbers_;
  bool nack_task_scheduled_ = false;
  std::atomic<bool> history_shutdown_{false};
  // Keep the queue after the state it operates on: members are destroyed in
  // reverse declaration order, so the worker is stopped before packet history
  // is released even if Destroy() was not called explicitly.
  std::shared_ptr<TaskQueue> task_queue_history_;
 private:
  FILE* file_rtp_sent_ = nullptr;
};
}  // namespace minirtc

#endif
