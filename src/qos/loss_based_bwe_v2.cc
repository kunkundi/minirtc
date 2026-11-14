/*
 *  Copyright 2021 The WebRTC project authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "loss_based_bwe_v2.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "bwe_defines.h"
#include "log.h"

namespace minirtc {
namespace webrtc {

namespace {

constexpr TimeDelta kInitHoldDuration = TimeDelta::Millis(300);
constexpr TimeDelta kMaxHoldDuration = TimeDelta::Seconds(60);

bool IsValid(DataRate datarate) { return datarate.IsFinite(); }

bool IsValid(std::optional<DataRate> datarate) {
  return datarate.has_value() && IsValid(datarate.value());
}

bool IsValid(Timestamp timestamp) { return timestamp.IsFinite(); }

double ToKiloBytes(DataSize datasize) { return datasize.bytes() / 1000.0; }

double GetLossProbability(double inherent_loss, DataRate loss_limited_bandwidth,
                          DataRate sending_rate) {
  if (inherent_loss < 0.0 || inherent_loss > 1.0) {
    LOG_WARN("The inherent loss must be in [0,1]: {}", inherent_loss);
    inherent_loss = std::min(std::max(inherent_loss, 0.0), 1.0);
  }
  if (!sending_rate.IsFinite()) {
    LOG_WARN("The sending rate must be finite: {}", ToString(sending_rate));
  }
  if (!loss_limited_bandwidth.IsFinite()) {
    LOG_WARN("The loss limited bandwidth must be finite: {}",
             ToString(loss_limited_bandwidth));
  }

  double loss_probability = inherent_loss;
  if (IsValid(sending_rate) && IsValid(loss_limited_bandwidth) &&
      (sending_rate > loss_limited_bandwidth)) {
    loss_probability += (1 - inherent_loss) *
                        (sending_rate - loss_limited_bandwidth) / sending_rate;
  }
  return std::min(std::max(loss_probability, 1.0e-6), 1.0 - 1.0e-6);
}

}  // namespace

LossBasedBweV2::LossBasedBweV2(double max_increase_factor)
    : config_([max_increase_factor] {
        Config config;
        config.max_increase_factor = std::isfinite(max_increase_factor)
            ? std::max(1.0, max_increase_factor) : 1.3;
        return config;
      }()) {
  current_best_estimate_.inherent_loss = config_.initial_inherent_loss_estimate;
  observations_.resize(config_.observation_window_size);
  temporal_weights_.resize(config_.observation_window_size);
  instant_upper_bound_temporal_weights_.resize(config_.observation_window_size);
  CalculateTemporalWeights();
  last_hold_info_.duration = kInitHoldDuration;
}

bool LossBasedBweV2::IsReady() const {
  return IsValid(current_best_estimate_.loss_limited_bandwidth) &&
         num_observations_ >= config_.min_num_observations;
}

bool LossBasedBweV2::ReadyToUseInStartPhase() const {
  return IsReady() && config_.use_in_start_phase;
}

LossBasedBweV2::Result LossBasedBweV2::GetLossBasedResult() const {
  if (!IsReady()) {
    return {IsValid(delay_based_estimate_) ? delay_based_estimate_
                                           : DataRate::PlusInfinity(),
            LossBasedState::kDelayBasedEstimate};
  }
  return loss_based_result_;
}

void LossBasedBweV2::SetAcknowledgedBitrate(DataRate acknowledged_bitrate) {
  if (IsValid(acknowledged_bitrate)) {
    acknowledged_bitrate_ = acknowledged_bitrate;
    CalculateInstantLowerBound();
  } else {
    LOG_WARN("The acknowledged bitrate must be finite: {}",
             ToString(acknowledged_bitrate));
  }
}

void LossBasedBweV2::SetMinMaxBitrate(DataRate min_bitrate,
                                      DataRate max_bitrate) {
  if (IsValid(min_bitrate)) {
    min_bitrate_ = min_bitrate;
    CalculateInstantLowerBound();
  } else {
    LOG_WARN("The min bitrate must be finite: {}", ToString(min_bitrate));
  }

  if (IsValid(max_bitrate)) {
    max_bitrate_ = max_bitrate;
  } else {
    LOG_WARN("The max bitrate must be finite: {}", ToString(max_bitrate));
  }
}

void LossBasedBweV2::UpdateBandwidthEstimate(
    const std::vector<PacketResult>& packet_results,
    DataRate delay_based_estimate, bool in_alr) {
  delay_based_estimate_ = delay_based_estimate;

  if (packet_results.empty()) {
    LOG_DEBUG("The estimate cannot be updated without any loss statistics.");
    return;
  }

  if (!PushBackObservation(packet_results)) {
    return;
  }

  if (!IsValid(current_best_estimate_.loss_limited_bandwidth)) {
    if (!IsValid(delay_based_estimate)) {
      LOG_WARN("The delay based estimate must be finite: {}",
               ToString(delay_based_estimate));
      return;
    }
    current_best_estimate_.loss_limited_bandwidth = delay_based_estimate;
    loss_based_result_ = {delay_based_estimate,
                          LossBasedState::kDelayBasedEstimate};
  }

  ChannelParameters best_candidate = current_best_estimate_;
  double objective_max = std::numeric_limits<double>::lowest();
  for (ChannelParameters candidate : GetCandidates(in_alr)) {
    NewtonsMethodUpdate(candidate);

    const double candidate_objective = GetObjective(candidate);
    if (candidate_objective > objective_max ||
        (candidate_objective == objective_max &&
         candidate.loss_limited_bandwidth >
             best_candidate.loss_limited_bandwidth)) {
      // Select the candidate with the highest objective or the one with the
      // same objective but higher bandwidth if they are tied.
      objective_max = candidate_objective;
      best_candidate = candidate;
    }
  }
  if (best_candidate.loss_limited_bandwidth <
      current_best_estimate_.loss_limited_bandwidth) {
    last_time_estimate_reduced_ = last_send_time_most_recent_observation_;
  }

  // Do not increase the estimate if the average loss is greater than current
  // inherent loss.
  if (average_reported_loss_ratio_ > best_candidate.inherent_loss &&
      config_.not_increase_if_inherent_loss_less_than_average_loss &&
      current_best_estimate_.loss_limited_bandwidth <
          best_candidate.loss_limited_bandwidth) {
    best_candidate.loss_limited_bandwidth =
        current_best_estimate_.loss_limited_bandwidth;
  }

  if (IsInLossLimitedState()) {
    // Bound the estimate increase if:
    // 1. The estimate has been increased for less than
    // `delayed_increase_window` ago, and
    // 2. The best candidate is greater than bandwidth_limit_in_current_window.
    if (recovering_after_loss_timestamp_.IsFinite() &&
        recovering_after_loss_timestamp_ + config_.delayed_increase_window >
            last_send_time_most_recent_observation_ &&
        best_candidate.loss_limited_bandwidth >
            bandwidth_limit_in_current_window_) {
      best_candidate.loss_limited_bandwidth =
          bandwidth_limit_in_current_window_;
    }

    bool increasing_when_loss_limited = IsEstimateIncreasingWhenLossLimited(
        /*old_estimate=*/current_best_estimate_.loss_limited_bandwidth,
        /*new_estimate=*/best_candidate.loss_limited_bandwidth);
    // Bound the best candidate by the acked bitrate.
    if (increasing_when_loss_limited && IsValid(acknowledged_bitrate_)) {
      double rampup_factor = config_.bandwidth_rampup_upper_bound_factor;
      if (IsValid(last_hold_info_.rate) &&
          acknowledged_bitrate_ <
              config_.bandwidth_rampup_hold_threshold * last_hold_info_.rate) {
        rampup_factor = config_.bandwidth_rampup_upper_bound_factor_in_hold;
      }

      best_candidate.loss_limited_bandwidth =
          std::max(current_best_estimate_.loss_limited_bandwidth,
                   std::min(best_candidate.loss_limited_bandwidth,
                            rampup_factor * (*acknowledged_bitrate_)));
      // Increase current estimate by at least 1kbps to make sure that the state
      // will be switched to kIncreasing, thus padding is triggered.
      if (loss_based_result_.state == LossBasedState::kDecreasing &&
          best_candidate.loss_limited_bandwidth ==
              current_best_estimate_.loss_limited_bandwidth) {
        best_candidate.loss_limited_bandwidth =
            current_best_estimate_.loss_limited_bandwidth +
            DataRate::BitsPerSec(1);
      }
    }
  }

  DataRate bounded_bandwidth_estimate = DataRate::PlusInfinity();
  if (IsValid(delay_based_estimate_)) {
    bounded_bandwidth_estimate =
        std::max(GetInstantLowerBound(),
                 std::min({best_candidate.loss_limited_bandwidth,
                           GetInstantUpperBound(), delay_based_estimate_}));
  } else {
    bounded_bandwidth_estimate = std::max(
        GetInstantLowerBound(), std::min(best_candidate.loss_limited_bandwidth,
                                         GetInstantUpperBound()));
  }
  if (config_.bound_best_candidate &&
      bounded_bandwidth_estimate < best_candidate.loss_limited_bandwidth) {
    // If network is lossy, cap the best estimate by the instant upper bound,
    // e.g. 450kbps if loss rate is 50%.
    // Otherwise, cap the estimate by the delay-based estimate or max_bitrate.
    current_best_estimate_.loss_limited_bandwidth = bounded_bandwidth_estimate;
    current_best_estimate_.inherent_loss = 0;
  } else {
    current_best_estimate_ = best_candidate;
    if (config_.lower_bound_by_acked_rate_factor > 0.0) {
      current_best_estimate_.loss_limited_bandwidth =
          std::max(current_best_estimate_.loss_limited_bandwidth,
                   GetInstantLowerBound());
    }
  }

  if (loss_based_result_.state == LossBasedState::kDecreasing &&
      last_hold_info_.timestamp > last_send_time_most_recent_observation_ &&
      bounded_bandwidth_estimate < delay_based_estimate_) {
    // Ensure that acked rate is the lower bound of HOLD rate.
    if (config_.lower_bound_by_acked_rate_factor > 0.0) {
      last_hold_info_.rate =
          std::max(GetInstantLowerBound(), last_hold_info_.rate);
    }
    // BWE is not allowed to increase above the HOLD rate. The purpose of
    // HOLD is to not immediately ramp up BWE to a rate that may cause loss.
    loss_based_result_.bandwidth_estimate =
        std::min(last_hold_info_.rate, bounded_bandwidth_estimate);
    return;
  }

  if (IsEstimateIncreasingWhenLossLimited(
          /*old_estimate=*/loss_based_result_.bandwidth_estimate,
          /*new_estimate=*/bounded_bandwidth_estimate) &&
      CanKeepIncreasingState(bounded_bandwidth_estimate) &&
      bounded_bandwidth_estimate < delay_based_estimate_ &&
      bounded_bandwidth_estimate < max_bitrate_) {
    if (config_.padding_duration > TimeDelta::Zero() &&
        bounded_bandwidth_estimate > last_padding_info_.padding_rate) {
      // Start a new padding duration.
      last_padding_info_.padding_rate = bounded_bandwidth_estimate;
      last_padding_info_.padding_timestamp =
          last_send_time_most_recent_observation_;
    }
    loss_based_result_.state = config_.padding_duration > TimeDelta::Zero()
                                   ? LossBasedState::kIncreaseUsingPadding
                                   : LossBasedState::kIncreasing;
  } else if (bounded_bandwidth_estimate < delay_based_estimate_ &&
             bounded_bandwidth_estimate < max_bitrate_) {
    if (loss_based_result_.state != LossBasedState::kDecreasing &&
        config_.hold_duration_factor > 0) {
      LOG_DEBUG(
          "Switch to HOLD. Bounded BWE: {}, duration: {}, avg loss rate: {}",
          bounded_bandwidth_estimate.kbps(), last_hold_info_.duration.ms(),
          average_reported_loss_ratio_);
      last_hold_info_ = {
          last_send_time_most_recent_observation_ + last_hold_info_.duration,
          std::min(kMaxHoldDuration,
                   last_hold_info_.duration * config_.hold_duration_factor),
          bounded_bandwidth_estimate};
    }
    last_padding_info_ = PaddingInfo();
    loss_based_result_.state = LossBasedState::kDecreasing;
  } else {
    // Reset the HOLD info if delay based estimate works to avoid getting
    // stuck in low bitrate.
    last_hold_info_ = {Timestamp::MinusInfinity(), kInitHoldDuration,
                       DataRate::PlusInfinity()};
    last_padding_info_ = PaddingInfo();
    loss_based_result_.state = LossBasedState::kDelayBasedEstimate;
  }
  loss_based_result_.bandwidth_estimate = bounded_bandwidth_estimate;

  if (IsInLossLimitedState() &&
      (recovering_after_loss_timestamp_.IsInfinite() ||
       recovering_after_loss_timestamp_ + config_.delayed_increase_window <
           last_send_time_most_recent_observation_)) {
    bandwidth_limit_in_current_window_ =
        std::max(kCongestionControllerMinBitrate,
                 current_best_estimate_.loss_limited_bandwidth *
                     config_.max_increase_factor);
    recovering_after_loss_timestamp_ = last_send_time_most_recent_observation_;
  }
}

