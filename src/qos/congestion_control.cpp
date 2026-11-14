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

constexpr TimeDelta kProbeEstimateSettleTime = TimeDelta::Millis(100);

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

CongestionControl::CongestionControl(StartingBitrates starting_bitrates)
    : probe_controller_(new ProbeController()),
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
  constraints.max_data_rate = DataRate::BitsPerSec(kDefaultMaxNetworkBitrateBps);
  constraints.starting_rate = DataRate::BitsPerSec(starting_bitrates_.direct_bps);
  initial_constraints_ = constraints;
  probe_controller_->EnablePeriodicAlrProbing(true);
}

CongestionControl::~CongestionControl() {}

void CongestionControl::SetRepeatedInitialProbing(bool enable) {
  probe_controller_->EnableRepeatedInitialProbing(enable);
}

NetworkControlUpdate CongestionControl::SetRelayPath(bool relay_path,
                                                     Timestamp at_time) {
  NetworkControlUpdate update;
  if (relay_path_ == relay_path) {
    return update;
  }

  pending_probe_.reset();
  relay_path_ = relay_path;
  loss_tracker_.Reset(at_time);
  last_packet_feedback_ = Timestamp::MinusInfinity();
  next_loss_update_ = Timestamp::MinusInfinity();
  lost_packets_since_last_loss_update_ = 0;
  expected_packets_since_last_loss_update_ = 0;
  probe_controller_->SetRelayPath(relay_path, at_time);
  bandwidth_ramp_.Reset(last_pushback_target_rate_, relay_path_, at_time,
                         min_data_rate_);
  probe_controller_->SetGrowthLimit(bandwidth_ramp_.probe_limit(at_time),
                                    DataRate::Zero(), Timestamp::MinusInfinity());
  const DataRate desired_starting_rate =
      DataRate::BitsPerSec(relay_path_ ? starting_bitrates_.relay_bps
                                     : starting_bitrates_.direct_bps);

  if (initial_constraints_) {
    initial_constraints_->starting_rate = desired_starting_rate;
    LOG_INFO("Congestion-control startup profile updated: path={} "
             "starting_bitrate_bps={}",
             relay_path_ ? "relay" : "direct",
             desired_starting_rate.bps());
    return update;
  }

  // Apply the configured starting estimate when ICE moves onto a relay.
  // On a new direct path, measure the available headroom before raising media.
  if (!relay_path_ && network_available_) {
    update.probe_cluster_configs =
        probe_controller_->RequestProbeOnPathChange(at_time);
    LOG_INFO("Congestion-control probe profile updated: path=direct "
             "starting_bitrate_unchanged_bps={}",
             bandwidth_estimation_->target_rate().bps());
    return update;
  }

  TargetRateConstraints constraints;
  constraints.at_time = at_time;
  constraints.min_data_rate = min_data_rate_;
  constraints.max_data_rate = max_data_rate_;
  constraints.starting_rate = desired_starting_rate;
  update.probe_cluster_configs = ResetConstraints(constraints);
  MaybeTriggerOnNetworkChanged(&update, at_time);
  LOG_INFO("Congestion-control startup profile applied: path={} "
           "starting_bitrate_bps={}",
           relay_path_ ? "relay" : "direct",
           desired_starting_rate.bps());
  return update;
}

NetworkControlUpdate CongestionControl::OnNetworkAvailability(
    NetworkAvailability msg) {
  network_available_ = msg.network_available;
  if (!network_available_) {
    pending_probe_.reset();
    loss_tracker_.Reset(msg.at_time);
    last_packet_feedback_ = Timestamp::MinusInfinity();
    next_loss_update_ = Timestamp::MinusInfinity();
    lost_packets_since_last_loss_update_ = 0;
    expected_packets_since_last_loss_update_ = 0;
    bandwidth_ramp_.Reset(last_pushback_target_rate_, relay_path_, msg.at_time,
                           min_data_rate_);
  }
  NetworkControlUpdate update;
  update.probe_cluster_configs = probe_controller_->OnNetworkAvailability(msg);
  return update;
}

