#include "congestion_control.h"

#include <algorithm>
#include <numeric>
#include <vector>

#include "bitrate_limits.h"
#include "log.h"

namespace minirtc {
namespace {
constexpr int64_t kLossUpdateInterval = 1000;

// Pacing-rate relative to our target send rate.
// Multiplicative factor that is applied to the target bitrate to calculate
// the number of bytes that can be transmitted per interval.
// Increasing this factor will result in lower delays in cases of bitrate
// overshoots from the encoder.
constexpr float kDefaultPaceMultiplier = 2.5f;

}  // namespace

BandwidthLimitedCause GetBandwidthLimitedCause(LossBasedState loss_based_state,
                                               bool is_rtt_above_limit,
                                               BandwidthUsage bandwidth_usage) {
  if (bandwidth_usage == BandwidthUsage::kBwOverusing ||
      bandwidth_usage == BandwidthUsage::kBwUnderusing) {
    return BandwidthLimitedCause::kDelayBasedLimitedDelayIncreased;
  } else if (is_rtt_above_limit) {
    return BandwidthLimitedCause::kRttBasedBackOffHighRtt;
  }
  switch (loss_based_state) {
    case LossBasedState::kDecreasing:
      // Probes may not be sent in this state.
      return BandwidthLimitedCause::kLossLimitedBwe;
    case webrtc::LossBasedState::kIncreaseUsingPadding:
      // Probes may not be sent in this state.
      return BandwidthLimitedCause::kLossLimitedBwe;
    case LossBasedState::kIncreasing:
      // Probes may be sent in this state.
      return BandwidthLimitedCause::kLossLimitedBweIncreasing;
    case LossBasedState::kDelayBasedEstimate:
      return BandwidthLimitedCause::kDelayBasedLimited;
    default:
      return BandwidthLimitedCause::kLossLimitedBwe;
  }
}

CongestionControl::CongestionControl(StartingBitrates starting_bitrates,
                                     bool disable_receiver_padding_probing,
                                     RelayQosConfig relay_config)
    : relay_config_(relay_config),
      probe_controller_(new ProbeController(disable_receiver_padding_probing,
                                            relay_config)),
      bandwidth_estimation_(new SendSideBandwidthEstimation()),
      alr_detector_(new AlrDetector()),
      probe_bitrate_estimator_(new ProbeBitrateEstimator()),
      delay_based_bwe_(new DelayBasedBwe()),
      acknowledged_bitrate_estimator_(
          AcknowledgedBitrateEstimatorInterface::Create()),
      last_loss_based_target_rate_(DataRate::Zero()) {
  starting_bitrates_ = starting_bitrates;
  TargetRateConstraints constraints;
  constraints.min_data_rate = DataRate::BitsPerSec(300000);
  constraints.max_data_rate =
      DataRate::BitsPerSec(kDefaultMaxNetworkBitrateBps);
  constraints.starting_rate =
      DataRate::BitsPerSec(starting_bitrates_.direct_bps);
  initial_constraints_ = constraints;
  probe_controller_->EnablePeriodicAlrProbing(true);
}

CongestionControl::~CongestionControl() {}

NetworkControlUpdate CongestionControl::SetRelayPath(bool relay_path,
                                                     Timestamp at_time,
                                                     bool route_changed) {
  NetworkControlUpdate update;
  if (relay_path_ == relay_path && !route_changed) return update;
  relay_path_ = relay_path;
  probe_controller_->SetRelayPath(relay_path);
  const DataRate start =
      DataRate::BitsPerSec(relay_path ? starting_bitrates_.relay_bps
                                      : starting_bitrates_.direct_bps);
  bandwidth_estimation_->OnRouteChange(
      relay_path ? relay_config_.loss_recovery_max_increase : 1.3);
  if (relay_path) {
    LOG_INFO("Relay QoS: probe_duration_ms={} probe_delta_ms={} "
             "loss_recovery_max_increase={} probe_settle_ms={} "
             "further_probe_scale={} startup_retries={} startup_window_ms={} "
             "continuation_observation_ms={} failed_probe_backoff_ms={}",
             relay_config_.probe_duration_ms,
             relay_config_.probe_delta_ms,
             relay_config_.loss_recovery_max_increase,
             relay_config_.probe_settle_ms, relay_config_.further_probe_scale,
             relay_config_.initial_retry_limit,
             relay_config_.initial_retry_window_ms,
             relay_config_.continuation_observation_ms,
             relay_config_.failed_probe_backoff_ms);
  }
  if (initial_constraints_) {
    initial_constraints_->starting_rate = start;
    return update;
  }

  // A new ICE path has its own capacity and timing. Never carry a relay's
  // loss/delay history or probe results onto a direct path (or vice versa).
  acknowledged_bitrate_estimator_ =
      AcknowledgedBitrateEstimatorInterface::Create();
  delay_based_bwe_ = std::make_unique<DelayBasedBwe>();
  probe_bitrate_estimator_ = std::make_unique<ProbeBitrateEstimator>();
  alr_detector_ = std::make_unique<AlrDetector>();
  probe_controller_->Reset(at_time);
  loss_tracker_.Reset(at_time);
  pending_relay_probe_.reset();
  feedback_max_rtts_.clear();
  next_loss_update_ = Timestamp::MinusInfinity();
  lost_packets_since_last_loss_update_ = 0;
  expected_packets_since_last_loss_update_ = 0;
  previously_in_alr_ = false;
  first_feedback_received_ = false;
  last_diagnostic_time_ = Timestamp::MinusInfinity();
  outstanding_data_ = DataSize::Zero();
  current_data_window_.reset();
  encoding_rate_ratio_ = 1.0;
  TargetRateConstraints constraints;
  constraints.at_time = at_time;
  constraints.min_data_rate = min_data_rate_;
  constraints.max_data_rate = max_data_rate_;
  constraints.starting_rate = start;
  update.probe_cluster_configs = ResetConstraints(constraints);
  MaybeTriggerOnNetworkChanged(&update, at_time);
  // Also publish when the new route happens to start at the previous target.
  update.pacer_config = GetPacingRates(at_time);
  LOG_INFO("Congestion-control route reset: path={} starting_bitrate_bps={}",
           relay_path ? "relay" : "direct", start.bps());
  return update;
}

NetworkControlUpdate CongestionControl::OnNetworkAvailability(
    NetworkAvailability msg) {
  NetworkControlUpdate update;
  if (!msg.network_available) pending_relay_probe_.reset();
  update.probe_cluster_configs = probe_controller_->OnNetworkAvailability(msg);
  return update;
}

NetworkControlUpdate CongestionControl::OnProcessInterval(ProcessInterval msg) {
  NetworkControlUpdate update;
  probe_bitrate_estimator_->RemoveExpiredClusters(msg.at_time);
  if (initial_constraints_) {
    // Configuration has no clock until the first process tick. Using its
    // placeholder infinity would bypass startup limits and queue expiry if
    // the network became available before this tick.
    initial_constraints_->at_time = msg.at_time;
    update.probe_cluster_configs = ResetConstraints(*initial_constraints_);
    initial_constraints_.reset();
  }

  bandwidth_estimation_->UpdateEstimate(msg.at_time);
  std::optional<int64_t> start_time_ms =
      alr_detector_->GetApplicationLimitedRegionStartTime();
  probe_controller_->SetAlrStartTime(
      start_time_ms
          ? std::optional<Timestamp>(Timestamp::Millis(*start_time_ms))
          : std::nullopt);

  // Publish the current estimate before evaluating periodic ALR probes.
  MaybeTriggerOnNetworkChanged(&update, msg.at_time);
  auto probes = probe_controller_->Process(msg.at_time);
  update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                      probes.begin(), probes.end());