bool LossBasedBweV2::IsEstimateIncreasingWhenLossLimited(
    DataRate old_estimate, DataRate new_estimate) {
  return (old_estimate < new_estimate ||
          (old_estimate == new_estimate &&
           (loss_based_result_.state == LossBasedState::kIncreasing ||
            loss_based_result_.state ==
                LossBasedState::kIncreaseUsingPadding))) &&
         IsInLossLimitedState();
}

void LossBasedBweV2::UpdateAverageReportedLossRatio() {
  average_reported_loss_ratio_ =
      (config_.use_byte_loss_rate ? CalculateAverageReportedByteLossRatio()
                                  : CalculateAverageReportedPacketLossRatio());
}

double LossBasedBweV2::CalculateAverageReportedPacketLossRatio() const {
  if (num_observations_ <= 0) {
    return 0.0;
  }

  double num_packets = 0;
  double num_lost_packets = 0;
  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized()) {
      continue;
    }

    double instant_temporal_weight =
        instant_upper_bound_temporal_weights_[(num_observations_ - 1) -
                                              observation.id];
    num_packets += instant_temporal_weight * observation.num_packets;
    num_lost_packets += instant_temporal_weight * observation.num_lost_packets;
  }

  return num_lost_packets / num_packets;
}

double LossBasedBweV2::CalculateAverageReportedByteLossRatio() const {
  if (num_observations_ <= 0) {
    return 0.0;
  }

  DataSize total_bytes = DataSize::Zero();
  DataSize lost_bytes = DataSize::Zero();
  double min_loss_rate = 1.0;
  double max_loss_rate = 0.0;
  DataSize min_lost_bytes = DataSize::Zero();
  DataSize max_lost_bytes = DataSize::Zero();
  DataSize min_bytes_received = DataSize::Zero();
  DataSize max_bytes_received = DataSize::Zero();
  DataRate send_rate_of_max_loss_observation = DataRate::Zero();
  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized()) {
      continue;
    }

    double instant_temporal_weight =
        instant_upper_bound_temporal_weights_[(num_observations_ - 1) -
                                              observation.id];
    total_bytes += instant_temporal_weight * observation.size;
    lost_bytes += instant_temporal_weight * observation.lost_size;

    double loss_rate = !observation.size.IsZero()
                           ? observation.lost_size / observation.size
                           : 0.0;
    if (num_observations_ > 3) {
      if (loss_rate > max_loss_rate) {
        max_loss_rate = loss_rate;
        max_lost_bytes = instant_temporal_weight * observation.lost_size;
        max_bytes_received = instant_temporal_weight * observation.size;
        send_rate_of_max_loss_observation = observation.sending_rate;
      }
      if (loss_rate < min_loss_rate) {
        min_loss_rate = loss_rate;
        min_lost_bytes = instant_temporal_weight * observation.lost_size;
        min_bytes_received = instant_temporal_weight * observation.size;
      }
    }
  }
  if (GetMedianSendingRate() * config_.median_sending_rate_factor <=
      send_rate_of_max_loss_observation) {
    // If the median sending rate is less than half of the sending rate of the
    // observation with max loss rate, i.e. we suddenly send a lot of data, then
    // the loss rate might not be due to a spike.
    return lost_bytes / total_bytes;
  }

  if (total_bytes == max_bytes_received + min_bytes_received) {
    // It could happen if the observation window was 2.
    return lost_bytes / total_bytes;
  }

  return (lost_bytes - min_lost_bytes - max_lost_bytes) /
         (total_bytes - max_bytes_received - min_bytes_received);
}