void CongestionControl::PauseProbingUntil(Timestamp until, const char* reason,
                                         Timestamp at_time) {
  pending_probe_.reset();
  if (!at_time.IsFinite()) at_time = last_update_time_;
  if (at_time.IsFinite()) bandwidth_ramp_.Pause(at_time, until);
  probe_controller_->PauseProbingUntil(until, reason);
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
  probe_controller_->SetAlrStartTimeMs(start_time_ms);

  // Refresh health and delay state before evaluating a recovery timer.
  MaybeTriggerOnNetworkChanged(&update, msg.at_time);
  auto probes = probe_controller_->Process(msg.at_time);
  update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                      probes.begin(), probes.end());

  MaybeApplyPendingProbe(&update, msg.at_time);
  return update;
}

bool CongestionControl::ProbeNetworkHealthy() const {
  const TimeDelta rtt = bandwidth_estimation_->round_trip_time();
  return network_available_ &&
         delay_based_bwe_->last_state() != BandwidthUsage::kBwOverusing &&
         bandwidth_estimation_->fraction_loss() / 255.0 < 0.05 &&
         rtt.IsFinite() && rtt < TimeDelta::Millis(300);
}

std::optional<DataRate> CongestionControl::FilterProbeResult(
    DataRate bitrate, int probe_id, Timestamp at_time) {
  if (!probe_controller_->IsProbeResultValid(probe_id)) return std::nullopt;
  if (pending_probe_ && pending_probe_->id != probe_id) {
    pending_probe_.reset();
  }
  if (!ProbeNetworkHealthy()) {
    pending_probe_.reset();
    return bitrate;
  }
  const DataRate reference =
      pending_probe_ ? pending_probe_->reference : last_pushback_target_rate_;
  // Compare against the capped media target, not a raw delay estimate that
  // may exceed the configured maximum. Small revisions remain immediate.
  if (probe_id <= last_settled_probe_id_ || reference <= DataRate::Zero() ||
      bitrate >= reference * 0.90) {
    pending_probe_.reset();
    return bitrate;
  }
  if (pending_probe_) {
    // Revisions update the candidate, never the original deadline.
    pending_probe_->bitrate = bitrate;
  } else {
    pending_probe_ = PendingProbe{probe_id, bitrate, reference,
                                  at_time + kProbeEstimateSettleTime};
  }
  return std::nullopt;
}

void CongestionControl::MaybeApplyPendingProbe(NetworkControlUpdate* update,
                                               Timestamp at_time) {
  if (!pending_probe_) return;
  if (!ProbeNetworkHealthy()) {
    pending_probe_.reset();
    return;
  }
  if (at_time < pending_probe_->deadline) return;
  if (at_time - pending_probe_->deadline > TimeDelta::Seconds(1)) {
    pending_probe_.reset();
    return;
  }
  const PendingProbe probe = *pending_probe_;
  pending_probe_.reset();
  last_settled_probe_id_ = probe.id;
  // Ordinary delay/loss feedback continues throughout the settling window.
  // Do not use an old candidate to undo a newer congestion-driven reduction.
  if (bandwidth_estimation_->target_rate() < probe.bitrate) {
    return;
  }
  const auto result = delay_based_bwe_->ApplyProbeResult(
      probe.bitrate, at_time, acknowledged_bitrate_estimator_->bitrate(),
      alr_detector_->GetApplicationLimitedRegionStartTime().has_value());
  if (!result.updated || !result.probe) return;
  bandwidth_estimation_->SetSendBitrate(result.target_bitrate, at_time);
  bandwidth_estimation_->UpdateDelayBasedEstimate(at_time,
                                                  result.target_bitrate);
  MaybeTriggerOnNetworkChanged(update, at_time);
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
  pending_probe_.reset();
  min_target_rate_ = new_constraints.min_data_rate.value_or(DataRate::Zero());
  max_data_rate_ =
      new_constraints.max_data_rate.value_or(DataRate::PlusInfinity());
  starting_rate_ = new_constraints.starting_rate;
  ClampConstraints();

  bandwidth_estimation_->SetBitrates(starting_rate_, min_data_rate_,
                                     max_data_rate_, new_constraints.at_time);

  if (starting_rate_) delay_based_bwe_->SetStartBitrate(*starting_rate_);
  delay_based_bwe_->SetMinBitrate(min_data_rate_);

  bandwidth_ramp_.Reset(starting_rate_.value_or(min_data_rate_), relay_path_,
                         new_constraints.at_time, min_data_rate_);
  probe_controller_->SetGrowthLimit(
      bandwidth_ramp_.probe_limit(new_constraints.at_time), DataRate::Zero(),
      Timestamp::MinusInfinity());

  return probe_controller_->SetBitrates(
      min_data_rate_, starting_rate_.value_or(DataRate::Zero()), max_data_rate_,
      new_constraints.at_time);
}

