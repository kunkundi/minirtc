
#include "paced_sender.h"

#include <chrono>

#include "log.h"

namespace minirtc {

const int PacedSender::kNoPacketHoldback = -1;

PacedSender::PacedSender(std::shared_ptr<IceAgent> ice_agent,
                         std::shared_ptr<webrtc::Clock> clock,
                         std::shared_ptr<TaskQueue> task_queue)
    : ice_agent_(ice_agent),
      clock_(clock),
      pacing_controller_(clock.get(), this),
      max_hold_back_window_(webrtc::TimeDelta::Millis(5)),
      max_hold_back_window_in_packets_(3),
      next_process_time_(webrtc::Timestamp::MinusInfinity()),
      is_started_(false),
      transport_ready_(false),
      is_shutdown_(false),
      packet_size_(/*alpha=*/0.95),
      include_overhead_(false),
      last_send_time_(webrtc::Timestamp::Millis(0)),
      last_call_time_(webrtc::Timestamp::Millis(0)),
      task_queue_pacer_(task_queue) {}

PacedSender::~PacedSender() { is_shutdown_ = true; }

void PacedSender::RunOrPost(AnyInvocable<void()> task) {
  if (task_queue_pacer_->IsCurrent()) {
    task();
    return;
  }
  if (!task_queue_pacer_->PostTask(std::move(task))) {
    LOG_WARN("PacedSender task rejected: pacer task queue is stopped");
  }
}

void PacedSender::SetOnSentPacketFunc(
    std::function<void(std::unique_ptr<webrtc::RtpPacketToSend>)>
        on_sent_packet_func) {
  RunOrPost([this, on_sent_packet_func = std::move(on_sent_packet_func)]()
                mutable {
    on_sent_packet_func_ = std::move(on_sent_packet_func);
  });
}

void PacedSender::SetGeneratePaddingFunc(
    std::function<std::vector<std::unique_ptr<RtpPacket>>(uint32_t, int64_t)>
        generat_padding_func) {
  RunOrPost([this, generat_padding_func = std::move(generat_padding_func)]()
                mutable {
    generat_padding_func_ = std::move(generat_padding_func);
  });
}

std::vector<std::unique_ptr<webrtc::RtpPacketToSend>>
PacedSender::GeneratePadding(webrtc::DataSize size) {
  std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> to_send_rtp_packets;
  std::vector<std::unique_ptr<RtpPacket>> rtp_packets =
      generat_padding_func_(size.bytes(), clock_->CurrentTime().ms());
  for (auto &packet : rtp_packets) {
    std::unique_ptr<webrtc::RtpPacketToSend> rtp_packet_to_send(
        static_cast<webrtc::RtpPacketToSend *>(packet.release()));

    rtp_packet_to_send->set_capture_time(clock_->CurrentTime());
    rtp_packet_to_send->set_transport_sequence_number((transport_seq_)++);
    rtp_packet_to_send->set_packet_type(webrtc::RtpPacketMediaType::kPadding);

    to_send_rtp_packets.push_back(std::move(rtp_packet_to_send));
  }

  return to_send_rtp_packets;
}

void PacedSender::SetSendBurstInterval(webrtc::TimeDelta burst_interval) {
  RunOrPost([this, burst_interval] {
    pacing_controller_.SetSendBurstInterval(burst_interval);
  });
}

void PacedSender::SetAllowProbeWithoutMediaPacket(bool allow) {
  RunOrPost([this, allow] {
    pacing_controller_.SetAllowProbeWithoutMediaPacket(allow);
    MaybeScheduleProcessPackets();
  });
}

void PacedSender::SetTransportReady(bool ready) {
  RunOrPost([this, ready] {
    transport_ready_ = ready;
    if (ready) {
      pacing_controller_.Resume();
      return;
    }

    pacing_controller_.AbortProbing("transport_not_ready");
    pacing_controller_.Pause();
    next_process_time_ = webrtc::Timestamp::MinusInfinity();
  });
}

void PacedSender::EnsureStarted() {
  RunOrPost([this] {
    is_started_ = true;
    MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
  });
}

void PacedSender::CreateProbeClusters(
    std::vector<webrtc::ProbeClusterConfig> probe_cluster_configs) {
  RunOrPost([this, probe_cluster_configs = std::move(probe_cluster_configs)]()
                mutable {
    if (!transport_ready_) {
      for (const auto& config : probe_cluster_configs) {
        LOG_WARN(
            "Probe cluster discarded: id={} reason=transport_not_ready "
            "target_bitrate_bps={} target_bytes={} target_packets={}",
            config.id, config.target_data_rate.bps(),
            (config.target_data_rate * config.target_duration).bytes(),
            config.target_probe_count);
      }
      return;
    }
    pacing_controller_.CreateProbeClusters(probe_cluster_configs);
    MaybeScheduleProcessPackets();
  });
}

void PacedSender::Pause() {
  RunOrPost([this] { pacing_controller_.Pause(); });
}

void PacedSender::Resume() {
  RunOrPost([this] {
    pacing_controller_.Resume();
    MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
  });
}

void PacedSender::SetCongested(bool congested) {
  RunOrPost([this, congested] {
    pacing_controller_.SetCongested(congested);
    MaybeScheduleProcessPackets();
  });
}

void PacedSender::SetPacingRates(webrtc::DataRate pacing_rate,
                                 webrtc::DataRate padding_rate) {
  RunOrPost([this, pacing_rate, padding_rate] {
    pacing_controller_.SetPacingRates(pacing_rate, padding_rate);
    MaybeScheduleProcessPackets();
  });
}

void PacedSender::EnqueuePackets(
    std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> packets) {
  RunOrPost([this, packets = std::move(packets)]() mutable {
    EnqueuePacketsOnQueue(std::move(packets));
  });
}

void PacedSender::EnqueuePacket(
    std::unique_ptr<webrtc::RtpPacketToSend> packet) {
  RunOrPost([this, packet = std::move(packet)]() mutable {
    EnqueuePacketOnQueue(std::move(packet));
    MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
  });
}

void PacedSender::EnqueuePacketsOnQueue(
    std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> packets) {
  for (auto& packet : packets) {
    EnqueuePacketOnQueue(std::move(packet));
  }
  MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
}

void PacedSender::EnqueuePacketOnQueue(
    std::unique_ptr<webrtc::RtpPacketToSend> packet) {
  packet->set_transport_sequence_number(transport_seq_++);
  size_t packet_size = packet->payload_size() + packet->padding_size();
  if (include_overhead_) {
    packet_size += packet->headers_size();
  }
  packet_size_.Apply(1, packet_size);
  pacing_controller_.EnqueuePacket(std::move(packet));
}

void PacedSender::RemovePacketsForSsrc(uint32_t ssrc) {
  RunOrPost([this, ssrc] {
    pacing_controller_.RemovePacketsForSsrc(ssrc);
    MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
  });
}

void PacedSender::SetAccountForAudioPackets(bool account_for_audio) {
  RunOrPost([this, account_for_audio] {
    pacing_controller_.SetAccountForAudioPackets(account_for_audio);
    MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
  });
}

void PacedSender::SetIncludeOverhead() {
  RunOrPost([this] {
    include_overhead_ = true;
    pacing_controller_.SetIncludeOverhead();
    MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
  });
}

void PacedSender::SetTransportOverhead(
    webrtc::DataSize overhead_per_packet) {
  RunOrPost([this, overhead_per_packet] {
    pacing_controller_.SetTransportOverhead(overhead_per_packet);
    MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
  });
}

void PacedSender::SetQueueTimeLimit(webrtc::TimeDelta limit) {
  RunOrPost([this, limit] {
    pacing_controller_.SetQueueTimeLimit(limit);
    MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
  });
}

webrtc::TimeDelta PacedSender::ExpectedQueueTime() const {
  return GetStats().expected_queue_time;
}

webrtc::DataSize PacedSender::QueueSizeData() const {
  return GetStats().queue_size;
}

std::optional<webrtc::Timestamp> PacedSender::FirstSentPacketTime() const {
  return GetStats().first_sent_packet_time;
}

webrtc::TimeDelta PacedSender::OldestPacketWaitTime() const {
  webrtc::Timestamp oldest_packet = GetStats().oldest_packet_enqueue_time;
  if (oldest_packet.IsInfinite()) {
    return webrtc::TimeDelta::Zero();
  }

  // (webrtc:9716): The clock is not always monotonic.
  webrtc::Timestamp current = clock_->CurrentTime();
  if (current < oldest_packet) {
    return webrtc::TimeDelta::Zero();
  }

  return current - oldest_packet;
}

void PacedSender::OnStatsUpdated(const Stats& stats) {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  current_stats_ = stats;
}

void PacedSender::MaybeScheduleProcessPackets() {
  if (!processing_packets_) {
    MaybeProcessPackets(webrtc::Timestamp::MinusInfinity());
  }
}

void PacedSender::MaybeProcessPackets(
    webrtc::Timestamp scheduled_process_time) {
  if (is_shutdown_ || !is_started_ || !transport_ready_) {
    return;
  }

  if (scheduled_process_time.IsFinite()) {
    if (scheduled_process_time != next_process_time_) {
      return;
    }
    next_process_time_ = webrtc::Timestamp::MinusInfinity();
  }

  // Protects against re-entry from transport feedback calling into the task
  // queue pacer.
  processing_packets_ = true;
  auto cleanup = std::unique_ptr<PacedSender,
                                 std::function<void(PacedSender*)>>(
      this, [](PacedSender* sender) { sender->processing_packets_ = false; });

  webrtc::Timestamp next_send_time = pacing_controller_.NextSendTime();
  const webrtc::Timestamp now = clock_->CurrentTime();
  webrtc::TimeDelta early_execute_margin =
      pacing_controller_.IsProbing()
          ? webrtc::PacingController::kMaxEarlyProbeProcessing
          : webrtc::TimeDelta::Zero();

  // Process packets and update stats.
  while (next_send_time <= now + early_execute_margin) {
    pacing_controller_.ProcessPackets();
    next_send_time = pacing_controller_.NextSendTime();

    // Probing state could change. Get margin after process packets.
    early_execute_margin =
        pacing_controller_.IsProbing()
            ? webrtc::PacingController::kMaxEarlyProbeProcessing
            : webrtc::TimeDelta::Zero();
  }

  UpdateStats();

  // Do not hold back in probing.
  webrtc::TimeDelta hold_back_window = webrtc::TimeDelta::Zero();
  if (!pacing_controller_.IsProbing()) {
    hold_back_window = max_hold_back_window_;
    webrtc::DataRate pacing_rate = pacing_controller_.pacing_rate();
    if (max_hold_back_window_in_packets_ != kNoPacketHoldback &&
        !pacing_rate.IsZero() &&
        packet_size_.filtered() != rtc::ExpFilter::kValueUndefined) {
      webrtc::TimeDelta avg_packet_send_time =
          webrtc::DataSize::Bytes(packet_size_.filtered()) / pacing_rate;
      hold_back_window =
          std::min(hold_back_window,
                   avg_packet_send_time * max_hold_back_window_in_packets_);
    }
  }

  // Calculate next process time.
  webrtc::TimeDelta time_to_next_process =
      std::max(hold_back_window, next_send_time - now - early_execute_margin);
  next_send_time = now + time_to_next_process;

  // If no in flight task or in flight task is later than `next_send_time`,
  // schedule a new one. Previous in flight task will be retired.
  if (next_process_time_.IsMinusInfinity() ||
      next_process_time_ > next_send_time) {
    if (task_queue_pacer_->PostDelayedHighPrecisionTask(
            [this, next_send_time]() { MaybeProcessPackets(next_send_time); },
            std::chrono::microseconds(time_to_next_process
                                          .RoundUpTo(
                                              webrtc::TimeDelta::Millis(1))
                                          .us()))) {
      next_process_time_ = next_send_time;
    } else {
      LOG_WARN("Pacer process task rejected: task queue is stopped");
    }
  }
}

void PacedSender::UpdateStats() {
  Stats new_stats;
  new_stats.expected_queue_time = pacing_controller_.ExpectedQueueTime();
  new_stats.first_sent_packet_time = pacing_controller_.FirstSentPacketTime();
  new_stats.oldest_packet_enqueue_time =
      pacing_controller_.OldestPacketEnqueueTime();
  new_stats.queue_size = pacing_controller_.QueueSizeData();
  OnStatsUpdated(new_stats);
}

PacedSender::Stats PacedSender::GetStats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return current_stats_;
}