DataRate LossBasedBweV2::GetCandidateBandwidthUpperBound() const {
  DataRate candidate_bandwidth_upper_bound = max_bitrate_;
  if (IsInLossLimitedState() && IsValid(bandwidth_limit_in_current_window_)) {
    candidate_bandwidth_upper_bound = bandwidth_limit_in_current_window_;
  }

  if (!acknowledged_bitrate_.has_value())
    return candidate_bandwidth_upper_bound;

  if (config_.rampup_acceleration_max_factor > 0.0) {
    const TimeDelta time_since_bandwidth_reduced = std::min(
        config_.rampup_acceleration_maxout_time,
        std::max(TimeDelta::Zero(), last_send_time_most_recent_observation_ -
                                        last_time_estimate_reduced_));
    const double rampup_acceleration = config_.rampup_acceleration_max_factor *
                                       time_since_bandwidth_reduced /
                                       config_.rampup_acceleration_maxout_time;

    candidate_bandwidth_upper_bound +=
        rampup_acceleration * (*acknowledged_bitrate_);
  }
  return candidate_bandwidth_upper_bound;
}

std::vector<LossBasedBweV2::ChannelParameters> LossBasedBweV2::GetCandidates(
    bool in_alr) const {
  ChannelParameters best_estimate = current_best_estimate_;
  std::vector<DataRate> bandwidths;
  for (double candidate_factor : config_.candidate_factors) {
    bandwidths.push_back(candidate_factor *
                         best_estimate.loss_limited_bandwidth);
  }

  if (acknowledged_bitrate_.has_value() &&
      config_.append_acknowledged_rate_candidate) {
    if (!(config_.not_use_acked_rate_in_alr && in_alr) ||
        (config_.padding_duration > TimeDelta::Zero() &&
         last_padding_info_.padding_timestamp + config_.padding_duration >=
             last_send_time_most_recent_observation_)) {
      bandwidths.push_back(*acknowledged_bitrate_ *
                           config_.bandwidth_backoff_lower_bound_factor);
    }
  }

  if (IsValid(delay_based_estimate_) &&
      config_.append_delay_based_estimate_candidate) {
    if (delay_based_estimate_ > best_estimate.loss_limited_bandwidth) {
      bandwidths.push_back(delay_based_estimate_);
    }
  }

  if (in_alr && config_.append_upper_bound_candidate_in_alr &&
      best_estimate.loss_limited_bandwidth > GetInstantUpperBound()) {
    bandwidths.push_back(GetInstantUpperBound());
  }

  const DataRate candidate_bandwidth_upper_bound =
      GetCandidateBandwidthUpperBound();

  std::vector<ChannelParameters> candidates;
  candidates.resize(bandwidths.size());
  for (size_t i = 0; i < bandwidths.size(); ++i) {
    ChannelParameters candidate = best_estimate;
    candidate.loss_limited_bandwidth =
        std::min(bandwidths[i], std::max(best_estimate.loss_limited_bandwidth,
                                         candidate_bandwidth_upper_bound));
    candidate.inherent_loss = GetFeasibleInherentLoss(candidate);
    candidates[i] = candidate;
  }
  return candidates;
}