void CongestionControl::OnSentPacket(size_t bytes_sent, Timestamp at_time,
                                    int probe_cluster_id, bool is_video) {
  if (probe_cluster_id != PacedPacketInfo::kNotAProbe) {
    if (probe_controller_->IsProbeResultValid(probe_cluster_id)) {
      bandwidth_ramp_.OnProbeSent(probe_cluster_id, at_time);
      probe_controller_->OnProbeSent(probe_cluster_id, at_time);
    }
  } else {
    delay_based_bwe_->OnNormalPacketSent(DataSize::Bytes(bytes_sent), at_time);
  }
  if (is_video) probe_controller_->OnVideoPacketSent(at_time);
  alr_detector_->OnBytesSent(bytes_sent, at_time.ms());
  std::optional<int64_t> alr_start_time =
      alr_detector_->GetApplicationLimitedRegionStartTime();
  acknowledged_bitrate_estimator_->SetAlr(alr_start_time.has_value());
}

void CongestionControl::OnProbeAborted(int cluster_id, Timestamp at_time,
                                     const char* reason) {
  probe_controller_->OnProbeAborted(cluster_id, at_time, reason);
  bandwidth_ramp_.InvalidateProbe(cluster_id);
  if (pending_probe_ && pending_probe_->id == cluster_id)
    pending_probe_.reset();
}

void CongestionControl::OnVideoSendPaused(bool paused, Timestamp at_time) {
  probe_controller_->OnVideoSendPaused(paused, at_time);
  bandwidth_ramp_.OnVideoSendPaused(paused, at_time);
  if (paused) {
    pending_probe_.reset();
  }
}

