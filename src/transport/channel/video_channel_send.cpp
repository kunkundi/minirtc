#include "video_channel_send.h"

#include "common.h"
#include "log.h"
#include "rtc_base/network/sent_packet.h"

// #define SAVE_RTP_SENT_STREAM

VideoChannelSend::VideoChannelSend(
    const std::string& channel_name, std::shared_ptr<SystemClock> clock,
    std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics)
    : channel_name_(channel_name),
      ice_agent_(ice_agent),
      ice_io_statistics_(ice_io_statistics),
      ssrc_(GenerateUniqueSsrc()),
      rtx_ssrc_(GenerateUniqueSsrc()),
      delta_ntp_internal_ms_(clock->CurrentNtpInMilliseconds() -
                             clock->CurrentTimeMs()),
      rtp_packet_history_(clock),
      clock_(clock) {
#ifdef SAVE_RTP_SENT_STREAM
  file_rtp_sent_ = fopen("rtp_sent_stream.h264", "w+b");
  if (!file_rtp_sent_) {
    LOG_WARN("Fail to open rtp_sent_stream.h264");
  }
#endif
};

VideoChannelSend::~VideoChannelSend() {
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
  paced_sender_ = packet_sender;
  rtp_packetizer_ = RtpPacketizer::Create(payload_type, ssrc_);
  task_queue_history_ = std::make_shared<TaskQueue>("rtp pakcet history");
}

void VideoChannelSend::OnSentRtpPacket(
    std::unique_ptr<webrtc::RtpPacketToSend> packet) {
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

void VideoChannelSend::OnReceiveNack(
    const std::vector<uint16_t>& nack_sequence_numbers) {
  // int64_t rtt = rtt_ms();
  // if (rtt == 0) {
  //   if (std::optional<webrtc::TimeDelta> average_rtt =
  //           rtcp_receiver_.AverageRtt()) {
  //     rtt = average_rtt->ms();
  //   }
  // }

  int64_t avg_rtt = 10;

  task_queue_history_->PostTask([this, avg_rtt]() {
    rtp_packet_history_.SetRtt(TimeDelta::Millis(5 + avg_rtt));
  });

  for (uint16_t seq_no : nack_sequence_numbers) {
    const int32_t bytes_sent = ReSendPacket(seq_no);
    if (bytes_sent < 0) {
      // Failed to send one Sequence number. Give up the rest in this nack.
      LOG_WARN("Failed resending RTP packet {}, Discard rest of packets",
               seq_no);
      break;
    }
  }
}

std::vector<std::unique_ptr<RtpPacket>> VideoChannelSend::GeneratePadding(
    uint32_t payload_size, int64_t captured_timestamp_us) {
  if (rtp_packetizer_) {
    return rtp_packetizer_->BuildPadding(payload_size, captured_timestamp_us,
                                         true);
  }
  return std::vector<std::unique_ptr<RtpPacket>>{};
}

void VideoChannelSend::Destroy() {}

int VideoChannelSend::SendVideo(const EncodedFrame& encoded_frame) {
  if (rtp_packetizer_ && paced_sender_) {
    uint32_t rtp_timestamp =
        delta_ntp_internal_ms_ +
        static_cast<uint32_t>(encoded_frame.CapturedTimestamp() / 1000);
    std::vector<std::unique_ptr<RtpPacket>> rtp_packets =
        rtp_packetizer_->Build((uint8_t*)encoded_frame.Buffer(),
                               (uint32_t)encoded_frame.Size(), rtp_timestamp,
                               true);

#ifdef SAVE_RTP_SENT_STREAM
    fwrite((unsigned char*)encoded_frame.Buffer(), 1, encoded_frame.Size(),
           file_rtp_sent_);
#endif
    paced_sender_->EnqueueRtpPackets(rtp_packets, rtp_timestamp, channel_name_);
  }

  return 0;
}

int32_t VideoChannelSend::ReSendPacket(uint16_t packet_id) {
  task_queue_history_->PostTask([this, packet_id]() {
    int32_t packet_size = 0;
    std::unique_ptr<webrtc::RtpPacketToSend> packet =
        rtp_packet_history_.GetPacketAndMarkAsPending(
            packet_id, [&](const webrtc::RtpPacketToSend& stored_packet) {
              // Check if we're overusing retransmission bitrate.
              // TODO(sprang): Add histograms for nack success or failure
              // reasons.
              packet_size = stored_packet.size();
              std::unique_ptr<webrtc::RtpPacketToSend> retransmit_packet;

              retransmit_packet =
                  std::make_unique<webrtc::RtpPacketToSend>(stored_packet);

              retransmit_packet->SetSsrc(rtx_ssrc_);
              retransmit_packet->SetPayloadType(rtp::PAYLOAD_TYPE::RTX);

              retransmit_packet->set_retransmitted_sequence_number(
                  stored_packet.SequenceNumber());
              retransmit_packet->set_original_ssrc(stored_packet.Ssrc());
              retransmit_packet->BuildRtxPacket();

              return retransmit_packet;
            });
    if (packet_size == 0) {
      // Packet not found or already queued for retransmission, ignore.
      return;
    }
    if (!packet) {
      // Packet was found, but lambda helper above chose not to create
      // `retransmit_packet` out of it.
      LOG_WARN("packet not found");
      return;
    }

    packet->set_packet_type(webrtc::RtpPacketMediaType::kRetransmission);
    packet->set_fec_protect_packet(false);

    if (paced_sender_) {
      paced_sender_->EnqueueRtpPacket(std::move(packet));
    }
  });

  return 0;
}