LossBasedBweV2::Derivatives LossBasedBweV2::GetDerivatives(
    const ChannelParameters& channel_parameters) const {
  Derivatives derivatives;

  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized()) {
      continue;
    }

    double loss_probability = GetLossProbability(
        channel_parameters.inherent_loss,
        channel_parameters.loss_limited_bandwidth, observation.sending_rate);

    double temporal_weight =
        temporal_weights_[(num_observations_ - 1) - observation.id];
    if (config_.use_byte_loss_rate) {
      derivatives.first +=
          temporal_weight *
          ((ToKiloBytes(observation.lost_size) / loss_probability) -
           (ToKiloBytes(observation.size - observation.lost_size) /
            (1.0 - loss_probability)));
      derivatives.second -=
          temporal_weight *
          ((ToKiloBytes(observation.lost_size) /
            std::pow(loss_probability, 2)) +
           (ToKiloBytes(observation.size - observation.lost_size) /
            std::pow(1.0 - loss_probability, 2)));
    } else {
      derivatives.first +=
          temporal_weight *
          ((observation.num_lost_packets / loss_probability) -
           (observation.num_received_packets / (1.0 - loss_probability)));
      derivatives.second -=
          temporal_weight *
          ((observation.num_lost_packets / std::pow(loss_probability, 2)) +
           (observation.num_received_packets /
            std::pow(1.0 - loss_probability, 2)));
    }
  }

  if (derivatives.second >= 0.0) {
    LOG_ERROR(
        "The second derivative is mathematically guaranteed to be negative but "
        "is {}.",
        derivatives.second);
    derivatives.second = -1.0e-6;
  }

  return derivatives;
}

