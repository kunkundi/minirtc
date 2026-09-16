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
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "bitrate_limits.h"
#include "log.h"

namespace minirtc {
namespace webrtc {

namespace {
// Maximum waiting time from the time of initiating probing to getting
// the measured results back.
constexpr TimeDelta kMaxWaitingTimeForProbingResult = TimeDelta::Seconds(1);
constexpr TimeDelta kPathProbeWindow = TimeDelta::Seconds(5);
constexpr TimeDelta kPathProbeRetryInterval = TimeDelta::Seconds(1);
constexpr int kMaxPathProbeAttempts = 3;
constexpr TimeDelta kDropRecoveryWindow = TimeDelta::Seconds(10);
constexpr TimeDelta kDropRecoveryInterval = TimeDelta::Seconds(1);
constexpr int kMaxDropRecoveryAttempts = 3;

// Default probing bitrate limit. Applied only when the application didn't
// specify max bitrate.
constexpr DataRate kDefaultMaxProbingBitrate =
    DataRate::BitsPerSec(kDefaultMaxNetworkBitrateBps);

// Recover a sharp drop with bounded probes at a fraction of the last rate.
constexpr double kBitrateDropThreshold = 0.66;
constexpr double kProbeFractionAfterDrop = 0.85;
constexpr double kProbeUncertainty = 0.05;

// Use probing to recover faster after large bitrate estimate drops.
constexpr char kBweRapidRecoveryExperiment[] =
    "WebRTC-BweRapidRecoveryExperiment";

}  // namespace

ProbeControllerConfig::ProbeControllerConfig()
    : first_exponential_probe_scale(3.0),
      second_exponential_probe_scale(6.0),
      further_exponential_probe_scale(2),
      further_probe_threshold(0.7),
      abort_further_probe_if_max_lower_than_current(false),
      repeated_initial_probing_time_period(TimeDelta::Seconds(5)),
      initial_probe_duration(TimeDelta::Millis(100)),
      initial_min_probe_delta(TimeDelta::Millis(20)),
      alr_probing_interval(TimeDelta::Seconds(5)),
      alr_probe_scale(2),
      network_state_estimate_probing_interval(TimeDelta::PlusInfinity()),
      probe_if_estimate_lower_than_network_state_estimate_ratio(0),
      estimate_lower_than_network_state_estimate_probing_interval(
          TimeDelta::Seconds(3)),
      network_state_probe_scale(1.0),
      network_state_probe_duration(TimeDelta::Millis(15)),
      network_state_min_probe_delta(TimeDelta::Millis(20)),
      probe_on_max_allocated_bitrate_change(true),
      first_allocation_probe_scale(1),
      second_allocation_probe_scale(2),
      allocation_probe_limit_by_current_scale(2),
      min_probe_packets_sent(5),
      min_probe_duration(TimeDelta::Millis(15)),
      min_probe_delta(TimeDelta::Millis(2)),
      loss_limited_probe_scale(1.5),
      skip_if_estimate_larger_than_fraction_of_max(0.0),
      skip_probe_max_allocated_scale(1.0) {}

ProbeControllerConfig::ProbeControllerConfig(const ProbeControllerConfig&) =
    default;
ProbeControllerConfig::~ProbeControllerConfig() = default;

ProbeController::ProbeController()
    : network_available_(false), enable_periodic_alr_probing_(false) {
  Reset(Timestamp::Zero());
}

ProbeController::~ProbeController() {}

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
      if (network_available_) {
        return InitiateExponentialProbing(at_time);
      }
      break;

    case State::kWaitingForProbingResult:
      break;

    case State::kProbingComplete:
      // If the new max bitrate is higher than both the old max bitrate and the
      // estimate then initiate probing.
      if (!estimated_bitrate_.IsZero() && old_max_bitrate < max_bitrate_ &&
          estimated_bitrate_ < max_bitrate_) {
        LOG_DEBUG("Probing complete; max bitrate increased, probe new limit");
        return InitiateProbing(at_time, {max_bitrate_}, false);
      }
      break;
  }
  return std::vector<ProbeClusterConfig>();
}