  if (msg.at_time - last_diagnostic_time_ >= TimeDelta::Seconds(1)) {
    last_diagnostic_time_ = msg.at_time;
    const auto ack = acknowledged_bitrate_estimator_->bitrate();
    const auto loss = loss_tracker_.recent();
    const auto pacing = GetPacingRates(msg.at_time);
    LOG_DEBUG("BWE state: target_bps={} ack_bps={} in_alr={} limited_cause={} "
              "settled_lost={} settled_packets={} pacing_bps={} padding_bps={} "
              "feedback={}",
              last_loss_based_target_rate_.bps(), ack ? ack->bps() : 0,
              start_time_ms.has_value(),
              last_probe_limited_cause_ ? static_cast<int>(*last_probe_limited_cause_) : -1,
              loss.lost, loss.packets, pacing.data_rate().bps(),
              pacing.pad_rate().bps(), first_feedback_received_);
    probe_controller_->LogState(msg.at_time);
  }

  return update;
}

void CongestionControl::ClampConstraints() {
  min_data_rate_ = std::max(min_target_rate_, kCongestionControllerMinBitrate);
  if (max_data_rate_ < min_data_rate_) {
    LOG_WARN("max bitrate smaller than min bitrate");
    max_data_rate_ = min_data_rate_;
  }
  if (starting_rate_ && starting_rate_ < min_data_rate_) {
    LOG_WARN("start bitrate smaller than min bitrate");
    starting_rate_ = min_data_rate_;
  }
}