double LossBasedBweV2::GetFeasibleInherentLoss(
    const ChannelParameters& channel_parameters) const {
  return std::min(
      std::max(channel_parameters.inherent_loss,
               config_.inherent_loss_lower_bound),
      GetInherentLossUpperBound(channel_parameters.loss_limited_bandwidth));
}

double LossBasedBweV2::GetInherentLossUpperBound(DataRate bandwidth) const {
  if (bandwidth.IsZero()) {
    return 1.0;
  }

  double inherent_loss_upper_bound =
      config_.inherent_loss_upper_bound_offset +
      config_.inherent_loss_upper_bound_bandwidth_balance / bandwidth;
  return std::min(inherent_loss_upper_bound, 1.0);
}

double LossBasedBweV2::AdjustBiasFactor(double loss_rate,
                                        double bias_factor) const {
  return bias_factor *
         (config_.loss_threshold_of_high_bandwidth_preference - loss_rate) /
         (config_.bandwidth_preference_smoothing_factor +
          std::abs(config_.loss_threshold_of_high_bandwidth_preference -
                   loss_rate));
}

double LossBasedBweV2::GetHighBandwidthBias(DataRate bandwidth) const {
  if (IsValid(bandwidth)) {
    return AdjustBiasFactor(average_reported_loss_ratio_,
                            config_.higher_bandwidth_bias_factor) *
               bandwidth.kbps() +
           AdjustBiasFactor(average_reported_loss_ratio_,
                            config_.higher_log_bandwidth_bias_factor) *
               std::log(1.0 + bandwidth.kbps());
  }
  return 0.0;
}