std::vector<ProbeClusterConfig> ProbeController::OnMaxTotalAllocatedBitrate(
    DataRate max_total_allocated_bitrate, Timestamp at_time) {
  const bool in_alr = alr_start_time_.has_value();
  const bool allow_allocation_probe = in_alr;
  if (config_.probe_on_max_allocated_bitrate_change &&
      state_ == State::kProbingComplete &&
      max_total_allocated_bitrate != max_total_allocated_bitrate_ &&
      estimated_bitrate_ < max_bitrate_ &&
      estimated_bitrate_ < max_total_allocated_bitrate &&
      allow_allocation_probe) {
    max_total_allocated_bitrate_ = max_total_allocated_bitrate;

    if (!config_.first_allocation_probe_scale) {
      return std::vector<ProbeClusterConfig>();
    }

    DataRate first_probe_rate =
        max_total_allocated_bitrate * config_.first_allocation_probe_scale;
    const DataRate current_bwe_limit =
        config_.allocation_probe_limit_by_current_scale * estimated_bitrate_;
    bool limited_by_current_bwe = current_bwe_limit < first_probe_rate;
    if (limited_by_current_bwe) {
      first_probe_rate = current_bwe_limit;
    }

    std::vector<DataRate> probes = {first_probe_rate};
    if (!limited_by_current_bwe && config_.second_allocation_probe_scale) {
      DataRate second_probe_rate =
          max_total_allocated_bitrate * config_.second_allocation_probe_scale;
      limited_by_current_bwe = current_bwe_limit < second_probe_rate;
      if (limited_by_current_bwe) {
        second_probe_rate = current_bwe_limit;
      }
      if (second_probe_rate > first_probe_rate)
        probes.push_back(second_probe_rate);
    }
    bool allow_further_probing = limited_by_current_bwe;
    LOG_DEBUG("Allocation probe allow_further_probing={}",
              allow_further_probing);
    return InitiateProbing(at_time, probes, allow_further_probing);
  }
  if (!max_total_allocated_bitrate.IsZero()) {
    last_allowed_repeated_initial_probe_ = at_time;
  }

  max_total_allocated_bitrate_ = max_total_allocated_bitrate;
  return std::vector<ProbeClusterConfig>();
}

std::vector<ProbeClusterConfig> ProbeController::OnNetworkAvailability(
    NetworkAvailability msg) {
  const bool was_network_available = network_available_;
  network_available_ = msg.network_available;

  if (!network_available_) {
    pending_drop_recovery_.reset();
    drop_recovery_cooldown_ = Timestamp::MinusInfinity();
    pending_path_probe_.reset();
    state_ = State::kInit;
    min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
    last_allowed_repeated_initial_probe_ = Timestamp::Zero();
    return {};
  }

  if (!was_network_available && state_ == State::kInit &&
      !start_bitrate_.IsZero()) {
    return InitiateExponentialProbing(msg.at_time);
  }
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
  // When probing at 1.8 Mbps ( 6x 300), this represents a threshold of
  // 1.2 Mbps to continue probing.
  std::vector<DataRate> probes = {config_.first_exponential_probe_scale *
                                  start_bitrate_};
  if (config_.second_exponential_probe_scale &&
      config_.second_exponential_probe_scale > 0) {
    probes.push_back(config_.second_exponential_probe_scale * start_bitrate_);
  }
  if (repeated_initial_probing_enabled_ &&
      max_total_allocated_bitrate_.IsZero()) {
    last_allowed_repeated_initial_probe_ =
        at_time + config_.repeated_initial_probing_time_period;
    LOG_DEBUG(
        "Repeated initial probing enabled, last allowed probe: {} now: {}",
        last_allowed_repeated_initial_probe_.ms(), at_time.ms());
  }

  return InitiateProbing(at_time, probes, true);
}

