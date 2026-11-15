/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "probe_controller.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "bitrate_limits.h"
#include "log.h"
#include "packet_loss_tracker.h"

namespace minirtc {
namespace webrtc {
namespace {
constexpr TimeDelta kMaxWaitingTimeForProbingResult = TimeDelta::Seconds(1);
constexpr DataRate kDefaultMaxProbingBitrate =
    DataRate::BitsPerSec(kDefaultMaxNetworkBitrateBps);
constexpr double kBitrateDropThreshold = 0.66;
constexpr TimeDelta kBitrateDropTimeout = TimeDelta::Seconds(5);
constexpr double kProbeFractionAfterDrop = 0.85;
constexpr TimeDelta kAlrEndedTimeout = TimeDelta::Seconds(3);
constexpr TimeDelta kMinTimeBetweenAlrProbes = TimeDelta::Seconds(5);
constexpr double kProbeUncertainty = 0.05;
}  // namespace

ProbeController::ProbeController(bool require_video, RelayQosConfig relay_config)
    : network_available_(false),
      enable_periodic_alr_probing_(false),
      require_video_(require_video),
      relay_config_(relay_config) {
  Reset(Timestamp::Zero());
}
ProbeController::~ProbeController() = default;

std::vector<ProbeClusterConfig> ProbeController::SetBitrates(
    DataRate min_bitrate, DataRate start_bitrate, DataRate max_bitrate,
    Timestamp at_time) {
  if (start_bitrate > DataRate::Zero()) {
    start_bitrate_ = start_bitrate;
    estimated_bitrate_ = start_bitrate;
  } else if (start_bitrate_.IsZero()) {
    start_bitrate_ = min_bitrate;
  }

  // The reason we use the variable `old_max_bitrate_pbs` is because we
  // need to set `max_bitrate_` before we call InitiateProbing.
  DataRate old_max_bitrate = max_bitrate_;
  max_bitrate_ =
      max_bitrate.IsFinite() ? max_bitrate : kDefaultMaxProbingBitrate;

  switch (state_) {
    case State::kInit:
      if (network_available_) return InitiateExponentialProbing(at_time);
      break;

    case State::kWaitingForProbingResult:
      break;

    case State::kProbingComplete:
      // If the new max bitrate is higher than both the old max bitrate and the
      // estimate then initiate probing.
      if (!estimated_bitrate_.IsZero() && old_max_bitrate < max_bitrate_ &&
          estimated_bitrate_ < max_bitrate_) {
        return InitiateProbing(at_time, std::vector<DataRate>{max_bitrate_},
                               false);
      }
      break;
  }
  return std::vector<ProbeClusterConfig>();
}

std::vector<ProbeClusterConfig> ProbeController::OnNetworkAvailability(
    NetworkAvailability msg) {
  network_available_ = msg.network_available;
  if (!network_available_) {
    initial_retry_deadline_ = Timestamp::MinusInfinity();
    min_valid_probe_id_ = next_probe_cluster_id_;
    last_probe_result_id_ = -1;
    last_probe_received_fraction_ = 0;
  }

  if (!network_available_ && state_ == State::kWaitingForProbingResult) {
    state_ = State::kProbingComplete;
    min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
  }

  if (network_available_ && state_ == State::kInit && !start_bitrate_.IsZero())
    return InitiateExponentialProbing(msg.at_time);
  return std::vector<ProbeClusterConfig>();
}

void ProbeController::UpdateState(State new_state) {
  switch (new_state) {
    case State::kInit:
      state_ = State::kInit;
      break;
    case State::kWaitingForProbingResult:
      state_ = State::kWaitingForProbingResult;
      break;
    case State::kProbingComplete:
      state_ = State::kProbingComplete;
      min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
      break;
  }
}

std::vector<ProbeClusterConfig> ProbeController::InitiateExponentialProbing(
    Timestamp at_time) {
  assert(network_available_);
  assert(state_ == State::kInit);
  assert(start_bitrate_ > DataRate::Zero());

  // When probing at 1.8 Mbps ( 6x 300), this represents a threshold of
  // 1.2 Mbps to continue probing.
  std::vector<DataRate> probes = {config_.first_exponential_probe_scale *
                                  start_bitrate_};
  if (config_.second_exponential_probe_scale &&
      config_.second_exponential_probe_scale > 0) {
    probes.push_back(config_.second_exponential_probe_scale * start_bitrate_);
  }
  auto result = InitiateProbing(at_time, probes, true);
  if (!result.empty() && relay_path_ && relay_config_.initial_retry_limit > 0) {
    initial_retry_deadline_ =
        at_time + TimeDelta::Millis(relay_config_.initial_retry_window_ms);
    initial_probe_rate_ = result.back().target_data_rate;
    initial_retry_count_ = 0;
  }
  return result;
}

std::vector<ProbeClusterConfig> ProbeController::SetEstimatedBitrate(
    DataRate bitrate, BandwidthLimitedCause bandwidth_limited_cause,
    Timestamp at_time) {
  bandwidth_limited_cause_ = bandwidth_limited_cause;
  if (bitrate < kBitrateDropThreshold * estimated_bitrate_) {
    last_drop_in_alr_ =
        alr_start_time_.has_value() ||
        (alr_end_time_ && at_time - *alr_end_time_ < kAlrEndedTimeout);
    time_of_last_large_drop_ = at_time;
    bitrate_before_last_large_drop_ = estimated_bitrate_;
  }
  estimated_bitrate_ = bitrate;

  return ContinueProbing(at_time);
}

bool ProbeController::HasSignificantSettledLoss(Timestamp at_time) const {
  return relay_path_ && relay_config_.probe_settle_ms > 0 &&
         at_time - last_feedback_time_ <= TimeDelta::Seconds(1) &&
         settled_packets_ >= 20 && settled_lost_ * 10 >= settled_packets_;
}

TimeDelta ProbeController::ProbeResultWaitTime() const {
  if (!relay_path_ || relay_config_.probe_settle_ms <= 0)
    return kMaxWaitingTimeForProbingResult;
  // Keep the native one-second deadline unless the bounded RTT/grace window
  // itself needs longer. Allow one feedback interval after settling, never an
  // unbounded continuation from an old probe result.
  const auto settle = std::max(TimeDelta::Millis(relay_config_.probe_settle_ms),
                              feedback_rtt_ +
                                  PacketLossTracker::GracePeriod(feedback_rtt_));
  return std::max(kMaxWaitingTimeForProbingResult,
                  settle + TimeDelta::Millis(100));
}

const char* ProbeController::ProbeResultBlockReason(
    Timestamp at_time, bool require_settled) const {
  if (at_time - time_last_probing_initiated_ > ProbeResultWaitTime())
    return "result_expired";
  if (last_probe_result_id_ != next_probe_cluster_id_ - 1)
    return "waiting_valid_probe";
  if (relay_path_ && relay_config_.probe_settle_ms > 0) {
    if (bandwidth_limited_cause_ ==
        BandwidthLimitedCause::kRttBasedBackOffHighRtt)
      return "bandwidth_limited";
    // The upstream 80% threshold may publish an early useful estimate. It
    // must not immediately authorize another relay rate increase while the
    // tail of this cluster and the associated loss feedback are outstanding.
    if (last_probe_received_fraction_ < 0.95) return "incomplete_feedback";
    const auto grace = PacketLossTracker::GracePeriod(feedback_rtt_);
    const auto settle = std::max(TimeDelta::Millis(relay_config_.probe_settle_ms),
                                feedback_rtt_ + grace);
    if (require_settled && at_time < time_last_probing_initiated_ + settle)
      return "settling";
    // A report from the beginning of the observation window does not prove
    // that the path remained healthy through its end.
    const auto observation = require_settled ? settle : TimeDelta::Zero();
    if (last_feedback_time_ < time_last_probing_initiated_ + observation ||
        at_time - last_feedback_time_ > TimeDelta::Seconds(1))
      return "fresh_feedback_needed";
    if (HasSignificantSettledLoss(at_time)) return "settled_loss";
  }
  return nullptr;
}

bool ProbeController::IsProbeResultReady(int id, Timestamp at_time) const {
  return network_available_ && !video_paused_ &&
         (!require_video_ || video_started_) && IsProbeResultValid(id) &&
         id == last_probe_result_id_ &&
         ProbeResultBlockReason(at_time, false) == nullptr;
}

const char* ProbeController::ContinuationBlockReason(Timestamp at_time) const {
  if (const char* reason = ProbeResultBlockReason(at_time, true)) return reason;
  // Adopting a completed measurement lets the current delay/loss estimators
  // handle recovery. Sending more probe traffic still obeys their backoff.
  if (relay_path_ &&
      (bandwidth_limited_cause_ == BandwidthLimitedCause::kLossLimitedBwe ||
       bandwidth_limited_cause_ ==
           BandwidthLimitedCause::kDelayBasedLimitedDelayIncreased))
    return "bandwidth_limited";
  if (estimated_bitrate_ <= min_bitrate_to_probe_further_)
    return "below_further_threshold";
  return nullptr;
}

std::vector<ProbeClusterConfig> ProbeController::ContinueProbing(
    Timestamp at_time) {
  if (state_ != State::kWaitingForProbingResult ||
      ContinuationBlockReason(at_time))
    return {};
  // Re-evaluate using the current estimate after settling, never the peak
  // reported by an earlier partial cluster. This is a bounded relay policy,
  // not a substitute for WebRTC's optional network-state capacity estimator.
  return InitiateProbing(
      at_time, {config_.further_exponential_probe_scale * estimated_bitrate_},
      true);
}

void ProbeController::EnablePeriodicAlrProbing(bool enable) {
  enable_periodic_alr_probing_ = enable;
}

void ProbeController::SetAlrStartTime(std::optional<Timestamp> alr_start_time) {
  alr_start_time_ = alr_start_time;
}
void ProbeController::SetAlrEndedTime(Timestamp alr_end_time) {
  alr_end_time_ = alr_end_time;
}

std::vector<ProbeClusterConfig> ProbeController::RequestProbe(
    Timestamp at_time) {
  // Called once we have returned to normal state after a large drop in
  // estimated bandwidth. The current response is to initiate a single probe
  // session (if not already probing) at the previous bitrate.
  //
  // If the probe session fails, the assumption is that this drop was a
  // real one from a competing flow or a network change.
  if (bandwidth_limited_cause_ != BandwidthLimitedCause::kDelayBasedLimited &&
      bandwidth_limited_cause_ !=
          BandwidthLimitedCause::kLossLimitedBweIncreasing)
    return {};
  bool in_alr = alr_start_time_.has_value();
  bool alr_ended_recently =
      (alr_end_time_.has_value() &&
       at_time - alr_end_time_.value() < kAlrEndedTimeout);
  // Preserve the ALR context of the drop while waiting for the cooldown.
  // Sending at the reduced target can end ALR before that cooldown expires.
  if (in_alr || alr_ended_recently || last_drop_in_alr_) {
    if (state_ == State::kProbingComplete) {
      DataRate suggested_probe =
          kProbeFractionAfterDrop * bitrate_before_last_large_drop_;
      DataRate min_expected_probe_result =
          (1 - kProbeUncertainty) * suggested_probe;
      TimeDelta time_since_drop = at_time - time_of_last_large_drop_;
      TimeDelta time_since_probe = at_time - last_bwe_drop_probing_time_;
      if (min_expected_probe_result > estimated_bitrate_ &&
          time_since_drop < kBitrateDropTimeout &&
          time_since_probe > kMinTimeBetweenAlrProbes &&
          time_of_last_large_drop_ > last_bwe_drop_probing_time_) {
        LOG_DEBUG("Detected big bandwidth drop, start probing.");
        // Track how often we probe in response to bandwidth drop in ALR.
        // A relay recovery probe may undershoot the current path capacity.
        // Let a successful result continue through the same bounded, settled
        // policy instead of making each transient drop a lower ceiling.
        auto probes = InitiateProbing(
            at_time, std::vector<DataRate>{suggested_probe},
            relay_path_ && relay_config_.probe_settle_ms > 0);
        if (!probes.empty()) last_bwe_drop_probing_time_ = at_time;
        return probes;
      }
    }
  }
  return std::vector<ProbeClusterConfig>();
}

void ProbeController::Reset(Timestamp at_time) {
  min_valid_probe_id_ = next_probe_cluster_id_;
  current_probe_start_id_ = next_probe_cluster_id_;
  last_probe_result_id_ = -1;
  last_probe_received_fraction_ = 0;
  initial_retry_deadline_ = Timestamp::MinusInfinity();
  initial_probe_rate_ = DataRate::Zero();
  initial_retry_count_ = 0;
  last_feedback_time_ = Timestamp::MinusInfinity();
  feedback_rtt_ = TimeDelta::Millis(100);
  settled_packets_ = settled_lost_ = 0;
  video_started_ = false;
  alr_start_time_.reset();
  bandwidth_limited_cause_ = BandwidthLimitedCause::kDelayBasedLimited;
  state_ = State::kInit;
  min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
  time_last_probing_initiated_ = Timestamp::Zero();
  estimated_bitrate_ = DataRate::Zero();
  start_bitrate_ = DataRate::Zero();
  max_bitrate_ = kDefaultMaxProbingBitrate;
  Timestamp now = at_time;
  last_bwe_drop_probing_time_ = Timestamp::MinusInfinity();
  alr_end_time_.reset();
  time_of_last_large_drop_ = now;
  bitrate_before_last_large_drop_ = DataRate::Zero();
  last_drop_in_alr_ = false;
}

bool ProbeController::TimeForAlrProbe(Timestamp at_time) const {
  if (enable_periodic_alr_probing_ && alr_start_time_) {
    Timestamp next_probe_time =
        std::max(*alr_start_time_, time_last_probing_initiated_) +
        config_.alr_probing_interval;
    return at_time >= next_probe_time;
  }
  return false;
}

std::vector<ProbeClusterConfig> ProbeController::Process(Timestamp at_time) {
  if (!network_available_ || (require_video_ && !video_started_) ||
      video_paused_)
    return {};
  if (state_ == State::kInit && !start_bitrate_.IsZero())
    return InitiateExponentialProbing(at_time);
  auto continuation = ContinueProbing(at_time);
  if (!continuation.empty()) return continuation;
  if (at_time - time_last_probing_initiated_ > ProbeResultWaitTime()) {
    if (state_ == State::kWaitingForProbingResult) {
      LOG_DEBUG("kWaitingForProbingResult: timeout");
      UpdateState(State::kProbingComplete);
    }
  }
  if (estimated_bitrate_.IsZero() || state_ != State::kProbingComplete) {
    return {};
  }
  if (at_time < initial_retry_deadline_ &&
      initial_retry_count_ < relay_config_.initial_retry_limit &&
      at_time - time_last_probing_initiated_ >=
          ProbeResultWaitTime() &&
      at_time - last_feedback_time_ <= TimeDelta::Seconds(1)) {
    // Unlike unrestricted repeated initial probing, retry only while no
    // startup cluster has yielded a valid result, at no higher than the first
    // attempted rate. ALR is not required for this finite recovery window.
    auto retry = InitiateProbing(
        at_time,
        {std::min(initial_probe_rate_,
                  estimated_bitrate_ * config_.first_exponential_probe_scale)},
        true);
    if (!retry.empty()) {
      ++initial_retry_count_;
      LOG_DEBUG("Retry invalid startup probe: attempt={} target_bitrate_bps={}",
                initial_retry_count_, retry.back().target_data_rate.bps());
      return retry;
    }
  }
  if (TimeForAlrProbe(at_time)) {
    return InitiateProbing(
        at_time,
        std::vector<DataRate>{estimated_bitrate_ * config_.alr_probe_scale},
        true);
  }
  // A normal-state transition can arrive just before the cooldown expires.
  // Keep that one recovery opportunity within GCC's original five-second
  // drop window; a successfully scheduled request consumes it even if it fails.
  return RequestProbe(at_time);
}

ProbeClusterConfig ProbeController::CreateProbeClusterConfig(Timestamp at_time,
                                                             DataRate bitrate) {
  ProbeClusterConfig config;
  config.at_time = at_time;
  config.target_data_rate = bitrate;
  config.target_duration = config_.min_probe_duration;
  config.min_probe_delta = config_.min_probe_delta;
  config.target_probe_count = config_.min_probe_packets_sent;
  config.id = next_probe_cluster_id_;
  next_probe_cluster_id_++;
  LOG_DEBUG("Probe cluster created: id={} target_bitrate_bps={} duration_ms={}",
            config.id, bitrate.bps(), config.target_duration.ms());
  return config;
}

std::vector<ProbeClusterConfig> ProbeController::InitiateProbing(
    Timestamp now, const std::vector<DataRate>& bitrates_to_probe,
    bool probe_further) {
  if (!network_available_ || (require_video_ && !video_started_) ||
      video_paused_)
    return {};
  if (HasSignificantSettledLoss(now)) return {};
  DataRate max_probe_bitrate = max_bitrate_;
  switch (bandwidth_limited_cause_) {
    case BandwidthLimitedCause::kRttBasedBackOffHighRtt:
    case BandwidthLimitedCause::kDelayBasedLimitedDelayIncreased:
    case BandwidthLimitedCause::kLossLimitedBwe:
      return {};
    case BandwidthLimitedCause::kLossLimitedBweIncreasing:
      max_probe_bitrate =
          std::min(max_probe_bitrate,
                   estimated_bitrate_ * config_.loss_limited_probe_scale);
      break;
    case BandwidthLimitedCause::kDelayBasedLimited:
      break;
    default:
      break;
  }

  current_probe_start_id_ = next_probe_cluster_id_;
  last_probe_received_fraction_ = 0;
  std::vector<ProbeClusterConfig> pending_probes;
  for (DataRate bitrate : bitrates_to_probe) {
    assert(!bitrate.IsZero());
    if (bitrate >= max_probe_bitrate) {
      bitrate = max_probe_bitrate;
      probe_further = false;
    }
    pending_probes.push_back(CreateProbeClusterConfig(now, bitrate));
  }
  time_last_probing_initiated_ = now;
  if (probe_further) {
    UpdateState(State::kWaitingForProbingResult);
    // Dont expect probe results to be larger than a fraction of the actual
    // probe rate.
    min_bitrate_to_probe_further_ = pending_probes.back().target_data_rate *
                                    config_.further_probe_threshold;
  } else {
    UpdateState(State::kProbingComplete);
  }
  return pending_probes;
}

void ProbeController::SetRelayPath(bool relay_path) {
  relay_path_ = relay_path;
  config_ = ProbeControllerConfig();
  if (relay_path) {
    config_.first_exponential_probe_scale = relay_config_.first_probe_scale;
    config_.second_exponential_probe_scale = relay_config_.second_probe_scale;
    config_.alr_probe_scale = relay_config_.alr_probe_scale;
    config_.further_exponential_probe_scale = relay_config_.further_probe_scale;
    // Use the same policy for startup, ALR and recovery. A late recovery
    // probe must not silently revert to short, burst-sensitive sampling.
    config_.min_probe_duration = TimeDelta::Millis(relay_config_.probe_duration_ms);
    config_.min_probe_delta = TimeDelta::Millis(relay_config_.probe_delta_ms);
  }
}

void ProbeController::OnVideoPacketSent() { video_started_ = true; }

void ProbeController::OnVideoSendPaused(bool paused) {
  if (video_paused_ == paused) return;
  video_paused_ = paused;
  video_started_ = false;
  min_valid_probe_id_ = next_probe_cluster_id_;
  initial_retry_deadline_ = Timestamp::MinusInfinity();
  start_bitrate_ = estimated_bitrate_;
  UpdateState(State::kInit);
}

void ProbeController::OnProbeResult(int id, double received_fraction) {
  if (!IsProbeResultValid(id) || id < current_probe_start_id_ ||
      id < last_probe_result_id_)
    return;
  if (id > last_probe_result_id_) last_probe_received_fraction_ = 0;
  last_probe_result_id_ = id;
  last_probe_received_fraction_ =
      std::max(last_probe_received_fraction_, received_fraction);
  initial_retry_deadline_ = Timestamp::MinusInfinity();
}

void ProbeController::OnProbeSent(int id, Timestamp at_time) {
  if (IsProbeResultValid(id) && id >= current_probe_start_id_)
    time_last_probing_initiated_ =
        std::max(time_last_probing_initiated_, at_time);
}

void ProbeController::OnProbeResultRejected(int id) {
  // A later revision can invalidate an earlier partial estimate while the
  // relay controller is waiting for its loss/feedback window to settle.
  if (id == last_probe_result_id_ && id == next_probe_cluster_id_ - 1) {
    last_probe_result_id_ = -1;
    last_probe_received_fraction_ = 0;
  }
}

void ProbeController::OnProbeAborted(int id) {
  if (!IsProbeResultValid(id) || id < current_probe_start_id_) return;
  min_valid_probe_id_ = id + 1;
  initial_retry_deadline_ = Timestamp::MinusInfinity();
  if (id == next_probe_cluster_id_ - 1) UpdateState(State::kProbingComplete);
}

bool ProbeController::IsProbeResultValid(int id) const {
  return id >= min_valid_probe_id_ && id < next_probe_cluster_id_;
}

void ProbeController::OnTransportFeedback(Timestamp at_time, TimeDelta rtt) {
  last_feedback_time_ = at_time;
  if (rtt.IsFinite() && rtt > TimeDelta::Zero())
    feedback_rtt_ = std::min(rtt, TimeDelta::Seconds(1));
}

void ProbeController::SetTransportLossSample(int settled_packets,
                                             int settled_lost) {
  settled_packets_ = settled_packets;
  settled_lost_ = settled_lost;
}

void ProbeController::LogState(Timestamp at_time) const {
  const char* reason = ContinuationBlockReason(at_time);
  LOG_DEBUG("Probe state: state={} latest_id={} result_id={} received_fraction={} "
            "continuation={} startup_retries={} startup_window_ms={}",
            static_cast<int>(state_), next_probe_cluster_id_ - 1,
            last_probe_result_id_, last_probe_received_fraction_,
            reason ? reason : "ready", initial_retry_count_,
            initial_retry_deadline_.IsFinite()
                ? std::max<int64_t>(0, (initial_retry_deadline_ - at_time).ms())
                : 0);
}

}  // namespace webrtc
}  // namespace minirtc