double LossBasedBweV2::GetObjective(
    const ChannelParameters& channel_parameters) const {
  double objective = 0.0;

  const double high_bandwidth_bias =
      GetHighBandwidthBias(channel_parameters.loss_limited_bandwidth);

  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized()) {
      continue;
    }

    double loss_probability = GetLossProbability(
        channel_parameters.inherent_loss,
        channel_parameters.loss_limited_bandwidth, observation.sending_rate);

    double temporal_weight =
        temporal_weights_[(num_observations_ - 1) - observation.id];
    if (config_.use_byte_loss_rate) {
      objective +=
          temporal_weight *
          ((ToKiloBytes(observation.lost_size) * std::log(loss_probability)) +
           (ToKiloBytes(observation.size - observation.lost_size) *
            std::log(1.0 - loss_probability)));
      objective +=
          temporal_weight * high_bandwidth_bias * ToKiloBytes(observation.size);
    } else {
      objective +=
          temporal_weight *
          ((observation.num_lost_packets * std::log(loss_probability)) +
           (observation.num_received_packets *
            std::log(1.0 - loss_probability)));
      objective +=
          temporal_weight * high_bandwidth_bias * observation.num_packets;
    }
  }

  return objective;
}

DataRate LossBasedBweV2::GetSendingRate(
    DataRate instantaneous_sending_rate) const {
  if (num_observations_ <= 0) {
    return instantaneous_sending_rate;
  }

  const int most_recent_observation_idx =
      (num_observations_ - 1) % config_.observation_window_size;
  const Observation& most_recent_observation =
      observations_[most_recent_observation_idx];
  DataRate sending_rate_previous_observation =
      most_recent_observation.sending_rate;

  return config_.sending_rate_smoothing_factor *
             sending_rate_previous_observation +
         (1.0 - config_.sending_rate_smoothing_factor) *
             instantaneous_sending_rate;
}

