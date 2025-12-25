#include "rtp_data_sender.h"

#include <chrono>

#include "common.h"
#include "log.h"

#define RTCP_SR_INTERVAL 1000

namespace minirtc {

RtpDataSender::RtpDataSender() {}

RtpDataSender::RtpDataSender(std::shared_ptr<IOStatistics> io_statistics)
    : ssrc_(GenerateUniqueSsrc()), io_statistics_(io_statistics),
      last_update_time_(std::chrono::steady_clock::now()) {
  SetPeriod(std::chrono::milliseconds(5));
  SetThreadName("RtpDataSender");
}

RtpDataSender::~RtpDataSender() { SSRCManager::Instance().DeleteSsrc(ssrc_); }

void RtpDataSender::Enqueue(
    std::vector<std::unique_ptr<RtpPacket>>& rtp_packets) {
  for (auto& rtp_packet : rtp_packets) {
    rtp_packet_queue_.push(std::move(rtp_packet));
  }
}

void RtpDataSender::SetSendDataFunc(
    std::function<int(const char*, size_t)> data_send_func) {
  data_send_func_ = data_send_func;
}

void RtpDataSender::SetTargetBitrate(int64_t target_bitrate_bps) {
  target_bitrate_bps_.store(target_bitrate_bps);
  // Reset budget when bitrate changes
  if (target_bitrate_bps > 0) {
    int64_t max_bytes_in_budget = (kWindowMs * target_bitrate_bps) / 8000;
    bytes_remaining_ = std::min(bytes_remaining_, max_bytes_in_budget);
    bytes_remaining_ = std::max(bytes_remaining_, -max_bytes_in_budget);
  } else {
    bytes_remaining_ = 0;
  }
  last_update_time_ = std::chrono::steady_clock::now();
}

int RtpDataSender::SendRtpPacket(std::unique_ptr<RtpPacket> rtp_packet) {
  if (!data_send_func_) {
    LOG_ERROR("data_send_func_ is nullptr");
    return -1;
  }

  int ret = data_send_func_((const char*)rtp_packet->Buffer().data(),
                            rtp_packet->Size());
  if (-2 == ret) {
    rtp_packet_queue_.clear();
    return -1;
  }

  last_send_bytes_ += (uint32_t)rtp_packet->Size();
  total_rtp_payload_sent_ += (uint32_t)rtp_packet->PayloadSize();
  total_rtp_packets_sent_++;

  if (io_statistics_) {
    io_statistics_->UpdateDataOutboundBytes(last_send_bytes_);
    io_statistics_->IncrementDataOutboundRtpPacketCount();
  }

  // if (CheckIsTimeSendSR()) {
  //   SenderReport rtcp_sr;
  //   SenderInfo sender_info;
  //   RtcpReportBlock report;

  //   auto duration = std::chrono::system_clock::now().time_since_epoch();
  //   auto seconds =
  //   std::chrono::duration_cast<std::chrono::seconds>(duration); uint32_t
  //   seconds_u32 = static_cast<uint32_t>(
  //       std::chrono::duration_cast<std::chrono::seconds>(duration).count());

  //   uint32_t fraction_u32 = static_cast<uint32_t>(
  //       std::chrono::duration_cast<std::chrono::nanoseconds>(duration -
  //       seconds)
  //           .count());

  //   sender_info.sender_ssrc = 0x00;
  //   sender_info.ntp_ts_msw = (uint32_t)seconds_u32;
  //   sender_info.ntp_ts_lsw = (uint32_t)fraction_u32;
  //   sender_info.rtp_ts =
  //       std::chrono::system_clock::now().time_since_epoch().count() *
  //       1000000;
  //   sender_info.sender_packet_count = total_rtp_packets_sent_;
  //   sender_info.sender_octet_count = total_rtp_payload_sent_;

  //   rtcp_sr.SetSenderInfo(sender_info);

  //   report.source_ssrc = 0x00;
  //   report.fraction_lost = 0;
  //   report.cumulative_lost = 0;
  //   report.extended_high_seq_num = 0;
  //   report.jitter = 0;
  //   report.lsr = 0;
  //   report.dlsr = 0;

  //   rtcp_sr.SetReportBlock(report);

  //   rtcp_sr.Encode();

  //   // SendRtcpSR(rtcp_sr);
  // }

  return 0;
}

int RtpDataSender::SendRtcpSR(SenderReport& rtcp_sr) {
  if (!data_send_func_) {
    LOG_ERROR("data_send_func_ is nullptr");
    return -1;
  }

  if (data_send_func_((const char*)rtcp_sr.Buffer(), rtcp_sr.Size())) {
    LOG_ERROR("Send SR failed");
    return -1;
  }

  // LOG_ERROR("Send SR");

  return 0;
}

bool RtpDataSender::CheckIsTimeSendSR() {
  uint32_t now_ts = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  if (now_ts - last_send_rtcp_sr_packet_ts_ >= RTCP_SR_INTERVAL) {
    last_send_rtcp_sr_packet_ts_ = now_ts;
    return true;
  } else {
    return false;
  }
}

bool RtpDataSender::Process() {
  last_send_bytes_ = 0;

  // Update bandwidth budget
  int64_t target_bitrate = target_bitrate_bps_.load();
  if (target_bitrate > 0) {
    auto now = std::chrono::steady_clock::now();
    auto delta_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - last_update_time_)
                             .count();
    
    if (delta_time_ms > 0) {
      // Increase budget based on elapsed time
      int64_t bytes_to_add = (target_bitrate * delta_time_ms) / 8000;
      int64_t max_bytes_in_budget = (kWindowMs * target_bitrate) / 8000;
      
      if (bytes_remaining_ < 0) {
        // We overused last interval, compensate this interval
        bytes_remaining_ = std::min(bytes_remaining_ + bytes_to_add, max_bytes_in_budget);
      } else {
        // If we underused last interval, we can't use it this interval
        bytes_remaining_ = std::min(bytes_to_add, max_bytes_in_budget);
      }
      
      last_update_time_ = now;
    }
  }

  // Send packets with bandwidth limiting
  for (size_t i = 0; i < 10; i++) {
    if (!rtp_packet_queue_.isEmpty()) {
      std::optional<std::unique_ptr<RtpPacket>> rtp_packet =
          rtp_packet_queue_.pop();
      if (rtp_packet) {
        // Check bandwidth budget if limiting is enabled
        if (target_bitrate > 0) {
          size_t packet_size = (*rtp_packet)->Size();
          int64_t max_bytes_in_budget = (kWindowMs * target_bitrate) / 8000;
          
          // Check if we have enough budget
          if (bytes_remaining_ >= static_cast<int64_t>(packet_size)) {
            bytes_remaining_ -= static_cast<int64_t>(packet_size);
            SendRtpPacket(std::move(*rtp_packet));
          } else {
            // Not enough budget, allow sending but track overuse
            // This allows some burst but limits average rate
            bytes_remaining_ = std::max(bytes_remaining_ - static_cast<int64_t>(packet_size),
                                        -max_bytes_in_budget);
            SendRtpPacket(std::move(*rtp_packet));
          }
        } else {
          // No bandwidth limit, send as before
          SendRtpPacket(std::move(*rtp_packet));
        }
      }
    }
  }

  return true;
}
}  // namespace minirtc