std::vector<ProbeClusterConfig> CongestionControl::ResetConstraints(
    TargetRateConstraints new_constraints) {
  min_target_rate_ = new_constraints.min_data_rate.value_or(DataRate::Zero());
  max_data_rate_ =
      new_constraints.max_data_rate.value_or(DataRate::PlusInfinity());
  starting_rate_ = new_constraints.starting_rate;
  ClampConstraints();

  bandwidth_estimation_->SetBitrates(starting_rate_, min_data_rate_,
                                     max_data_rate_, new_constraints.at_time);

  if (starting_rate_) delay_based_bwe_->SetStartBitrate(*starting_rate_);
  delay_based_bwe_->SetMinBitrate(min_data_rate_);

  return probe_controller_->SetBitrates(
      min_data_rate_, starting_rate_.value_or(DataRate::Zero()), max_data_rate_,
      new_constraints.at_time);
}

void CongestionControl::OnSentPacket(size_t bytes_sent, Timestamp at_time,
                                     int probe_cluster_id, bool is_video,
                                     DataSize data_in_flight) {
  outstanding_data_ = data_in_flight;
  SentPacket sent_packet;
  sent_packet.send_time = at_time;
  sent_packet.size = DataSize::Bytes(bytes_sent);
  sent_packet.data_in_flight = data_in_flight;
  bandwidth_estimation_->OnSentPacket(sent_packet);
  if (probe_cluster_id != PacedPacketInfo::kNotAProbe &&
      probe_controller_->IsProbeResultValid(probe_cluster_id)) {
    probe_controller_->OnProbeSent(probe_cluster_id, at_time);
    probe_bitrate_estimator_->OnProbePacketSent(
        probe_cluster_id, DataSize::Bytes(bytes_sent), at_time);
  }
  if (is_video) probe_controller_->OnVideoPacketSent();
  alr_detector_->OnBytesSent(bytes_sent, at_time.ms());
  acknowledged_bitrate_estimator_->SetAlr(
      alr_detector_->GetApplicationLimitedRegionStartTime().has_value());
}

void CongestionControl::OnProbeCompleted(int cluster_id, int sent_packets,
                                         Timestamp at_time) {
  if (!probe_controller_->IsProbeResultValid(cluster_id)) return;
  const auto coverage = probe_bitrate_estimator_->OnProbeCompleted(
      cluster_id, sent_packets, at_time);
  if (coverage) probe_controller_->OnProbeResult(cluster_id, *coverage);
}

void CongestionControl::OnProbeAborted(int cluster_id, Timestamp at_time,
                                       const char* reason) {
  probe_controller_->OnProbeAborted(cluster_id);
  probe_bitrate_estimator_->OnProbeAborted(cluster_id);
  if (pending_relay_probe_ && pending_relay_probe_->id == cluster_id)
    pending_relay_probe_.reset();
  LOG_DEBUG("Probe aborted: id={} at_ms={} reason={}", cluster_id, at_time.ms(),
            reason);
}

