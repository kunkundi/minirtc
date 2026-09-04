#include "video_channel_send.h"

#include "common.h"
#include "log.h"
#include "rtc_base/network/sent_packet.h"

// #define SAVE_RTP_SENT_STREAM
namespace minirtc {
namespace {

constexpr int64_t kSenderReportIntervalUs = 1'000'000;

}  // namespace

VideoChannelSend::VideoChannelSend(
    const std::string& channel_name, std::shared_ptr<SystemClock> clock,
    std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics)
    : channel_name_(channel_name),
      ice_agent_(ice_agent),
      ice_io_statistics_(ice_io_statistics),
      ssrc_(GenerateUniqueSsrc()),
      rtx_ssrc_(GenerateUniqueSsrc()),
      clock_(clock),
      rtp_timestamp_generator_(rtp::kVideoPayloadTypeFrequency,
                               GenerateRandomRtpTimestamp()),
      rtp_packet_history_(clock) {
#ifdef SAVE_RTP_SENT_STREAM
  file_rtp_sent_ = fopen("rtp_sent_stream.h264", "w+b");
  if (!file_rtp_sent_) {
    LOG_WARN("Fail to open rtp_sent_stream.h264");
  }
#endif
};

VideoChannelSend::~VideoChannelSend() {
  Destroy();
  SSRCManager::Instance().DeleteSsrc(ssrc_);
  SSRCManager::Instance().DeleteSsrc(rtx_ssrc_);
#ifdef SAVE_RTP_SENT_STREAM
  if (file_rtp_sent_) {
    fflush(file_rtp_sent_);
    fclose(file_rtp_sent_);
    file_rtp_sent_ = nullptr;
  }
#endif
}

void VideoChannelSend::Initialize(rtp::PAYLOAD_TYPE payload_type,
                                  std::shared_ptr<PacedSender> packet_sender) {
  Initialize(payload_type, std::move(packet_sender), false);
}

void VideoChannelSend::Initialize(rtp::PAYLOAD_TYPE payload_type,
                                  std::shared_ptr<PacedSender> packet_sender,
                                  bool rtx_enabled) {
  rtx_enabled_ = rtx_enabled;
  paced_sender_ = packet_sender;
  rtp_packetizer_ = RtpPacketizer::Create(payload_type, ssrc_);
  padding_packetizer_ = RtpPacketizer::Create(
      payload_type, rtx_enabled_ ? rtx_ssrc_ : ssrc_);
  rtp_packetizer_->SetAbsoluteSendTimeExtensionId(
      abs_send_time_ext_id_);
  padding_packetizer_->SetAbsoluteSendTimeExtensionId(
      abs_send_time_ext_id_);
  task_queue_history_ = std::make_shared<TaskQueue>("rtp pakcet history");
}

void VideoChannelSend::SetAbsoluteSendTimeExtensionId(
    std::optional<uint8_t> extension_id) {
  abs_send_time_ext_id_ = extension_id;
  if (rtp_packetizer_) {
    rtp_packetizer_->SetAbsoluteSendTimeExtensionId(extension_id);
  }
  if (padding_packetizer_) {
    padding_packetizer_->SetAbsoluteSendTimeExtensionId(extension_id);
  }
}

void VideoChannelSend::OnSentRtpPacket(
    std::unique_ptr<webrtc::RtpPacketToSend> packet) {
  if (!packet) {
    return;
  }

  MaybeSendSenderReport(*packet);

  if (!task_queue_history_ || history_shutdown_.load()) {
    return;
  }
  task_queue_history_->PostTask([this, packet = std::move(packet)]() mutable {
    if (packet->retransmitted_sequence_number()) {
      rtp_packet_history_.MarkPacketAsSent(
          packet->retransmitted_sequence_number().value());
    } else if (packet->PayloadType() != rtp::PAYLOAD_TYPE::H264 - 1) {
      rtp_packet_history_.PutRtpPacket(std::move(packet),
                                       clock_->CurrentTime());
    }
  });
}

void VideoChannelSend::MaybeSendSenderReport(
    const webrtc::RtpPacketToSend& packet) {
  if (!clock_ || !ice_agent_ || packet.Ssrc() != ssrc_ ||
      packet.retransmitted_sequence_number().has_value() ||
      packet.packet_type() != webrtc::RtpPacketMediaType::kVideo) {
    return;
  }

  const int64_t now_us = clock_->CurrentTimeUs();
  uint32_t packet_count = 0;
  uint32_t octet_count = 0;
  {
    std::lock_guard<std::mutex> lock(sender_report_mtx_);
    ++sender_packet_count_;
    sender_octet_count_ += static_cast<uint32_t>(packet.payload_size());
    if (last_sender_report_time_us_ != 0 &&
        now_us - last_sender_report_time_us_ < kSenderReportIntervalUs) {
      return;
    }
    last_sender_report_time_us_ = now_us;
    packet_count = sender_packet_count_;
    octet_count = sender_octet_count_;
  }

  SenderReport sender_report;
  sender_report.SetSenderSsrc(ssrc_);
  sender_report.SetNtpTimestamp(clock_->MonotonicTimeUsToNtp(now_us));
  sender_report.SetTimestamp(
      rtp_timestamp_generator_.TimestampForPaddingTimeUs(now_us));
  sender_report.SetSenderPacketCount(packet_count);
  sender_report.SetSenderOctetCount(octet_count);
  if (!sender_report.Build() ||
      ice_agent_->Send(reinterpret_cast<const char*>(sender_report.Buffer()),
                       sender_report.Size()) < 0) {
    LOG_WARN("Failed sending video RTCP sender report for SSRC {}", ssrc_);
  }
}

void VideoChannelSend::OnRtpPacketSendFailed(
    const webrtc::RtpPacketToSend& packet) {
  if (!task_queue_history_ || history_shutdown_.load()) {
    return;
  }

  if (packet.retransmitted_sequence_number().has_value()) {
    const uint16_t original_sequence_number =
        *packet.retransmitted_sequence_number();
    task_queue_history_->PostTask([this, original_sequence_number] {
      rtp_packet_history_.MarkPacketAsAborted(original_sequence_number);
    });
    return;
  }

  // A failed original media packet still owns a valid media sequence number.
  // Retain it so a gap observed after transport recovery can be repaired.
  if (packet.packet_type() == webrtc::RtpPacketMediaType::kVideo) {
    auto packet_copy =
        std::make_unique<webrtc::RtpPacketToSend>(packet);
    task_queue_history_->PostTask(
        [this, packet = std::move(packet_copy)]() mutable {
          rtp_packet_history_.PutRtpPacket(std::move(packet),
                                           clock_->CurrentTime());
        });
  }
}

void VideoChannelSend::OnReceiveNack(
    const std::vector<uint16_t>& nack_sequence_numbers) {
  // int64_t rtt = rtt_ms();
  // if (rtt == 0) {
  //   if (std::optional<webrtc::TimeDelta> average_rtt =
  //           rtcp_receiver_.AverageRtt()) {
  //     rtt = average_rtt->ms();
  //   }
  // }

  if (!rtx_enabled_ || !task_queue_history_ ||
      nack_sequence_numbers.empty()) {
    return;
  }

  bool schedule_task = false;
  {
    std::lock_guard<std::mutex> lock(pending_nacks_mtx_);
    if (history_shutdown_.load()) {
      return;
    }

    for (uint16_t sequence_number : nack_sequence_numbers) {
      if (pending_nack_sequence_numbers_.size() >=
          RtpPacketHistory::kMaxCapacity) {
        break;
      }
      pending_nack_sequence_numbers_.insert(sequence_number);
    }
    if (!nack_task_scheduled_ && !pending_nack_sequence_numbers_.empty()) {
      nack_task_scheduled_ = true;
      schedule_task = true;
    }
  }

  if (schedule_task &&
      !task_queue_history_->PostTask([this] { ProcessPendingNacks(); })) {
    std::lock_guard<std::mutex> lock(pending_nacks_mtx_);
    nack_task_scheduled_ = false;
  }
}

void VideoChannelSend::OnRttUpdate(int64_t rtt_ms) {
  if (rtt_ms <= 0 || rtt_ms > 2000 || !task_queue_history_ ||
      history_shutdown_.load()) {
    return;
  }

  task_queue_history_->PostTask([this, rtt_ms] {
    if (!history_shutdown_.load()) {
      rtp_packet_history_.SetRtt(webrtc::TimeDelta::Millis(rtt_ms));
    }
  });
}

void VideoChannelSend::ProcessPendingNacks() {
  constexpr size_t kMaxNacksPerTask = 512;
  std::vector<uint16_t> packet_ids;
  packet_ids.reserve(kMaxNacksPerTask);

  {
    std::lock_guard<std::mutex> lock(pending_nacks_mtx_);
    if (history_shutdown_.load()) {
      pending_nack_sequence_numbers_.clear();
      nack_task_scheduled_ = false;
      return;
    }

    auto it = pending_nack_sequence_numbers_.begin();
    while (it != pending_nack_sequence_numbers_.end() &&
           packet_ids.size() < kMaxNacksPerTask) {
      packet_ids.push_back(*it);
      it = pending_nack_sequence_numbers_.erase(it);
    }
  }

  for (uint16_t seq_no : packet_ids) {
    if (history_shutdown_.load()) {
      break;
    }
    if (ReSendPacket(seq_no) < 0) {
      LOG_WARN("Failed resending RTP packet {}", seq_no);
    }
  }

  bool reschedule = false;
  {
    std::lock_guard<std::mutex> lock(pending_nacks_mtx_);
    if (history_shutdown_.load() || pending_nack_sequence_numbers_.empty()) {
      pending_nack_sequence_numbers_.clear();
      nack_task_scheduled_ = false;
    } else {
      reschedule = true;
    }
  }

  if (reschedule &&
      !task_queue_history_->PostTask([this] { ProcessPendingNacks(); })) {
    std::lock_guard<std::mutex> lock(pending_nacks_mtx_);
    nack_task_scheduled_ = false;
  }
}

std::vector<std::unique_ptr<RtpPacket>> VideoChannelSend::GeneratePadding(
    uint32_t payload_size, int64_t padding_time_us) {
  if (padding_packetizer_) {
    const uint32_t rtp_timestamp =
        rtp_timestamp_generator_.TimestampForPaddingTimeUs(padding_time_us);
    return padding_packetizer_->BuildPadding(
        payload_size, rtp_timestamp, true);
  }
  return std::vector<std::unique_ptr<RtpPacket>>{};
}

void VideoChannelSend::Destroy() {
  if (task_queue_history_) {
    history_shutdown_.store(true);
    {
      std::lock_guard<std::mutex> lock(pending_nacks_mtx_);
      pending_nack_sequence_numbers_.clear();
      nack_task_scheduled_ = false;
    }
    // At teardown retransmissions are no longer useful. Drop queued work and
    // wait only for the currently executing history operation to finish.
    task_queue_history_->ClearTasks();
    task_queue_history_->Stop();
  }
}

int VideoChannelSend::SendVideo(const EncodedFrame& encoded_frame) {
  if (rtp_packetizer_ && paced_sender_) {
    const bool is_key_frame =
        encoded_frame.FrameType() == VideoFrameType::kVideoFrameKey;
    rtp_packetizer_->SetIsKeyFrame(is_key_frame);
    const uint32_t rtp_timestamp =
        rtp_timestamp_generator_.TimestampForCaptureTimeUs(
            encoded_frame.CapturedTimestamp());
    std::vector<std::unique_ptr<RtpPacket>> rtp_packets =
        rtp_packetizer_->Build((uint8_t*)encoded_frame.Buffer(),
                               (uint32_t)encoded_frame.Size(), rtp_timestamp,
                               true);
    for (size_t index = 0; index < rtp_packets.size(); ++index) {
      auto* packet_to_send =
          static_cast<webrtc::RtpPacketToSend*>(rtp_packets[index].get());
      packet_to_send->set_is_key_frame(is_key_frame);
      packet_to_send->set_first_packet_of_frame(index == 0);
    }

#ifdef SAVE_RTP_SENT_STREAM
    fwrite((unsigned char*)encoded_frame.Buffer(), 1, encoded_frame.Size(),
           file_rtp_sent_);
#endif
    paced_sender_->EnqueueRtpPackets(
        rtp_packets, encoded_frame.CapturedTimestamp(), channel_name_);
  }

  return 0;
}

int32_t VideoChannelSend::ReSendPacket(uint16_t packet_id) {
  if (!rtx_enabled_ || !paced_sender_) {
    return -1;
  }

  int32_t packet_size = 0;
  std::unique_ptr<webrtc::RtpPacketToSend> packet =
      rtp_packet_history_.GetPacketAndMarkAsPending(
          packet_id, [&](const webrtc::RtpPacketToSend& stored_packet) {
            packet_size = stored_packet.size();
            auto retransmit_packet =
                std::make_unique<webrtc::RtpPacketToSend>(stored_packet);

            const uint16_t original_sequence_number =
                stored_packet.SequenceNumber();
            retransmit_packet->SetSsrc(rtx_ssrc_);
            retransmit_packet->SetPayloadType(rtp::PAYLOAD_TYPE::RTX);
            retransmit_packet->set_retransmitted_sequence_number(
                original_sequence_number);
            retransmit_packet->set_original_ssrc(stored_packet.Ssrc());
            if (!retransmit_packet->BuildRtxPacket()) {
              return std::unique_ptr<webrtc::RtpPacketToSend>();
            }
            return retransmit_packet;
          });
  if (packet_size == 0) {
    // Packet was not found, is already pending, or was sent too recently.
    return 0;
  }
  if (!packet) {
    LOG_WARN("Failed building RTX packet for OSN {}", packet_id);
    return -1;
  }

  packet->set_packet_type(webrtc::RtpPacketMediaType::kRetransmission);
  packet->set_fec_protect_packet(false);
  const int enqueue_result =
      paced_sender_->EnqueueRtpPacket(std::move(packet));
  if (enqueue_result < 0) {
    rtp_packet_history_.MarkPacketAsAborted(packet_id);
  }
  return enqueue_result;
}
}  // namespace minirtc