DataRate LossBasedBweV2::GetInstantUpperBound() const {
  return cached_instant_upper_bound_.value_or(max_bitrate_);
}

void LossBasedBweV2::CalculateInstantUpperBound() {
  DataRate instant_limit = max_bitrate_;
  if (average_reported_loss_ratio_ > config_.instant_upper_bound_loss_offset) {
    instant_limit = config_.instant_upper_bound_bandwidth_balance /
                    (average_reported_loss_ratio_ -
                     config_.instant_upper_bound_loss_offset);
  }

  cached_instant_upper_bound_ = instant_limit;
}

DataRate LossBasedBweV2::GetInstantLowerBound() const {
  return cached_instant_lower_bound_.value_or(DataRate::Zero());
}

void LossBasedBweV2::CalculateInstantLowerBound() {
  DataRate instance_lower_bound = DataRate::Zero();
  if (IsValid(acknowledged_bitrate_) &&
      config_.lower_bound_by_acked_rate_factor > 0.0) {
    instance_lower_bound = config_.lower_bound_by_acked_rate_factor *
                           acknowledged_bitrate_.value();
  }

  if (IsValid(min_bitrate_)) {
    instance_lower_bound = std::max(instance_lower_bound, min_bitrate_);
  }
  cached_instant_lower_bound_ = instance_lower_bound;
}

void LossBasedBweV2::CalculateTemporalWeights() {
  for (int i = 0; i < config_.observation_window_size; ++i) {
    temporal_weights_[i] = std::pow(config_.temporal_weight_factor, i);
    instant_upper_bound_temporal_weights_[i] =
        std::pow(config_.instant_upper_bound_temporal_weight_factor, i);
  }
}

void LossBasedBweV2::NewtonsMethodUpdate(
    ChannelParameters& channel_parameters) const {
  if (num_observations_ <= 0) {
    return;
  }

  for (int i = 0; i < config_.newton_iterations; ++i) {
    const Derivatives derivatives = GetDerivatives(channel_parameters);
    channel_parameters.inherent_loss -=
        config_.newton_step_size * derivatives.first / derivatives.second;
    channel_parameters.inherent_loss =
        GetFeasibleInherentLoss(channel_parameters);
  }
}