std::vector<ProbeClusterConfig> ProbeController::SetEstimatedBitrate(
    DataRate bitrate, BandwidthLimitedCause bandwidth_limited_cause,
    Timestamp at_time, bool recovery_network_healthy) {
  bandwidth_limited_cause_ = bandwidth_limited_cause;
  recovery_network_healthy_ = recovery_network_healthy;
  if (pending_drop_recovery_) {
    auto& recovery = *pending_drop_recovery_;
    const DataRate expected = recovery.reference_rate *
                              kProbeFractionAfterDrop * (1 - kProbeUncertainty);
    if (bitrate >= expected) {
      if (recovery.recovered_since.IsInfinite())
        recovery.recovered_since = at_time;
    } else {
      recovery.recovered_since = Timestamp::PlusInfinity();
    }
  }
  if (bitrate < kBitrateDropThreshold * estimated_bitrate_) {
    if (network_available_ && !pending_drop_recovery_ &&
        at_time >= drop_recovery_cooldown_) {
      pending_drop_recovery_ =
          DropRecovery{estimated_bitrate_, at_time + kDropRecoveryWindow,
                       at_time + TimeDelta::Millis(500)};
    }
  }
  estimated_bitrate_ = bitrate;
  if (state_ == State::kWaitingForProbingResult) {
    // Continue probing if probing results indicate channel has greater
    // capacity unless we already reached the needed bitrate.
    if (config_.abort_further_probe_if_max_lower_than_current &&
        (bitrate > max_bitrate_ ||
         (!max_total_allocated_bitrate_.IsZero() &&
          bitrate > 2 * max_total_allocated_bitrate_))) {
      // No need to continue probing.
      min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
    }
    DataRate network_state_estimate_probe_further_limit =
        config_.network_state_estimate_probing_interval.IsFinite() &&
                network_estimate_ &&
                network_estimate_->link_capacity_upper.IsFinite()
            ? network_estimate_->link_capacity_upper *
                  config_.further_probe_threshold
            : DataRate::PlusInfinity();
    if (bitrate > min_bitrate_to_probe_further_ &&
        bitrate <= network_state_estimate_probe_further_limit) {
      LOG_DEBUG(
          "Probe estimate qualified for further probing: measured_bps={} "
          "minimum_bps={} upper_limit_bps={}",
          bitrate.bps(), min_bitrate_to_probe_further_.bps(),
          network_state_estimate_probe_further_limit.bps());
      return InitiateProbing(
          at_time, {config_.further_exponential_probe_scale * bitrate}, true);
    }
  }
  return {};
}

void ProbeController::EnablePeriodicAlrProbing(bool enable) {
  enable_periodic_alr_probing_ = enable;
}

void ProbeController::EnableRepeatedInitialProbing(bool enable) {
  repeated_initial_probing_enabled_ = enable;
  if (!enable) {
    last_allowed_repeated_initial_probe_ = Timestamp::Zero();
  }
}

void ProbeController::SetRelayPath(bool relay_path) {
  if (relay_path_ == relay_path) {
    return;
  }

  relay_path_ = relay_path;
  pending_path_probe_.reset();
  pending_drop_recovery_.reset();
  drop_recovery_cooldown_ = Timestamp::MinusInfinity();
  config_ = ProbeControllerConfig();
  if (relay_path_) {
    // Starting from 1.5 Mbps, probe at 3 and 6 Mbps rather than the direct
    // path's 7.5 and 15 Mbps. Continue in 1.5x steps only after receiving at
    // least 85% of the preceding target, and stop repeating bootstrap probes
    // sooner. This reduces the chance that padding competes with the first
    // keyframes on a lossy TURN allocation.
    config_.first_exponential_probe_scale = 2.0;
    config_.second_exponential_probe_scale = 4.0;
    config_.further_exponential_probe_scale = 1.5;
    config_.further_probe_threshold = 0.85;
    config_.repeated_initial_probing_time_period = TimeDelta::Seconds(3);
    config_.initial_probe_duration = TimeDelta::Millis(80);

    // Apply the same restraint to later allocation and ALR probes so the
    // relay is not hit by a second abrupt burst immediately after startup.
    config_.alr_probing_interval = TimeDelta::Seconds(8);
    config_.alr_probe_scale = 1.5;
    config_.second_allocation_probe_scale = 1.5;
    config_.allocation_probe_limit_by_current_scale = 1.5;
    config_.loss_limited_probe_scale = 1.25;
  }

  LOG_INFO(
      "Bandwidth probe profile updated: path={} initial_scales={}/{} "
      "further_scale={} further_threshold={} repeated_window_ms={}",
      relay_path_ ? "relay" : "direct",
      config_.first_exponential_probe_scale,
      config_.second_exponential_probe_scale,
      config_.further_exponential_probe_scale,
      config_.further_probe_threshold,
      config_.repeated_initial_probing_time_period.ms());
}