void CongestionControl::UpdatePacketLoss(Timestamp at_time) {
  const auto sample =
      loss_tracker_.Advance(at_time, bandwidth_estimation_->round_trip_time());
  expected_packets_since_last_loss_update_ += sample.packets;
  lost_packets_since_last_loss_update_ += sample.lost;
  if (at_time >= next_loss_update_ &&
      expected_packets_since_last_loss_update_ > 0) {
    next_loss_update_ = at_time + TimeDelta::Millis(kLossUpdateInterval);
    if (lost_packets_since_last_loss_update_ > 0) {
      LOG_DEBUG("BWE settled loss: lost={} packets={} loss_grace_ms={}",
                lost_packets_since_last_loss_update_,
                expected_packets_since_last_loss_update_,
                PacketLossTracker::GracePeriod(
                    bandwidth_estimation_->round_trip_time()).ms());
    }
    // Loss backoff must act on the budget actually sent, not an optimistic
    // probe candidate that may be several times higher than that budget.
    if (expected_packets_since_last_loss_update_ >= 20 &&
        lost_packets_since_last_loss_update_ >
            expected_packets_since_last_loss_update_ * 0.10 &&
        last_pushback_target_rate_ > DataRate::Zero() &&
        bandwidth_estimation_->target_rate() > last_pushback_target_rate_) {
      bandwidth_estimation_->SetSendBitrate(last_pushback_target_rate_,
                                            at_time);
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
  last_packet_feedback_ = report.feedback_time;
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
  std::optional<int64_t> alr_start_time =
      alr_detector_->GetApplicationLimitedRegionStartTime();

  if (previously_in_alr_ && !alr_start_time.has_value()) {
    int64_t now_ms = report.feedback_time.ms();
    acknowledged_bitrate_estimator_->SetAlrEndedTime(report.feedback_time);
    probe_controller_->SetAlrEndedTimeMs(now_ms);
  }
  previously_in_alr_ = alr_start_time.has_value();

  const auto sorted_feedback = report.SortedByReceiveTime();
  auto normal_feedback = sorted_feedback;
  normal_feedback.erase(
      std::remove_if(normal_feedback.begin(), normal_feedback.end(),
                     [](const PacketResult& packet) {
                       return packet.sent_packet.pacing_info.probe_cluster_id !=
                              PacedPacketInfo::kNotAProbe;
                     }),
      normal_feedback.end());
  acknowledged_bitrate_estimator_->IncomingPacketFeedbackVector(
      normal_feedback);
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
      bandwidth_ramp_.OnProbeSent(id, feedback.sent_packet.send_time);
      auto probe_result =
          probe_bitrate_estimator_->HandleProbeAndEstimateBitrate(feedback);
      if (probe_result) {
        probe_controller_->OnProbeResult(id, *probe_result, report.feedback_time);
        bandwidth_ramp_.OnProbeResult(id, *probe_result, report.feedback_time);
      } else if (const char* reason =
                     probe_bitrate_estimator_->LastRejectionReason(id)) {
        probe_controller_->OnProbeFeedbackRejected(id, reason,
                                                   report.feedback_time);
        bandwidth_ramp_.InvalidateProbe(id);
        if (pending_probe_ && pending_probe_->id == id) pending_probe_.reset();
      }
    }
  }

  bandwidth_ramp_.OnFeedback(report);

  int probe_id = -1;
  std::optional<DataRate> probe_bitrate =
      probe_bitrate_estimator_->FetchAndResetLastEstimatedBitrate(&probe_id);
  if (probe_bitrate) {
    probe_bitrate =
        FilterProbeResult(*probe_bitrate, probe_id, report.feedback_time);
  }

  NetworkControlUpdate update;

  DelayBasedBwe::Result result;
  UpdatePacketLoss(report.feedback_time);
  delay_based_bwe_->SetTransportState(
      last_pushback_target_rate_, bandwidth_estimation_->round_trip_time(),
      bandwidth_estimation_->fraction_loss() / 255.0);
  result = delay_based_bwe_->IncomingPacketFeedbackVector(
      report, acknowledged_bitrate, probe_bitrate, alr_start_time.has_value());

  if (result.updated) {
    if (result.probe) {
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
  if (!ProbeNetworkHealthy()) pending_probe_.reset();
  if (result.updated) {
    // Update the estimate in the ProbeController, in case we want to probe.
    MaybeTriggerOnNetworkChanged(&update, report.feedback_time);
  }

  MaybeApplyPendingProbe(&update, report.feedback_time);

  if (result.recovered_from_overuse) {
    probe_controller_->SetAlrStartTimeMs(alr_start_time);
    auto probes = probe_controller_->RequestProbe(report.feedback_time);
    update.probe_cluster_configs.insert(update.probe_cluster_configs.end(),
                                        probes.begin(), probes.end());
  }

  return update;
}

void CongestionControl::MaybeTriggerOnNetworkChanged(
    NetworkControlUpdate* update, Timestamp at_time) {
  last_update_time_ = at_time;
  UpdatePacketLoss(at_time);
  uint8_t fraction_loss = bandwidth_estimation_->fraction_loss();
  TimeDelta round_trip_time = bandwidth_estimation_->round_trip_time();
  DataRate loss_based_target_rate = bandwidth_estimation_->target_rate();
  LossBasedState loss_based_state = bandwidth_estimation_->loss_based_state();
  // Probe estimates remain candidates inside BWE. Publish a separately
  // confirmed budget to the encoder, pacer, ALR detector and probe limiter.
  const bool fresh_feedback =
      last_packet_feedback_.IsFinite() &&
      at_time - last_packet_feedback_ <= TimeDelta::Seconds(1);
  const bool high_rtt =
      round_trip_time.IsFinite() && round_trip_time >= TimeDelta::Millis(300);
  const bool delay_overuse =
      delay_based_bwe_->last_state() == BandwidthUsage::kBwOverusing;
  // The ramp can pause growth on fresh delay/loss evidence, but BWE alone
  // owns decreases. Its settled loss window also expires independently.
  const bool ramp_healthy = !fresh_feedback || (!high_rtt && !delay_overuse);
  DataRate pushback_target_rate =
      network_available_
          ? bandwidth_ramp_.Update(loss_based_target_rate, ramp_healthy,
                                   round_trip_time, at_time)
          : std::min(loss_based_target_rate, last_pushback_target_rate_);
  pushback_target_rate = std::clamp(pushback_target_rate, min_data_rate_,
                                   max_data_rate_);
  if (at_time < bandwidth_ramp_.hold_until()) {
    pending_probe_.reset();
    probe_controller_->PauseProbingUntil(bandwidth_ramp_.hold_until(),
                                         "bandwidth_ramp");
    update->probe_cluster_configs.clear();
  }
  probe_controller_->SetGrowthLimit(bandwidth_ramp_.probe_limit(at_time),
                                    bandwidth_ramp_.confirmed_rate(),
                                    bandwidth_ramp_.confirmed_at());

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
      cause_changed ||
      in_alr != last_reported_in_alr_ ||
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

    alr_detector_->SetEstimatedBitrate(pushback_target_rate.bps());

    TimeDelta bwe_period = delay_based_bwe_->GetExpectedBwePeriod();

    TargetTransferRate target_rate_msg;
    target_rate_msg.at_time = at_time;
    target_rate_msg.target_rate = pushback_target_rate;
    target_rate_msg.stable_target_rate = stable_target_rate;
    target_rate_msg.network_estimate.at_time = at_time;
    target_rate_msg.network_estimate.round_trip_time = round_trip_time;
    target_rate_msg.network_estimate.loss_rate_ratio = fraction_loss / 255.0f;
    target_rate_msg.network_estimate.bwe_period = bwe_period;
    target_rate_msg.network_estimate.in_alr = in_alr;

    update->target_rate = target_rate_msg;

    const bool probe_healthy =
        fraction_loss / 255.0 < 0.05 && round_trip_time.IsFinite() &&
        round_trip_time < TimeDelta::Millis(300);
    if (cause_changed || probe_health_blocked_ != !probe_healthy ||
        (!probe_healthy &&
         at_time - last_probe_health_log_ >= TimeDelta::Seconds(2))) {
      LOG_DEBUG("Probe BWE health: loss_rtt_healthy={} loss={} rtt_ms={} "
                "limited_cause={} estimate_bps={} in_alr={}",
                probe_healthy, fraction_loss / 255.0,
                round_trip_time.IsFinite() ? round_trip_time.ms() : -1,
                static_cast<int>(limited_cause), loss_based_target_rate.bps(),
                in_alr);
      last_probe_health_log_ = at_time;
    }
    probe_health_blocked_ = !probe_healthy;
    last_probe_limited_cause_ = limited_cause;
    auto probes = probe_controller_->SetEstimatedBitrate(
        loss_based_target_rate,
        limited_cause, at_time, probe_healthy);
    update->probe_cluster_configs.insert(update->probe_cluster_configs.end(),
                                         probes.begin(), probes.end());
    update->pacer_config = GetPacingRates(at_time);

  }
}

PacerConfig CongestionControl::GetPacingRates(Timestamp at_time) const {
  // Use the published transport budget, including its recovery ceiling.
  // Raw burst estimates must not independently accelerate ordinary packets.
  const DataRate pacing_rate = last_pushback_target_rate_ * kDefaultPaceMultiplier;
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