void CongestionControl::OnVideoSendPaused(bool paused, Timestamp /*at_time*/) {
  probe_controller_->OnVideoSendPaused(paused);
  if (paused) pending_relay_probe_.reset();
}

void CongestionControl::SetMaxTotalAllocatedBitrate(DataRate bitrate) {
  probe_controller_->SetMaxTotalAllocatedBitrate(bitrate);
}

void CongestionControl::UpdatePacketLoss(Timestamp at_time) {
  const auto sample =
      loss_tracker_.Advance(at_time, bandwidth_estimation_->round_trip_time());
  expected_packets_since_last_loss_update_ += sample.packets;
  lost_packets_since_last_loss_update_ += sample.lost;
  const auto recent = loss_tracker_.recent();
  probe_controller_->SetTransportLossSample(recent.packets, recent.lost);
  if (at_time >= next_loss_update_ &&
      expected_packets_since_last_loss_update_ > 0) {
    next_loss_update_ = at_time + TimeDelta::Millis(kLossUpdateInterval);
    if (lost_packets_since_last_loss_update_ > 0) {
      LOG_DEBUG("BWE settled loss: lost={} packets={} loss_grace_ms={}",
                lost_packets_since_last_loss_update_,
                expected_packets_since_last_loss_update_,
                PacketLossTracker::GracePeriod(
                    bandwidth_estimation_->round_trip_time())
                    .ms());
    }
    bandwidth_estimation_->UpdatePacketsLost(
        lost_packets_since_last_loss_update_,
        expected_packets_since_last_loss_update_, at_time);
    expected_packets_since_last_loss_update_ = 0;
    lost_packets_since_last_loss_update_ = 0;
  }
}

