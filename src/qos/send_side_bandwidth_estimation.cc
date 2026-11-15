/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "send_side_bandwidth_estimation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "api/transport/bandwidth_usage.h"
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "bwe_defines.h"
#include "log.h"

namespace minirtc {
namespace webrtc {
namespace {
constexpr TimeDelta kBweIncreaseInterval = TimeDelta::Millis(1000);
constexpr TimeDelta kBweDecreaseInterval = TimeDelta::Millis(300);
constexpr TimeDelta kStartPhase = TimeDelta::Millis(2000);
constexpr int kLimitNumPackets = 20;
constexpr DataRate kDefaultMaxBitrate = DataRate::BitsPerSec(1000000000);
constexpr TimeDelta kLowBitrateLogPeriod = TimeDelta::Millis(10000);
constexpr TimeDelta kRtcEventLogPeriod = TimeDelta::Millis(5000);
// Expecting that RTCP feedback is sent uniformly within [0.5, 1.5]s intervals.
constexpr TimeDelta kMaxRtcpFeedbackInterval = TimeDelta::Millis(5000);

constexpr float kDefaultLowLossThreshold = 0.02f;
constexpr float kDefaultHighLossThreshold = 0.1f;
constexpr DataRate kDefaultBitrateThreshold = DataRate::Zero();

}  // namespace

void LinkCapacityTracker::UpdateDelayBasedEstimate(
    Timestamp at_time, DataRate delay_based_bitrate) {
  if (delay_based_bitrate < last_delay_based_estimate_) {
    capacity_estimate_bps_ =
        std::min(capacity_estimate_bps_, delay_based_bitrate.bps<double>());
    last_link_capacity_update_ = at_time;
  }
  last_delay_based_estimate_ = delay_based_bitrate;
}

void LinkCapacityTracker::OnStartingRate(DataRate start_rate) {
  if (last_link_capacity_update_.IsInfinite())
    capacity_estimate_bps_ = start_rate.bps<double>();
}

void LinkCapacityTracker::OnRateUpdate(std::optional<DataRate> acknowledged,
                                       DataRate target, Timestamp at_time) {
  if (!acknowledged) return;
  DataRate acknowledged_target = std::min(*acknowledged, target);
  if (acknowledged_target.bps() > capacity_estimate_bps_) {
    TimeDelta delta = at_time - last_link_capacity_update_;
    double alpha =
        delta.IsFinite() ? exp(-(delta / TimeDelta::Seconds(10))) : 0;
    capacity_estimate_bps_ = alpha * capacity_estimate_bps_ +
                             (1 - alpha) * acknowledged_target.bps<double>();
  }
  last_link_capacity_update_ = at_time;
}

void LinkCapacityTracker::OnRttBackoff(DataRate backoff_rate,
                                       Timestamp at_time) {
  capacity_estimate_bps_ =
      std::min(capacity_estimate_bps_, backoff_rate.bps<double>());
  last_link_capacity_update_ = at_time;
}

DataRate LinkCapacityTracker::estimate() const {
  return DataRate::BitsPerSec(capacity_estimate_bps_);
}

RttBasedBackoff::RttBasedBackoff()
    : disabled_(false),
      configured_limit_(TimeDelta::Seconds(3)),
      drop_fraction_(0.8),
      drop_interval_(TimeDelta::Seconds(1)),
      bandwidth_floor_(DataRate::KilobitsPerSec(5)),
      rtt_limit_(TimeDelta::PlusInfinity()),
      // By initializing this to plus infinity, we make sure that we never
      // trigger rtt backoff unless packet feedback is enabled.
      last_propagation_rtt_update_(Timestamp::PlusInfinity()),
      last_propagation_rtt_(TimeDelta::Zero()),
      last_packet_sent_(Timestamp::MinusInfinity()) {
  if (!disabled_) {
    rtt_limit_ = configured_limit_;
  }
}

void RttBasedBackoff::UpdatePropagationRtt(Timestamp at_time,
                                           TimeDelta propagation_rtt) {
  if (!at_time.IsFinite() || !propagation_rtt.IsFinite() ||
      propagation_rtt < TimeDelta::Zero() ||
      (last_propagation_rtt_update_.IsFinite() &&
       at_time < last_propagation_rtt_update_))
    return;
  last_propagation_rtt_update_ = at_time;
  last_propagation_rtt_ = propagation_rtt;
}

bool RttBasedBackoff::IsRttAboveLimit() const {
  return !disabled_ && CorrectedRtt() > rtt_limit_;
}

TimeDelta RttBasedBackoff::CorrectedRtt() const {
  if (!last_packet_sent_.IsFinite() ||
      !last_propagation_rtt_update_.IsFinite())
    return TimeDelta::Zero();
  // Avoid timeout when no packets are being sent.
  TimeDelta timeout_correction = std::max(
      last_packet_sent_ - last_propagation_rtt_update_, TimeDelta::Zero());
  return timeout_correction + last_propagation_rtt_;
}

RttBasedBackoff::~RttBasedBackoff() = default;

SendSideBandwidthEstimation::SendSideBandwidthEstimation()
    : lost_packets_since_last_loss_update_(0),
      expected_packets_since_last_loss_update_(0),
      current_target_(DataRate::Zero()),
      last_logged_target_(DataRate::Zero()),
      min_bitrate_configured_(kCongestionControllerMinBitrate),
      max_bitrate_configured_(kDefaultMaxBitrate),
      last_low_bitrate_log_(Timestamp::MinusInfinity()),
      has_decreased_since_last_fraction_loss_(false),
      last_loss_feedback_(Timestamp::MinusInfinity()),
      last_loss_packet_report_(Timestamp::MinusInfinity()),
      last_fraction_loss_(0),
      last_logged_fraction_loss_(0),
      last_round_trip_time_(TimeDelta::Zero()),
      receiver_limit_(DataRate::PlusInfinity()),
      delay_based_limit_(DataRate::PlusInfinity()),
      time_last_decrease_(Timestamp::MinusInfinity()),
      first_report_time_(Timestamp::MinusInfinity()),
      low_loss_threshold_(kDefaultLowLossThreshold),
      high_loss_threshold_(kDefaultHighLossThreshold),
      bitrate_threshold_(kDefaultBitrateThreshold),
      loss_based_state_(LossBasedState::kDelayBasedEstimate),
      disable_receiver_limit_caps_only_(false) {
}

SendSideBandwidthEstimation::~SendSideBandwidthEstimation() {}

void SendSideBandwidthEstimation::OnRouteChange(double loss_recovery_max_increase) {
  loss_based_bwe_ = std::make_unique<LossBasedBweV2>(loss_recovery_max_increase);
  loss_based_state_ = LossBasedState::kDelayBasedEstimate;
  acknowledged_rate_.reset();
  min_bitrate_history_.clear();
  link_capacity_ = LinkCapacityTracker();
  rtt_backoff_ = RttBasedBackoff();
  lost_packets_since_last_loss_update_ = 0;
  expected_packets_since_last_loss_update_ = 0;
  current_target_ = DataRate::Zero();
  min_bitrate_configured_ = kCongestionControllerMinBitrate;
  max_bitrate_configured_ = kDefaultMaxBitrate;
  last_low_bitrate_log_ = Timestamp::MinusInfinity();
  has_decreased_since_last_fraction_loss_ = false;
  last_loss_feedback_ = Timestamp::MinusInfinity();
  last_loss_packet_report_ = Timestamp::MinusInfinity();
  last_fraction_loss_ = 0;
  last_logged_fraction_loss_ = 0;
  last_round_trip_time_ = TimeDelta::Zero();
  receiver_limit_ = DataRate::PlusInfinity();
  delay_based_limit_ = DataRate::PlusInfinity();
  time_last_decrease_ = Timestamp::MinusInfinity();
  first_report_time_ = Timestamp::MinusInfinity();
}

void SendSideBandwidthEstimation::SetBitrates(
    std::optional<DataRate> send_bitrate, DataRate min_bitrate,
    DataRate max_bitrate, Timestamp at_time) {
  SetMinMaxBitrate(min_bitrate, max_bitrate);
  if (send_bitrate) {
    link_capacity_.OnStartingRate(*send_bitrate);
    SetSendBitrate(*send_bitrate, at_time);
  }
}

void SendSideBandwidthEstimation::SetSendBitrate(DataRate bitrate,
                                                 Timestamp at_time) {
  // Reset to avoid being capped by the estimate.
  delay_based_limit_ = DataRate::PlusInfinity();
  UpdateTargetBitrate(bitrate, at_time);
  // Clear last sent bitrate history so the new value can be used directly
  // and not capped.
  min_bitrate_history_.clear();
}

void SendSideBandwidthEstimation::SetMinMaxBitrate(DataRate min_bitrate,
                                                   DataRate max_bitrate) {
  min_bitrate_configured_ =
      std::max(min_bitrate, kCongestionControllerMinBitrate);
  if (max_bitrate > DataRate::Zero() && max_bitrate.IsFinite()) {
    max_bitrate_configured_ = std::max(min_bitrate_configured_, max_bitrate);
  } else {
    max_bitrate_configured_ = kDefaultMaxBitrate;
  }
  loss_based_bwe_->SetMinMaxBitrate(min_bitrate_configured_,
                                   max_bitrate_configured_);
}

int SendSideBandwidthEstimation::GetMinBitrate() const {
  return min_bitrate_configured_.bps<int>();
}

DataRate SendSideBandwidthEstimation::target_rate() const {
  DataRate target = current_target_;
  if (!disable_receiver_limit_caps_only_)
    target = std::min(target, receiver_limit_);
  return std::max(min_bitrate_configured_, target);
}

LossBasedState SendSideBandwidthEstimation::loss_based_state() const {
  return loss_based_state_;
}

bool SendSideBandwidthEstimation::IsRttAboveLimit() const {
  return rtt_backoff_.IsRttAboveLimit();
}

DataRate SendSideBandwidthEstimation::GetEstimatedLinkCapacity() const {
  return link_capacity_.estimate();
}

void SendSideBandwidthEstimation::UpdateReceiverEstimate(Timestamp at_time,
                                                         DataRate bandwidth) {
  // TODO(srte): Ensure caller passes PlusInfinity, not zero, to represent no
  // limitation.
  receiver_limit_ = bandwidth.IsZero() ? DataRate::PlusInfinity() : bandwidth;
  ApplyTargetLimits(at_time);
}

void SendSideBandwidthEstimation::UpdateDelayBasedEstimate(Timestamp at_time,
                                                           DataRate bitrate) {
  link_capacity_.UpdateDelayBasedEstimate(at_time, bitrate);
  // TODO(srte): Ensure caller passes PlusInfinity, not zero, to represent no
  // limitation.
  delay_based_limit_ = bitrate.IsZero() ? DataRate::PlusInfinity() : bitrate;
  ApplyTargetLimits(at_time);
}

void SendSideBandwidthEstimation::SetAcknowledgedRate(
    std::optional<DataRate> acknowledged_rate, Timestamp at_time) {
  acknowledged_rate_ = acknowledged_rate;
  if (acknowledged_rate)
    loss_based_bwe_->SetAcknowledgedBitrate(*acknowledged_rate);
}

void SendSideBandwidthEstimation::UpdateLossBasedEstimator(
    const TransportPacketsFeedback& report,
    BandwidthUsage /*delay_detector_state*/,
    std::optional<DataRate> /*probe_bitrate*/, bool in_alr) {
  loss_based_bwe_->UpdateBandwidthEstimate(report.packet_feedbacks,
                                           delay_based_limit_, in_alr);
  UpdateEstimate(report.feedback_time);
}

void SendSideBandwidthEstimation::UpdatePacketsLost(int64_t packets_lost,
                                                    int64_t number_of_packets,
                                                    Timestamp at_time) {
  last_loss_feedback_ = at_time;
  if (first_report_time_.IsInfinite()) first_report_time_ = at_time;

  // Check sequence number diff and weight loss report
  if (number_of_packets > 0) {
    int64_t expected =
        expected_packets_since_last_loss_update_ + number_of_packets;

    // Don't generate a loss rate until it can be based on enough packets.
    if (expected < kLimitNumPackets) {
      // Accumulate reports.
      expected_packets_since_last_loss_update_ = expected;
      lost_packets_since_last_loss_update_ += packets_lost;
      return;
    }

    has_decreased_since_last_fraction_loss_ = false;
    int64_t lost_q8 =
        std::max<int64_t>(lost_packets_since_last_loss_update_ + packets_lost,
                          0)
        << 8;
    last_fraction_loss_ = std::min<int>(lost_q8 / expected, 255);

    // Reset accumulators.
    lost_packets_since_last_loss_update_ = 0;
    expected_packets_since_last_loss_update_ = 0;
    last_loss_packet_report_ = at_time;
    UpdateEstimate(at_time);
  }
}

void SendSideBandwidthEstimation::UpdateRtt(TimeDelta rtt, Timestamp at_time) {
  // Update RTT if we were able to compute an RTT based on this RTCP.
  // FlexFEC doesn't send RTCP SR, which means we won't be able to compute RTT.
  if (rtt > TimeDelta::Zero()) last_round_trip_time_ = rtt;
}

void SendSideBandwidthEstimation::UpdateEstimate(Timestamp at_time) {
  if (rtt_backoff_.IsRttAboveLimit()) {
    // Process ticks alone must not keep reducing an idle sender. Outstanding
    // data remains guarded by CWND/queue limits; another send resumes backoff.
    if (at_time - time_last_decrease_ >= rtt_backoff_.drop_interval_ &&
        rtt_backoff_.last_packet_sent_ > time_last_decrease_ &&
        current_target_ > rtt_backoff_.bandwidth_floor_) {
      time_last_decrease_ = at_time;
      DataRate new_bitrate =
          std::max(current_target_ * rtt_backoff_.drop_fraction_,
                   rtt_backoff_.bandwidth_floor_);
      link_capacity_.OnRttBackoff(new_bitrate, at_time);
      UpdateTargetBitrate(new_bitrate, at_time);
      return;
    }
    // TODO(srte): This is likely redundant in most cases.
    ApplyTargetLimits(at_time);
    return;
  }
  // We trust the REMB and/or delay-based estimate during the first 2 seconds if
  // we haven't had any packet loss reported, to allow startup bitrate probing.
  if (last_fraction_loss_ == 0 && IsInStartPhase(at_time) &&
      !loss_based_bwe_->ReadyToUseInStartPhase()) {
    DataRate new_bitrate = current_target_;
    // TODO(srte): We should not allow the new_bitrate to be larger than the
    // receiver limit here.
    if (receiver_limit_.IsFinite())
      new_bitrate = std::max(receiver_limit_, new_bitrate);
    if (delay_based_limit_.IsFinite())
      new_bitrate = std::max(delay_based_limit_, new_bitrate);

    if (new_bitrate != current_target_) {
      min_bitrate_history_.clear();
      min_bitrate_history_.push_back(std::make_pair(at_time, current_target_));
      UpdateTargetBitrate(new_bitrate, at_time);
      return;
    }
  }
  UpdateMinHistory(at_time);
  if (loss_based_bwe_->IsReady()) {
    const auto result = loss_based_bwe_->GetLossBasedResult();
    loss_based_state_ = result.state;
    UpdateTargetBitrate(result.bandwidth_estimate, at_time);
    return;
  }
  if (last_loss_packet_report_.IsInfinite()) {
    // No feedback received.
    // TODO(srte): This is likely redundant in most cases.
    ApplyTargetLimits(at_time);
    return;
  }

  TimeDelta time_since_loss_packet_report = at_time - last_loss_packet_report_;
  if (time_since_loss_packet_report < 1.2 * kMaxRtcpFeedbackInterval) {
    // We only care about loss above a given bitrate threshold.
    float loss = last_fraction_loss_ / 256.0f;
    // We only make decisions based on loss when the bitrate is above a
    // threshold. This is a crude way of handling loss which is uncorrelated
    // to congestion.
    // LOG_WARN("loss: [{}]", loss);
    if (current_target_ < bitrate_threshold_ || loss <= low_loss_threshold_) {
      // Loss < 2%: Increase rate by 8% of the min bitrate in the last
      // kBweIncreaseInterval.
      // Note that by remembering the bitrate over the last second one can
      // rampup up one second faster than if only allowed to start ramping
      // at 8% per second rate now. E.g.:
      //   If sending a constant 100kbps it can rampup immediately to 108kbps
      //   whenever a receiver report is received with lower packet loss.
      //   If instead one would do: current_bitrate_ *= 1.08^(delta time),
      //   it would take over one second since the lower packet loss to achieve
      //   108kbps.
      DataRate new_bitrate = DataRate::BitsPerSec(
          min_bitrate_history_.front().second.bps() * 1.08 + 0.5);

      // Add 1 kbps extra, just to make sure that we do not get stuck
      // (gives a little extra increase at low rates, negligible at higher
      // rates).
      new_bitrate += DataRate::BitsPerSec(1000);
      // LOG_WARN("new_bitrate: [{}]", ToString(new_bitrate).c_str());
      UpdateTargetBitrate(new_bitrate, at_time);
      return;
    } else if (current_target_ > bitrate_threshold_) {
      if (loss <= high_loss_threshold_) {
        // Loss between 2% - 10%: Do nothing.
      } else {
        // Loss > 10%: Limit the rate decreases to once a kBweDecreaseInterval
        // + rtt.
        if (!has_decreased_since_last_fraction_loss_ &&
            (at_time - time_last_decrease_) >=
                (kBweDecreaseInterval + last_round_trip_time_)) {
          time_last_decrease_ = at_time;

          // Reduce rate:
          //   newRate = rate * (1 - 0.5*lossRate);
          //   where packetLoss = 256*lossRate;
          DataRate new_bitrate = DataRate::BitsPerSec(
              (current_target_.bps() *
               static_cast<double>(512 - last_fraction_loss_)) /
              512.0);
          has_decreased_since_last_fraction_loss_ = true;
          // LOG_WARN("new_bitrate: [{}]", ToString(new_bitrate).c_str());
          UpdateTargetBitrate(new_bitrate, at_time);
          return;
        }
      }
    }
  }
  // TODO(srte): This is likely redundant in most cases.
  ApplyTargetLimits(at_time);
}

void SendSideBandwidthEstimation::UpdatePropagationRtt(
    Timestamp at_time, TimeDelta propagation_rtt) {
  rtt_backoff_.UpdatePropagationRtt(at_time, propagation_rtt);
}

void SendSideBandwidthEstimation::OnSentPacket(const SentPacket& sent_packet) {
  // Only feedback-triggering packets will be reported here.
  if (!sent_packet.send_time.IsFinite() ||
      sent_packet.send_time < rtt_backoff_.last_packet_sent_)
    return;
  // Begin the feedback clock with the first packet, including when no report
  // ever arrives. An empty previous flight also starts a fresh waiting period:
  // idle desktop time must not look like a feedback blackout after resuming.
  // The adapter includes this packet in data_in_flight. Zero means unknown for
  // callers without in-flight accounting, not proof that the path has drained.
  const bool previous_flight_empty =
      sent_packet.size > DataSize::Zero() &&
      sent_packet.data_in_flight > DataSize::Zero() &&
      sent_packet.data_in_flight <= sent_packet.size;
  if (rtt_backoff_.last_propagation_rtt_update_.IsInfinite() ||
      previous_flight_empty) {
    rtt_backoff_.UpdatePropagationRtt(sent_packet.send_time,
                                    rtt_backoff_.last_propagation_rtt_);
  }
  rtt_backoff_.last_packet_sent_ = sent_packet.send_time;
}

bool SendSideBandwidthEstimation::IsInStartPhase(Timestamp at_time) const {
  return first_report_time_.IsInfinite() ||
         at_time - first_report_time_ < kStartPhase;
}

void SendSideBandwidthEstimation::UpdateMinHistory(Timestamp at_time) {
  // Remove old data points from history.
  // Since history precision is in ms, add one so it is able to increase
  // bitrate if it is off by as little as 0.5ms.
  while (!min_bitrate_history_.empty() &&
         at_time - min_bitrate_history_.front().first + TimeDelta::Millis(1) >
             kBweIncreaseInterval) {
    min_bitrate_history_.pop_front();
  }

  // Typical minimum sliding-window algorithm: Pop values higher than current
  // bitrate before pushing it.
  while (!min_bitrate_history_.empty() &&
         current_target_ <= min_bitrate_history_.back().second) {
    min_bitrate_history_.pop_back();
  }

  min_bitrate_history_.push_back(std::make_pair(at_time, current_target_));
}

DataRate SendSideBandwidthEstimation::GetUpperLimit() const {
  DataRate upper_limit = delay_based_limit_;
  if (disable_receiver_limit_caps_only_) {
    upper_limit = std::min(upper_limit, receiver_limit_);
  }
  return std::min(upper_limit, max_bitrate_configured_);
}

void SendSideBandwidthEstimation::MaybeLogLowBitrateWarning(DataRate bitrate,
                                                            Timestamp at_time) {
  if (at_time - last_low_bitrate_log_ > kLowBitrateLogPeriod) {
    LOG_WARN(
        "Estimated available bandwidth {} is below configured min bitrate {}",
        ToString(bitrate), ToString(min_bitrate_configured_));
    last_low_bitrate_log_ = at_time;
  }
}

void SendSideBandwidthEstimation::UpdateTargetBitrate(DataRate new_bitrate,
                                                      Timestamp at_time) {
  new_bitrate = std::min(new_bitrate, GetUpperLimit());
  if (new_bitrate < min_bitrate_configured_) {
    MaybeLogLowBitrateWarning(new_bitrate, at_time);
    new_bitrate = min_bitrate_configured_;
  }
  current_target_ = new_bitrate;
  link_capacity_.OnRateUpdate(acknowledged_rate_, current_target_, at_time);
}

void SendSideBandwidthEstimation::ApplyTargetLimits(Timestamp at_time) {
  UpdateTargetBitrate(current_target_, at_time);
}

}  // namespace webrtc
}  // namespace minirtc