/*----------------------------------------------------------------------------*/

int PacedSender::EnqueueRtpPackets(
    std::vector<std::unique_ptr<RtpPacket>> &rtp_packets,
    int64_t captured_timestamp_us, const std::string &stream_name) {
  std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> to_send_rtp_packets;
  for (auto &rtp_packet : rtp_packets) {
    std::unique_ptr<webrtc::RtpPacketToSend> rtp_packet_to_send(
        static_cast<webrtc::RtpPacketToSend *>(rtp_packet.release()));
    rtp_packet_to_send->set_capture_time(clock_->CurrentTime());
    rtp_packet_to_send->set_stream_name(stream_name);

    switch (rtp_packet_to_send->PayloadType()) {
      case rtp::PAYLOAD_TYPE::H264:
        rtp_packet_to_send->set_packet_type(webrtc::RtpPacketMediaType::kVideo);
        break;
      case rtp::PAYLOAD_TYPE::AV1:
        rtp_packet_to_send->set_packet_type(webrtc::RtpPacketMediaType::kVideo);
        break;
      case rtp::PAYLOAD_TYPE::H264_FEC_SOURCE:
        rtp_packet_to_send->set_packet_type(
            webrtc::RtpPacketMediaType::kForwardErrorCorrection);
        break;
      case rtp::PAYLOAD_TYPE::H264_FEC_REPAIR:
        rtp_packet_to_send->set_packet_type(
            webrtc::RtpPacketMediaType::kForwardErrorCorrection);
        break;
      case rtp::PAYLOAD_TYPE::OPUS:
        rtp_packet_to_send->set_packet_type(webrtc::RtpPacketMediaType::kAudio);
        break;
      default:
        rtp_packet_to_send->set_packet_type(webrtc::RtpPacketMediaType::kVideo);
        break;
    }
    // webrtc::PacedPacketInfo cluster_info;
    // SendPacket(std::move(rtp_packet_to_send), cluster_info);

    to_send_rtp_packets.push_back(std::move(rtp_packet_to_send));
  }

  EnqueuePackets(std::move(to_send_rtp_packets));
  return 0;
}

int PacedSender::EnqueueRtpPackets(
    std::vector<std::unique_ptr<webrtc::RtpPacketToSend>> &rtp_packets) {
  EnqueuePackets(std::move(rtp_packets));
  return 0;
}

int PacedSender::EnqueueRtpPacket(
    std::unique_ptr<webrtc::RtpPacketToSend> rtp_packet) {
  EnqueuePacket(std::move(rtp_packet));
  return 0;
}
}  // namespace minirtc