std::vector<ProbeClusterConfig> ProbeController::RequestProbeOnPathChange(
    Timestamp at_time) {
  pending_drop_recovery_.reset();
  if (relay_path_ || !network_available_ || estimated_bitrate_.IsZero()) {
    return {};
  }
  if (!pending_path_probe_) {
    pending_path_probe_ =
        PathProbe{at_time + kPathProbeWindow, Timestamp::MinusInfinity(),
                  std::nullopt, 0};
  }
  return MaybeProbeNewPath(at_time);
}

void ProbeController::OnProbeResult(int cluster_id, Timestamp at_time) {
  if (pending_path_probe_ && pending_path_probe_->cluster_id == cluster_id &&
      at_time <= pending_path_probe_->deadline) {
    pending_path_probe_.reset();
  }
}

std::vector<ProbeClusterConfig> ProbeController::MaybeProbeNewPath(
    Timestamp at_time) {
  if (!pending_path_probe_) return {};
  if (!network_available_ || relay_path_) {
    pending_path_probe_.reset();
    return {};
  }
  if (at_time >= pending_path_probe_->deadline) {
    pending_path_probe_.reset();
    return {};
  }
  if (estimated_bitrate_.IsZero() || state_ != State::kProbingComplete ||
      at_time - pending_path_probe_->last_probe_time <
          kPathProbeRetryInterval) {
    return {};
  }
  if (pending_path_probe_->attempts >= kMaxPathProbeAttempts) {
    pending_path_probe_.reset();
    return {};
  }

  // One step above the current estimate; further steps require feedback.
  // InitiateProbing also enforces congestion and configured bitrate limits.
  auto probes = InitiateProbing(
      at_time, {estimated_bitrate_ * config_.alr_probe_scale}, true);
  if (!probes.empty()) {
    pending_path_probe_->cluster_id = probes.front().id;
    pending_path_probe_->last_probe_time = at_time;
    ++pending_path_probe_->attempts;
  }
  return probes;
}

void ProbeController::SetAlrStartTimeMs(
    std::optional<int64_t> alr_start_time_ms) {
  if (alr_start_time_ms) {
    alr_start_time_ = Timestamp::Millis(*alr_start_time_ms);
  } else {
    alr_start_time_ = std::nullopt;
  }
}
void ProbeController::SetAlrEndedTimeMs(int64_t alr_end_time_ms) {
  alr_end_time_.emplace(Timestamp::Millis(alr_end_time_ms));
}

std::vector<ProbeClusterConfig> ProbeController::RequestProbe(
    Timestamp at_time) {
  return MaybeProbeAfterDrop(at_time);
}

std::vector<ProbeClusterConfig> ProbeController::MaybeProbeAfterDrop(
    Timestamp at_time) {
  if (!pending_drop_recovery_) return {};
  auto& recovery = *pending_drop_recovery_;
  // The estimator may revise the same cluster after more feedback arrives.
  // Confirm the adopted media rate, not the first optimistic packet result.
  if (recovery_network_healthy_ &&
      bandwidth_limited_cause_ == BandwidthLimitedCause::kDelayBasedLimited &&
      recovery.recovered_since.IsFinite() &&
      at_time - recovery.recovered_since >= TimeDelta::Seconds(1)) {
    pending_drop_recovery_.reset();
    return {};
  }
  if (!network_available_ || at_time >= recovery.deadline ||
      (recovery.attempts >= kMaxDropRecoveryAttempts &&
       at_time >= recovery.next_attempt &&
       recovery.recovered_since.IsInfinite())) {
    pending_drop_recovery_.reset();
    drop_recovery_cooldown_ = at_time + kDropRecoveryWindow;
    return {};
  }
  if (!recovery_network_healthy_ ||
      bandwidth_limited_cause_ != BandwidthLimitedCause::kDelayBasedLimited ||
      recovery.recovered_since.IsFinite() || pending_path_probe_ ||
      state_ != State::kProbingComplete || at_time < recovery.next_attempt) {
    return {};
  }
  // Probe capacity rather than raising the media rate without evidence. The
  // normal congestion and configured-rate limits still apply to this burst.
  auto probes = InitiateProbing(
      at_time, {recovery.reference_rate * kProbeFractionAfterDrop}, false);
  recovery.next_attempt = at_time + kDropRecoveryInterval;
  if (!probes.empty()) {
    ++recovery.attempts;
  }
  return probes;
}

