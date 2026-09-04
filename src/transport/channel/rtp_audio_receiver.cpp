#include "rtp_audio_receiver.h"

#define RTCP_RR_INTERVAL 1000

namespace minirtc {

RtpAudioReceiver::RtpAudioReceiver() {}

RtpAudioReceiver::RtpAudioReceiver(
    uint32_t remote_ssrc, std::shared_ptr<SystemClock> clock,
    std::shared_ptr<IOStatistics> io_statistics)
    : io_statistics_(io_statistics),
      clock_(clock),
      remote_ssrc_(remote_ssrc) {}

RtpAudioReceiver::~RtpAudioReceiver() {}

void RtpAudioReceiver::InsertRtpPacket(RtpPacket& rtp_packet) {
  if (clock_) {
    last_mapped_capture_time_us_.store(rtp_timestamp_mapper_.ToLocalTimeUs(
        rtp_packet.Timestamp(), clock_->CurrentTimeUs()));
  }

  last_recv_bytes_ = (uint32_t)rtp_packet.Size();
  total_rtp_payload_recv_ += (uint32_t)rtp_packet.PayloadSize();
  total_rtp_packets_recv_++;

  if (io_statistics_) {
    io_statistics_->UpdateAudioInboundBytes(last_recv_bytes_);
    io_statistics_->IncrementAudioInboundRtpPacketCount();
    io_statistics_->UpdateAudioPacketLossCount(rtp_packet.SequenceNumber());
  }

  // if (CheckIsTimeSendRR()) {
  //   ReceiverReport rtcp_rr;
  //   RtcpReportBlock report;

  //   // auto duration = std::chrono::system_clock::now().time_since_epoch();
  //   // auto seconds =
  //   // std::chrono::duration_cast<std::chrono::seconds>(duration); uint32_t
  //   // seconds_u32 = static_cast<uint32_t>(
  //   // std::chrono::duration_cast<std::chrono::seconds>(duration).count());

  //   // uint32_t fraction_u32 = static_cast<uint32_t>(
  //   //     std::chrono::duration_cast<std::chrono::nanoseconds>(duration -
  //   //     seconds)
  //   //         .count());

  //   report.source_ssrc = 0x00;
  //   report.fraction_lost = 0;
  //   report.cumulative_lost = 0;
  //   report.extended_high_seq_num = 0;
  //   report.jitter = 0;
  //   report.lsr = 0;
  //   report.dlsr = 0;

  //   rtcp_rr.SetReportBlock(report);

  //   rtcp_rr.Encode();

  //   // SendRtcpRR(rtcp_rr);
  // }

  if (on_receive_data_) {
    on_receive_data_((const char*)rtp_packet.Payload(),
                     rtp_packet.PayloadSize());
  }
}

void RtpAudioReceiver::OnSenderReport(const SenderReport& sender_report) {
  if (!clock_ || sender_report.SenderSsrc() != remote_ssrc_ ||
      sender_report.NtpTimestamp() == 0) {
    return;
  }

  rtp_timestamp_mapper_.UpdateFromSenderReport(
      sender_report.Timestamp(),
      clock_->NtpToMonotonicTimeUs(sender_report.NtpTimestamp()));
  last_sr_ = sender_report.CompactNtpTimestamp();
  last_sender_report_arrival_ntp_ =
      SystemClock::CompactNtp(clock_->CurrentNtpTime());
}

void RtpAudioReceiver::SetSendDataFunc(
    std::function<int(const char*, size_t)> data_send_func) {
  data_send_func_ = data_send_func;
}

int RtpAudioReceiver::SendRtcpRR(ReceiverReport& rtcp_rr) {
  if (!data_send_func_) {
    LOG_ERROR("data_send_func_ is nullptr");
    return -1;
  }

  if (data_send_func_((const char*)rtcp_rr.Buffer(), rtcp_rr.Size())) {
    LOG_ERROR("Send RR failed");
    return -1;
  }

  // LOG_ERROR("Send RR");

  return 0;
}

bool RtpAudioReceiver::CheckIsTimeSendRR() {
  uint32_t now_ts = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  if (now_ts - last_send_rtcp_rr_packet_ts_ >= RTCP_RR_INTERVAL) {
    last_send_rtcp_rr_packet_ts_ = now_ts;
    return true;
  } else {
    return false;
  }
}
}  // namespace minirtc