bool LossBasedBweV2::PushBackObservation(
    const std::vector<PacketResult>& packet_results) {
  if (packet_results.empty()) {
    return false;
  }

  Timestamp last_send_time = Timestamp::MinusInfinity();
  Timestamp first_send_time = Timestamp::PlusInfinity();
  for (const PacketResult& packet : packet_results) {
    if (packet.IsReceived()) {
      partial_observation_.lost_packets.erase(
          packet.sent_packet.sequence_number);
    } else {
      partial_observation_.lost_packets.emplace(
          packet.sent_packet.sequence_number, packet.sent_packet.size);
    }
    // A late receipt corrects loss above; its bytes were already counted.
    if (packet.previously_reported_lost) continue;
    ++partial_observation_.num_packets;
    partial_observation_.size += packet.sent_packet.size;
    last_send_time = std::max(last_send_time, packet.sent_packet.send_time);
    first_send_time = std::min(first_send_time, packet.sent_packet.send_time);
  }

  // This is the first packet report we have received.
  if (!IsValid(last_send_time_most_recent_observation_)) {
    last_send_time_most_recent_observation_ = first_send_time;
  }

  const TimeDelta observation_duration =
      last_send_time - last_send_time_most_recent_observation_;
  // Too small to be meaningful.
  // To consider: what if it is too long?, i.e. we did not receive any packets
  // for a long time, then all the packets we received are too old.
  if (observation_duration <= TimeDelta::Zero() ||
      observation_duration < config_.observation_duration_lower_bound) {
    return false;
  }

  last_send_time_most_recent_observation_ = last_send_time;

  Observation observation;
  observation.num_packets = partial_observation_.num_packets;
  observation.num_lost_packets = partial_observation_.lost_packets.size();
  observation.num_received_packets =
      observation.num_packets - observation.num_lost_packets;
  observation.sending_rate =
      GetSendingRate(partial_observation_.size / observation_duration);
  for (auto const& [key, packet_size] : partial_observation_.lost_packets) {
    observation.lost_size += packet_size;
  }
  observation.size = partial_observation_.size;
  observation.id = num_observations_++;
  observations_[observation.id % config_.observation_window_size] = observation;

  partial_observation_ = PartialObservation();
  UpdateAverageReportedLossRatio();
  CalculateInstantUpperBound();
  return true;
}

bool LossBasedBweV2::IsInLossLimitedState() const {
  return loss_based_result_.state != LossBasedState::kDelayBasedEstimate;
}

bool LossBasedBweV2::CanKeepIncreasingState(DataRate estimate) const {
  if (config_.padding_duration == TimeDelta::Zero() ||
      loss_based_result_.state != LossBasedState::kIncreaseUsingPadding)
    return true;

  // Keep using the kIncreaseUsingPadding if either the state has been
  // kIncreaseUsingPadding for less than kPaddingDuration or the estimate
  // increases.
  return last_padding_info_.padding_timestamp + config_.padding_duration >=
             last_send_time_most_recent_observation_ ||
         last_padding_info_.padding_rate < estimate;
}

DataRate LossBasedBweV2::GetMedianSendingRate() const {
  std::vector<DataRate> sending_rates;
  for (const Observation& observation : observations_) {
    if (!observation.IsInitialized() || !IsValid(observation.sending_rate) ||
        observation.sending_rate.IsZero()) {
      continue;
    }
    sending_rates.push_back(observation.sending_rate);
  }
  if (sending_rates.empty()) {
    return DataRate::Zero();
  }
  std::sort(sending_rates.begin(), sending_rates.end());
  if (sending_rates.size() % 2 == 0) {
    return (sending_rates[sending_rates.size() / 2 - 1] +
            sending_rates[sending_rates.size() / 2]) /
           2;
  }
  return sending_rates[sending_rates.size() / 2];
}

}  // namespace webrtc
}  // namespace minirtc