void ProbeController::SetNetworkStateEstimate(
    webrtc::NetworkStateEstimate estimate) {
  network_estimate_ = estimate;
}

void ProbeController::Reset(Timestamp at_time) {
  pending_path_probe_.reset();
  pending_drop_recovery_.reset();
  drop_recovery_cooldown_ = Timestamp::MinusInfinity();
  recovery_network_healthy_ = true;
  bandwidth_limited_cause_ = BandwidthLimitedCause::kDelayBasedLimited;
  state_ = State::kInit;
  min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
  time_last_probing_initiated_ = Timestamp::Zero();
  estimated_bitrate_ = DataRate::Zero();
  network_estimate_ = std::nullopt;
  start_bitrate_ = DataRate::Zero();
  max_bitrate_ = kDefaultMaxProbingBitrate;
  alr_end_time_.reset();
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

bool ProbeController::TimeForNetworkStateProbe(Timestamp at_time) const {
  if (!network_estimate_ ||
      network_estimate_->link_capacity_upper.IsInfinite()) {
    return false;
  }

  bool probe_due_to_low_estimate =
      bandwidth_limited_cause_ == BandwidthLimitedCause::kDelayBasedLimited &&
      estimated_bitrate_ <
          config_.probe_if_estimate_lower_than_network_state_estimate_ratio *
              network_estimate_->link_capacity_upper;
  if (probe_due_to_low_estimate &&
      config_.estimate_lower_than_network_state_estimate_probing_interval
          .IsFinite()) {
    Timestamp next_probe_time =
        time_last_probing_initiated_ +
        config_.estimate_lower_than_network_state_estimate_probing_interval;
    return at_time >= next_probe_time;
  }

  bool periodic_probe =
      estimated_bitrate_ < network_estimate_->link_capacity_upper;
  if (periodic_probe &&
      config_.network_state_estimate_probing_interval.IsFinite()) {
    Timestamp next_probe_time = time_last_probing_initiated_ +
                                config_.network_state_estimate_probing_interval;
    return at_time >= next_probe_time;
  }

  return false;
}

bool ProbeController::TimeForNextRepeatedInitialProbe(Timestamp at_time) const {
  if (repeated_initial_probing_enabled_ &&
      state_ != State::kWaitingForProbingResult &&
      last_allowed_repeated_initial_probe_ > at_time) {
    Timestamp next_probe_time =
        time_last_probing_initiated_ + kMaxWaitingTimeForProbingResult;
    if (at_time >= next_probe_time) {
      return true;
    }
  }
  return false;
}

std::vector<ProbeClusterConfig> ProbeController::Process(Timestamp at_time) {
  if (at_time - time_last_probing_initiated_ >
      kMaxWaitingTimeForProbingResult) {
    if (state_ == State::kWaitingForProbingResult) {
      LOG_DEBUG(
          "Probe did not qualify for further probing: measured_bps={} "
          "minimum_bps={} elapsed_ms={}",
          estimated_bitrate_.bps(), min_bitrate_to_probe_further_.bps(),
          (at_time - time_last_probing_initiated_).ms());
      UpdateState(State::kProbingComplete);
    }
  }

  auto path_probes = MaybeProbeNewPath(at_time);
  if (!path_probes.empty()) return path_probes;
  if (pending_drop_recovery_) return MaybeProbeAfterDrop(at_time);
  if (estimated_bitrate_.IsZero() || state_ != State::kProbingComplete) {
    return {};
  }
  if (TimeForNextRepeatedInitialProbe(at_time)) {
    return InitiateProbing(
        at_time, {estimated_bitrate_ * config_.first_exponential_probe_scale},
        true);
  }
  if (TimeForAlrProbe(at_time) || TimeForNetworkStateProbe(at_time)) {
    return InitiateProbing(
        at_time, {estimated_bitrate_ * config_.alr_probe_scale}, true);
  }
  return std::vector<ProbeClusterConfig>();
}

ProbeClusterConfig ProbeController::CreateProbeClusterConfig(Timestamp at_time,
                                                             DataRate bitrate) {
  ProbeClusterConfig config;
  config.at_time = at_time;
  config.target_data_rate = bitrate;
  if (network_estimate_ &&
      config_.network_state_estimate_probing_interval.IsFinite() &&
      network_estimate_->link_capacity_upper.IsFinite() &&
      network_estimate_->link_capacity_upper >= bitrate) {
    config.target_duration = config_.network_state_probe_duration;
    config.min_probe_delta = config_.network_state_min_probe_delta;
  } else if (at_time < last_allowed_repeated_initial_probe_) {
    config.target_duration = config_.initial_probe_duration;
    config.min_probe_delta = config_.initial_min_probe_delta;
  } else {
    config.target_duration = config_.min_probe_duration;
    config.min_probe_delta = config_.min_probe_delta;
  }
  config.target_probe_count = config_.min_probe_packets_sent;
  config.id = next_probe_cluster_id_;
  next_probe_cluster_id_++;
  LOG_DEBUG(
      "Probe cluster created: id={} target_bitrate_bps={} target_bytes={} "
      "target_probe_batches={} duration_ms={} min_probe_delta_ms={}",
      config.id, config.target_data_rate.bps(),
      (config.target_data_rate * config.target_duration).bytes(),
      config.target_probe_count, config.target_duration.ms(),
      config.min_probe_delta.ms());
  return config;
}

std::vector<ProbeClusterConfig> ProbeController::InitiateProbing(
    Timestamp now, std::vector<DataRate> bitrates_to_probe,
    bool probe_further) {
  if (config_.skip_if_estimate_larger_than_fraction_of_max > 0) {
    DataRate network_estimate = network_estimate_
                                    ? network_estimate_->link_capacity_upper
                                    : DataRate::PlusInfinity();
    DataRate max_probe_rate =
        max_total_allocated_bitrate_.IsZero()
            ? max_bitrate_
            : std::min(config_.skip_probe_max_allocated_scale *
                           max_total_allocated_bitrate_,
                       max_bitrate_);
    if (std::min(network_estimate, estimated_bitrate_) >
        config_.skip_if_estimate_larger_than_fraction_of_max * max_probe_rate) {
      UpdateState(State::kProbingComplete);
      return {};
    }
  }

  DataRate max_probe_bitrate = max_bitrate_;
  if (max_total_allocated_bitrate_ > DataRate::Zero()) {
    // If a max allocated bitrate has been configured, allow probing up to 2x
    // that rate. This allows some overhead to account for bursty streams,
    // which otherwise would have to ramp up when the overshoot is already in
    // progress.
    // It also avoids minor quality reduction caused by probes often being
    // received at slightly less than the target probe bitrate.
    max_probe_bitrate =
        std::min(max_probe_bitrate, max_total_allocated_bitrate_ * 2);
  }

  switch (bandwidth_limited_cause_) {
    case BandwidthLimitedCause::kRttBasedBackOffHighRtt:
    case BandwidthLimitedCause::kDelayBasedLimitedDelayIncreased:
    case BandwidthLimitedCause::kLossLimitedBwe:
      LOG_DEBUG("Not sending probe in bandwidth limited state {}",
                static_cast<int>(bandwidth_limited_cause_));
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

  if (config_.network_state_estimate_probing_interval.IsFinite() &&
      network_estimate_ && network_estimate_->link_capacity_upper.IsFinite()) {
    if (network_estimate_->link_capacity_upper.IsZero()) {
      return {};
    }
    max_probe_bitrate = std::min(
        {max_probe_bitrate,
         std::max(estimated_bitrate_, network_estimate_->link_capacity_upper *
                                          config_.network_state_probe_scale)});
  }

  std::vector<ProbeClusterConfig> pending_probes;
  for (DataRate bitrate : bitrates_to_probe) {
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

}  // namespace webrtc
}  // namespace minirtc