NetworkControlUpdate CongestionControl::OnTransportPacketsFeedback(
    TransportPacketsFeedback report) {
  if (report.packet_feedbacks.empty()) {
    // TODO(bugs.webrtc.org/10125): Design a better mechanism to safe-guard
    // against building very large network queues.
    return NetworkControlUpdate();
  }
  first_feedback_received_ = true;
  outstanding_data_ = report.data_in_flight;
  loss_tracker_.OnFeedback(report);

  TimeDelta max_feedback_rtt = TimeDelta::MinusInfinity();
  TimeDelta min_propagation_rtt = TimeDelta::PlusInfinity();
  Timestamp max_recv_time = Timestamp::MinusInfinity();

  std::vector<PacketResult> feedbacks = report.ReceivedWithSendInfo();
  for (const auto& feedback : feedbacks)
    max_recv_time = std::max(max_recv_time, feedback.receive_time);

  for (const auto& feedback : feedbacks) {
    TimeDelta feedback_rtt =
        report.feedback_time - feedback.sent_packet.send_time;
    TimeDelta min_pending_time = max_recv_time - feedback.receive_time;
    TimeDelta propagation_rtt = feedback_rtt - min_pending_time;
    max_feedback_rtt = std::max(max_feedback_rtt, feedback_rtt);
    min_propagation_rtt = std::min(min_propagation_rtt, propagation_rtt);
  }

  if (max_feedback_rtt.IsFinite()) {
    feedback_max_rtts_.push_back(max_feedback_rtt.ms());
    const size_t kMaxFeedbackRttWindow = 32;
    if (feedback_max_rtts_.size() > kMaxFeedbackRttWindow)
      feedback_max_rtts_.pop_front();
    // TODO(srte): Use time since last unacknowledged packet.
    bandwidth_estimation_->UpdatePropagationRtt(report.feedback_time,
                                                min_propagation_rtt);
  }
  if (!feedback_max_rtts_.empty()) {
    const int64_t sum_rtt_ms = std::accumulate(
        feedback_max_rtts_.begin(), feedback_max_rtts_.end(), int64_t{0});
    delay_based_bwe_->OnRttUpdate(
        TimeDelta::Millis(sum_rtt_ms / feedback_max_rtts_.size()));
  }
  if (min_propagation_rtt.IsFinite()) {
    bandwidth_estimation_->UpdateRtt(min_propagation_rtt, report.feedback_time);
  }
  // Process ticks may settle pending losses, but only an actual transport
  // report refreshes the evidence required for relay probe continuation.
  probe_controller_->OnTransportFeedback(
      report.feedback_time, bandwidth_estimation_->round_trip_time());
  std::optional<int64_t> alr_start_time =
      alr_detector_->GetApplicationLimitedRegionStartTime();

  if (previously_in_alr_ && !alr_start_time.has_value()) {
    acknowledged_bitrate_estimator_->SetAlrEndedTime(report.feedback_time);
    probe_controller_->SetAlrEndedTime(report.feedback_time);
  }
  previously_in_alr_ = alr_start_time.has_value();

  const auto sorted_feedback = report.SortedByReceiveTime();
  acknowledged_bitrate_estimator_->IncomingPacketFeedbackVector(
      sorted_feedback);
  auto acknowledged_bitrate = acknowledged_bitrate_estimator_->bitrate();
  bandwidth_estimation_->SetAcknowledgedRate(acknowledged_bitrate,
                                             report.feedback_time);
  for (const auto& feedback : sorted_feedback) {
    const int id = feedback.sent_packet.pacing_info.probe_cluster_id;
    if (id != PacedPacketInfo::kNotAProbe &&
        probe_controller_->IsProbeResultValid(id)) {
      // Feedback may reach the CC queue before the successful-send task.
      // It also proves transmission, using the recorded send timestamp.
      probe_controller_->OnProbeSent(id, feedback.sent_packet.send_time);
      probe_bitrate_estimator_->HandleProbeAndEstimateBitrate(
          feedback, report.feedback_time);
    }
  }

  int probe_id = -1;
  int rejected_probe_id = -1;
  double received_fraction = 0;
  auto probe_bitrate =
      probe_bitrate_estimator_->FetchAndResetLastEstimatedBitrate(
          &probe_id, &received_fraction, &rejected_probe_id);
  if (rejected_probe_id >= 0) {
    probe_controller_->OnProbeResultRejected(rejected_probe_id);
    if (pending_relay_probe_ && pending_relay_probe_->id == rejected_probe_id)
      pending_relay_probe_.reset();
  }
  if (probe_bitrate && !probe_controller_->IsProbeResultValid(probe_id))
    probe_bitrate.reset();
  if (probe_bitrate)
    probe_controller_->OnProbeResult(probe_id, received_fraction);
  if (relay_path_ && relay_config_.probe_settle_ms > 0) {
    // Partial relay feedback is only a candidate. Adopt it after the full
    // send and sufficient actual receipts; another probe additionally waits
    // for the following loss feedback to settle.
    if (probe_bitrate &&
        (!pending_relay_probe_ || probe_id >= pending_relay_probe_->id))
      pending_relay_probe_ = PendingProbe{probe_id, *probe_bitrate};
    probe_bitrate.reset();
  }

  UpdatePacketLoss(report.feedback_time);
  int adopted_relay_probe_id = -1;
  if (pending_relay_probe_ && probe_controller_->IsProbeResultReady(
                                  pending_relay_probe_->id,
                                  report.feedback_time)) {
    probe_bitrate = pending_relay_probe_->bitrate;
    adopted_relay_probe_id = pending_relay_probe_->id;
  }
  // Upstream's lower bound protects against an incomplete low probe without
  // deferring valid estimates or requiring normal media to fill the link.
  if (probe_bitrate && acknowledged_bitrate) {
    probe_bitrate =
        std::max(*probe_bitrate, std::min(delay_based_bwe_->last_estimate(),
                                          *acknowledged_bitrate * 0.85));
  }

  NetworkControlUpdate update;

  DelayBasedBwe::Result result;
  result = delay_based_bwe_->IncomingPacketFeedbackVector(
      report, acknowledged_bitrate, probe_bitrate, alr_start_time.has_value());

  if (result.updated) {
    if (result.probe) {
      if (adopted_relay_probe_id >= 0) {
        probe_controller_->OnProbeResultAdopted(
            adopted_relay_probe_id, report.feedback_time, result.target_bitrate);
        LOG_DEBUG("Relay probe adopted: id={} bitrate_bps={}",
                  adopted_relay_probe_id, result.target_bitrate.bps());
        pending_relay_probe_.reset();
      }
      bandwidth_estimation_->SetSendBitrate(result.target_bitrate,
                                            report.feedback_time);
    }
    // Since SetSendBitrate now resets the delay-based estimate, we have to
    // call UpdateDelayBasedEstimate after SetSendBitrate.
    bandwidth_estimation_->UpdateDelayBasedEstimate(report.feedback_time,
                                                    result.target_bitrate);
  }
  bandwidth_estimation_->UpdateLossBasedEstimator(
      report, result.delay_detector_state, probe_bitrate,
      alr_start_time.has_value());
  Timestamp latest_send_time = Timestamp::MinusInfinity();
  DataSize non_probe_received = DataSize::Zero();
  const auto observation_start = probe_controller_->ProbeObservationStartTime();
  for (const auto& feedback : feedbacks) {
    latest_send_time = std::max(latest_send_time, feedback.sent_packet.send_time);
    if (feedback.sent_packet.send_time > observation_start &&
        feedback.sent_packet.pacing_info.probe_cluster_id ==
            PacedPacketInfo::kNotAProbe)
      non_probe_received += feedback.sent_packet.size;
  }
  probe_controller_->OnProbeHealthFeedback(
      report.feedback_time, latest_send_time, bandwidth_estimation_->target_rate(),
      result.delay_detector_state == BandwidthUsage::kBwOverusing,
      alr_start_time.has_value(), non_probe_received);
  if (!feedback_max_rtts_.empty()) {
    const auto window =
        TimeDelta::Millis(*std::min_element(feedback_max_rtts_.begin(),
                                            feedback_max_rtts_.end())) +
        TimeDelta::Millis(350);
    const DataSize measured = bandwidth_estimation_->target_rate() * window;
    current_data_window_ =
        std::max(DataSize::Bytes(3000),
                 current_data_window_ ? (measured + *current_data_window_) / 2
                                      : measured);
  }
  MaybeTriggerOnNetworkChanged(&update, report.feedback_time);

  if (result.recovered_from_overuse) {
    probe_controller_->SetAlrStartTime(
        alr_start_time
            ? std::optional<Timestamp>(Timestamp::Millis(*alr_start_time))
            : std::nullopt);
    auto probes = probe_controller_->RequestProbe(report.feedback_time);
    update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                        probes.begin(), probes.end());
  }

  return update;
}

