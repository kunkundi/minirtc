#include "rtp_audio_sender.h"

#include <chrono>

#include "common.h"
#include "log.h"
#include "rtp_timestamp.h"

namespace minirtc {
namespace {

constexpr int64_t kSenderReportIntervalUs = 1'000'000;
constexpr uint32_t kOpusRtpClockRate = 48'000;

}  // namespace

RtpAudioSender::RtpAudioSender() { SetPeriod(std::chrono::milliseconds(5)); }

RtpAudioSender::RtpAudioSender(std::shared_ptr<SystemClock> clock,
                               std::shared_ptr<IOStatistics> io_statistics)
    : ssrc_(GenerateUniqueSsrc()),
      clock_(clock),
      io_statistics_(io_statistics) {
  SetPeriod(std::chrono::milliseconds(5));
  SetThreadName("RtpAudioSender");
}

RtpAudioSender::~RtpAudioSender() { SSRCManager::Instance().DeleteSsrc(ssrc_); }

void RtpAudioSender::Enqueue(
    std::vector<std::unique_ptr<RtpPacket>>& rtp_packets,
    int64_t media_time_us) {
  for (auto& rtp_packet : rtp_packets) {
    rtp_packet_queue_.push({std::move(rtp_packet), media_time_us});
  }
}

void RtpAudioSender::SetSendDataFunc(
    std::function<int(const char*, size_t)> data_send_func) {
  data_send_func_ = data_send_func;
}

int RtpAudioSender::SendRtpPacket(QueuedAudioPacket queued_packet) {
  if (!data_send_func_ || !queued_packet.packet) {
    LOG_ERROR("data_send_func_ is nullptr");
    return -1;
  }

  RtpPacket& rtp_packet = *queued_packet.packet;
  const int64_t send_time_us = clock_ ? clock_->CurrentTimeUs() : 0;
  if (clock_ && abs_send_time_ext_id_.has_value()) {
    const uint64_t send_time_ntp =
        clock_->MonotonicTimeUsToNtp(send_time_us);
    if (!rtp_packet.UpdateAbsoluteSendTimestamp(
            *abs_send_time_ext_id_,
            SystemClock::NtpToAbsoluteSendTime(send_time_ntp))) {
      LOG_ERROR("Failed updating audio Absolute Send Time extension");
      return -1;
    }
  }

  int ret = data_send_func_((const char*)rtp_packet.Buffer().data(),
                            rtp_packet.Size());
  if (-2 == ret) {
    rtp_packet_queue_.clear();
    return -1;
  }
  if (ret < 0) {
    return ret;
  }

  total_rtp_payload_sent_ += (uint32_t)rtp_packet.PayloadSize();
  total_rtp_packets_sent_++;

  if (io_statistics_) {
    io_statistics_->UpdateAudioOutboundBytes(
        static_cast<uint32_t>(rtp_packet.Size()));
    io_statistics_->IncrementAudioOutboundRtpPacketCount();
  }

  if (clock_) {
    const int64_t now_us = send_time_us;
    if (last_sender_report_time_us_ == 0 ||
        now_us - last_sender_report_time_us_ >= kSenderReportIntervalUs) {
      last_sender_report_time_us_ = now_us;
      SenderReport sender_report;
      sender_report.SetSenderSsrc(ssrc_);
      sender_report.SetNtpTimestamp(
          clock_->MonotonicTimeUsToNtp(now_us));
      sender_report.SetTimestamp(ExtrapolateRtpTimestamp(
          rtp_packet.Timestamp(), queued_packet.media_time_us, now_us,
          kOpusRtpClockRate));
      sender_report.SetSenderPacketCount(total_rtp_packets_sent_);
      sender_report.SetSenderOctetCount(total_rtp_payload_sent_);
      sender_report.Build();
      SendRtcpSR(sender_report);
    }
  }

  return 0;
}

int RtpAudioSender::SendRtcpSR(SenderReport& rtcp_sr) {
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

bool RtpAudioSender::Process() {
  for (size_t i = 0; i < 10; i++)
    if (!rtp_packet_queue_.isEmpty()) {
      std::optional<QueuedAudioPacket> queued_packet = rtp_packet_queue_.pop();
      if (queued_packet) {
        SendRtpPacket(std::move(*queued_packet));
      }
    }

  return true;
}
}  // namespace minirtc