void CongestionControl::MaybeTriggerOnNetworkChanged(
    NetworkControlUpdate* update, Timestamp at_time) {
  UpdatePacketLoss(at_time);
  uint8_t fraction_loss = bandwidth_estimation_->fraction_loss();
  TimeDelta round_trip_time = bandwidth_estimation_->round_trip_time();
  DataRate loss_based_target_rate = bandwidth_estimation_->target_rate();
  LossBasedState loss_based_state = bandwidth_estimation_->loss_based_state();
  // Apply WebRTC's congestion-window pushback to the encoder only. Pacing,
  // probing and ALR continue to use the network estimate, so a slow encoder
  // cannot become a permanent capacity ceiling.
  if (current_data_window_ && !current_data_window_->IsZero()) {
    const double fill = outstanding_data_.bytes<double>() /
                        current_data_window_->bytes<double>();
    if (fill > 1.5)
      encoding_rate_ratio_ *= 0.9;
    else if (fill > 1.0)
      encoding_rate_ratio_ *= 0.95;
    else if (fill < 0.1)
      encoding_rate_ratio_ = 1.0;
    else
      encoding_rate_ratio_ = std::min(1.0, encoding_rate_ratio_ * 1.05);
  }
  const DataRate pushback_target_rate = std::min(
      loss_based_target_rate,
      std::max(min_data_rate_, loss_based_target_rate * encoding_rate_ratio_));

  DataRate stable_target_rate =
      bandwidth_estimation_->GetEstimatedLinkCapacity();
  stable_target_rate = std::min(stable_target_rate, pushback_target_rate);

  const bool in_alr =
      alr_detector_->GetApplicationLimitedRegionStartTime().has_value();
  const auto limited_cause = GetBandwidthLimitedCause(
      loss_based_state, bandwidth_estimation_->IsRttAboveLimit(),
      delay_based_bwe_->last_state());
  const bool cause_changed = last_probe_limited_cause_ != limited_cause;

  if ((loss_based_target_rate != last_loss_based_target_rate_) ||
      cause_changed || in_alr != last_reported_in_alr_ ||
      (loss_based_state != last_loss_base_state_) ||
      (fraction_loss != last_estimated_fraction_loss_) ||
      (round_trip_time != last_estimated_round_trip_time_) ||
      (pushback_target_rate != last_pushback_target_rate_) ||
      (stable_target_rate != last_stable_target_rate_)) {
    last_loss_based_target_rate_ = loss_based_target_rate;
    last_pushback_target_rate_ = pushback_target_rate;
    last_estimated_fraction_loss_ = fraction_loss;
    last_estimated_round_trip_time_ = round_trip_time;
    last_stable_target_rate_ = stable_target_rate;
    last_loss_base_state_ = loss_based_state;
    last_reported_in_alr_ = in_alr;

    alr_detector_->SetEstimatedBitrate(loss_based_target_rate.bps());

    TimeDelta bwe_period = delay_based_bwe_->GetExpectedBwePeriod();

    TargetTransferRate target_rate_msg;
    target_rate_msg.at_time = at_time;
    target_rate_msg.target_rate = loss_based_target_rate;
    target_rate_msg.cwnd_reduce_ratio =
        loss_based_target_rate.IsZero()
            ? 0.0
            : 1.0 - pushback_target_rate.bps<double>() /
                        loss_based_target_rate.bps<double>();
    target_rate_msg.stable_target_rate = stable_target_rate;
    target_rate_msg.network_estimate.at_time = at_time;
    target_rate_msg.network_estimate.bandwidth = loss_based_target_rate;
    target_rate_msg.network_estimate.round_trip_time = round_trip_time;
    target_rate_msg.network_estimate.loss_rate_ratio = fraction_loss / 255.0f;
    target_rate_msg.network_estimate.bwe_period = bwe_period;
    target_rate_msg.network_estimate.in_alr = in_alr;

    update->target_rate = target_rate_msg;

    last_probe_limited_cause_ = limited_cause;
    auto probes = probe_controller_->SetEstimatedBitrate(
        loss_based_target_rate, limited_cause, at_time);
    update->probe_cluster_configs.insert(update->probe_cluster_configs.end(),
                                         probes.begin(), probes.end());
    update->pacer_config = GetPacingRates(at_time);
  }
}

PacerConfig CongestionControl::GetPacingRates(Timestamp at_time) const {
  // Screen content arrives as large per-frame bursts: one delta frame can be
  // several times the average frame, and a key frame can exceed a full second
  // of budget. Pacing needs headroom to drain that burst, otherwise a single
  // frame's transmission time approaches the frame interval and the pacer
  // queue holds a standing delay until desktop frame admission (80 ms
  // expected / 100 ms oldest packet) starts dropping frames.
  // Keep the WebRTC default multiplier for the whole session. The delay-based
  // estimator still reacts to the larger bursts, and the admission guard above
  // still bounds the queue when the path is genuinely saturated.
  const DataRate pacing_rate =
      last_loss_based_target_rate_ * kDefaultPaceMultiplier;
  const DataRate padding_rate =
      last_loss_base_state_ == LossBasedState::kIncreaseUsingPadding
          ? last_pushback_target_rate_
          : DataRate::Zero();
  PacerConfig msg;
  msg.at_time = at_time;
  msg.time_window = TimeDelta::Seconds(1);
  msg.data_window = pacing_rate * msg.time_window;
  msg.pad_window = padding_rate * msg.time_window;
  return msg;
}
}  // namespace minirtc
