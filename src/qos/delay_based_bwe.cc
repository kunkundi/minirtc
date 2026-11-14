/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "delay_based_bwe.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "bwe_defines.h"
#include "inter_arrival_delta.h"
#include "log.h"
#include "trendline_estimator.h"

namespace minirtc {
namespace webrtc {
namespace {
constexpr TimeDelta kStreamTimeOut = TimeDelta::Seconds(2);
constexpr TimeDelta kSendTimeGroupLength = TimeDelta::Millis(5);

// This ssrc is used to fulfill the current API but will be removed
// after the API has been changed.
constexpr uint32_t kFixedSsrc = 0;

}  // namespace

constexpr char BweSeparateAudioPacketsSettings::kKey[];

DelayBasedBwe::Result::Result()
    : updated(false),
      probe(false),
      target_bitrate(DataRate::Zero()),
      recovered_from_overuse(false),
      delay_detector_state(BandwidthUsage::kBwNormal) {}

DelayBasedBwe::DelayBasedBwe()
    : audio_packets_since_last_video_(0),
      last_video_packet_recv_time_(Timestamp::MinusInfinity()),
      last_seen_packet_(Timestamp::MinusInfinity()),
      video_delay_detector_(new TrendlineEstimator()),
      audio_delay_detector_(new TrendlineEstimator()),
      active_delay_detector_(video_delay_detector_.get()),
      uma_recorded_(false),
      rate_control_(true),
      prev_bitrate_(DataRate::Zero()),
      prev_state_(BandwidthUsage::kBwNormal) {}

DelayBasedBwe::~DelayBasedBwe() {}

DelayBasedBwe::Result DelayBasedBwe::IncomingPacketFeedbackVector(
    const TransportPacketsFeedback& msg, std::optional<DataRate> acked_bitrate,
    std::optional<DataRate> probe_bitrate, bool in_alr) {
  auto packet_feedback_vector = msg.SortedByReceiveTime();
  // TODO(holmer): An empty feedback vector here likely means that
  // all acks were too late and that the send time history had
  // timed out. We should reduce the rate when this occurs.
  if (packet_feedback_vector.empty()) {
    LOG_WARN("Very late feedback received");
    return DelayBasedBwe::Result();
  }

  if (!uma_recorded_) {
    uma_recorded_ = true;
  }
  bool delayed_feedback = true;
  bool recovered_from_overuse = false;
  BandwidthUsage prev_detector_state = active_delay_detector_->State();
  for (const auto& packet_feedback : packet_feedback_vector) {
    // Probe bursts measure capacity separately. Mixing their spacing with
    // sparse desktop frames can turn the end of a burst into a delay trend.
    if (packet_feedback.sent_packet.pacing_info.probe_cluster_id !=
        PacedPacketInfo::kNotAProbe)
      continue;
    delayed_feedback = false;
    IncomingPacketFeedback(packet_feedback, msg.feedback_time);
    if (prev_detector_state == BandwidthUsage::kBwUnderusing &&
        active_delay_detector_->State() == BandwidthUsage::kBwNormal) {
      recovered_from_overuse = true;
    }
    prev_detector_state = active_delay_detector_->State();
  }

  if (delayed_feedback) {
    return probe_bitrate ? ApplyProbeResult(*probe_bitrate, msg.feedback_time,
                                            acked_bitrate, in_alr)
                         : Result();
  }
  rate_control_.SetInApplicationLimitedRegion(in_alr);
  return MaybeUpdateEstimate(acked_bitrate, probe_bitrate,
                             recovered_from_overuse, in_alr, msg.feedback_time);
}

DelayBasedBwe::Result DelayBasedBwe::ApplyProbeResult(
    DataRate bitrate, Timestamp at_time, std::optional<DataRate> acked_bitrate,
    bool in_alr) {
  if (active_delay_detector_->State() == BandwidthUsage::kBwOverusing) {
    return Result();
  }
  rate_control_.SetInApplicationLimitedRegion(in_alr);
  return MaybeUpdateEstimate(acked_bitrate, bitrate, false, in_alr, at_time);
}

void DelayBasedBwe::IncomingPacketFeedback(const PacketResult& packet_feedback,
                                           Timestamp at_time) {
  // Reset if the stream has timed out.
  if (last_seen_packet_.IsInfinite() ||
      at_time - last_seen_packet_ > kStreamTimeOut) {
    video_inter_arrival_delta_ =
        std::make_unique<InterArrivalDelta>(kSendTimeGroupLength);
    audio_inter_arrival_delta_ =
        std::make_unique<InterArrivalDelta>(kSendTimeGroupLength);

    video_delay_detector_.reset(new TrendlineEstimator());
    audio_delay_detector_.reset(new TrendlineEstimator());
    active_delay_detector_ = video_delay_detector_.get();
    overuse_since_ = Timestamp::MinusInfinity();
  }
  last_seen_packet_ = at_time;

  // As an alternative to ignoring small packets, we can separate audio and
  // video packets for overuse detection.
  DelayIncreaseDetectorInterface* delay_detector_for_packet =
      video_delay_detector_.get();
  if (separate_audio_.enabled) {
    if (packet_feedback.sent_packet.audio) {
      delay_detector_for_packet = audio_delay_detector_.get();
      audio_packets_since_last_video_++;
      if (audio_packets_since_last_video_ > separate_audio_.packet_threshold &&
          packet_feedback.receive_time - last_video_packet_recv_time_ >
              separate_audio_.time_threshold) {
        active_delay_detector_ = audio_delay_detector_.get();
      }
    } else {
      audio_packets_since_last_video_ = 0;
      last_video_packet_recv_time_ =
          std::max(last_video_packet_recv_time_, packet_feedback.receive_time);
      active_delay_detector_ = video_delay_detector_.get();
    }
  }
  DataSize packet_size = packet_feedback.sent_packet.size;

  TimeDelta send_delta = TimeDelta::Zero();
  TimeDelta recv_delta = TimeDelta::Zero();
  int size_delta = 0;

  InterArrivalDelta* inter_arrival_for_packet =
      (separate_audio_.enabled && packet_feedback.sent_packet.audio)
          ? audio_inter_arrival_delta_.get()
          : video_inter_arrival_delta_.get();
  bool calculated_deltas = inter_arrival_for_packet->ComputeDeltas(
      packet_feedback.sent_packet.send_time, packet_feedback.receive_time,
      at_time, packet_size.bytes(), &send_delta, &recv_delta, &size_delta);

  delay_detector_for_packet->Update(recv_delta.ms<double>(),
                                    send_delta.ms<double>(),
                                    packet_feedback.sent_packet.send_time.ms(),
                                    packet_feedback.receive_time.ms(),
                                    packet_size.bytes(), calculated_deltas);
}

DataRate DelayBasedBwe::TriggerOveruse(Timestamp at_time,
                                       std::optional<DataRate> link_capacity) {
  RateControlInput input(BandwidthUsage::kBwOverusing, link_capacity);
  return rate_control_.Update(input, at_time);
}

DelayBasedBwe::Result DelayBasedBwe::MaybeUpdateEstimate(
    std::optional<DataRate> acked_bitrate,
    std::optional<DataRate> probe_bitrate, bool recovered_from_overuse,
    bool in_alr, Timestamp at_time) {
  Result result;

  // Currently overusing the bandwidth.
  if (active_delay_detector_->State() == BandwidthUsage::kBwOverusing) {
    if (!overuse_since_.IsFinite()) overuse_since_ = at_time;
    const DataRate reference =
        published_rate_.IsZero()
            ? rate_control_.LatestEstimate()
            : std::min(published_rate_, rate_control_.LatestEstimate());
    while (!normal_sends_.empty() &&
           at_time - normal_sends_.front().first >= TimeDelta::Seconds(1)) {
      normal_sent_bytes_ -= normal_sends_.front().second;
      normal_sends_.pop_front();
    }
    const auto send_window =
        std::min(TimeDelta::Seconds(1), at_time - first_normal_send_);
    const DataRate sent_rate = send_window > TimeDelta::Zero()
                                   ? normal_sent_bytes_ / send_window
                                   : DataRate::Zero();
    // A congested link also has a low ACK rate. Use offered media load, not
    // delivered throughput, to distinguish that case from a quiet desktop.
    const bool low_load = in_alr || (first_normal_send_.IsFinite() &&
                                     send_window >= TimeDelta::Millis(500) &&
                                     sent_rate < reference * 0.65);
    const bool strong_congestion =
        network_loss_ >= 0.10 ||
        (network_rtt_.IsFinite() && network_rtt_ >= TimeDelta::Millis(300));
    const bool guarded = low_load && !strong_congestion;
    if (guarded) {
      // A quiet desktop's ACK rate measures demand, not available capacity.
      // Require fresh, sustained overuse, then reduce the published budget
      // gradually. Large RTT/loss and a loaded stream retain fast backoff.
      if (at_time - overuse_since_ >= TimeDelta::Millis(200) &&
          at_time - last_low_load_backoff_ >= TimeDelta::Millis(500)) {
        rate_control_.SetEstimate(reference * 0.90, at_time);
        result.updated = true;
        result.target_bitrate = rate_control_.LatestEstimate();
        last_low_load_backoff_ = at_time;
      }
    } else if (acked_bitrate &&
               rate_control_.TimeToReduceFurther(at_time, *acked_bitrate)) {
      result.updated =
          UpdateEstimate(at_time, acked_bitrate, &result.target_bitrate);
    } else if (!acked_bitrate && rate_control_.ValidEstimate() &&
               rate_control_.InitialTimeToReduceFurther(at_time)) {
      // Overusing before we have a measured acknowledged bitrate. Reduce send
      // rate by 50% every 200 ms.
      // TODO(tschumim): Improve this and/or the acknowledged bitrate estimator
      // so that we (almost) always have a bitrate estimate.
      rate_control_.SetEstimate(rate_control_.LatestEstimate() / 2, at_time);
      result.updated = true;
      result.probe = false;
      result.target_bitrate = rate_control_.LatestEstimate();
    }
    if (result.updated ||
        at_time - last_decision_log_ >= TimeDelta::Seconds(1)) {
      LOG_DEBUG(
          "Delay BWE decision: action={} low_load={} in_alr={} "
          "published_bps={} reference_bps={} sent_bps={} acked_bps={} "
          "target_bps={} rtt_ms={} "
          "loss={} overuse_ms={}",
          result.updated ? (guarded ? "bounded_backoff" : "backoff")
                         : "confirm_overuse",
          low_load, in_alr, published_rate_.bps(), reference.bps(),
          sent_rate.bps(), acked_bitrate ? acked_bitrate->bps() : 0,
          result.updated ? result.target_bitrate.bps() : reference.bps(),
          network_rtt_.IsFinite() ? network_rtt_.ms() : -1, network_loss_,
          (at_time - overuse_since_).ms());
      last_decision_log_ = at_time;
    }
  } else {
    overuse_since_ = Timestamp::MinusInfinity();
    if (probe_bitrate) {
      result.probe = true;
      result.updated = true;
      rate_control_.SetEstimate(*probe_bitrate, at_time);
      result.target_bitrate = rate_control_.LatestEstimate();
    } else {
      result.updated =
          UpdateEstimate(at_time, acked_bitrate, &result.target_bitrate);
      result.recovered_from_overuse = recovered_from_overuse;
    }
  }
  BandwidthUsage detector_state = active_delay_detector_->State();
  if ((result.updated && prev_bitrate_ != result.target_bitrate) ||
      detector_state != prev_state_) {
    DataRate bitrate = result.updated ? result.target_bitrate : prev_bitrate_;

    prev_bitrate_ = bitrate;
    prev_state_ = detector_state;
  }

  result.delay_detector_state = detector_state;

  return result;
}

bool DelayBasedBwe::UpdateEstimate(Timestamp at_time,
                                   std::optional<DataRate> acked_bitrate,
                                   DataRate* target_rate) {
  const RateControlInput input(active_delay_detector_->State(), acked_bitrate);
  *target_rate = rate_control_.Update(input, at_time);
  return rate_control_.ValidEstimate();
}

void DelayBasedBwe::OnRttUpdate(TimeDelta avg_rtt) {
  rate_control_.SetRtt(avg_rtt);
}

void DelayBasedBwe::SetTransportState(DataRate published_rate, TimeDelta rtt,
                                      double loss_ratio) {
  published_rate_ = published_rate;
  network_rtt_ = rtt;
  network_loss_ = loss_ratio;
}

void DelayBasedBwe::OnNormalPacketSent(DataSize size, Timestamp at_time) {
  if (!first_normal_send_.IsFinite()) first_normal_send_ = at_time;
  normal_sends_.emplace_back(at_time, size);
  normal_sent_bytes_ += size;
  while (!normal_sends_.empty() &&
         at_time - normal_sends_.front().first >= TimeDelta::Seconds(1)) {
    normal_sent_bytes_ -= normal_sends_.front().second;
    normal_sends_.pop_front();
  }
}

bool DelayBasedBwe::LatestEstimate(std::vector<uint32_t>* ssrcs,
                                   DataRate* bitrate) const {
  // Currently accessed from both the process thread (see
  // ModuleRtpRtcpImpl::Process()) and the configuration thread (see
  // Call::GetStats()). Should in the future only be accessed from a single
  // thread.
  if (!rate_control_.ValidEstimate()) return false;

  *ssrcs = {kFixedSsrc};
  *bitrate = rate_control_.LatestEstimate();
  return true;
}

void DelayBasedBwe::SetStartBitrate(DataRate start_bitrate) {
  LOG_INFO("BWE Setting start bitrate to: {}", ToString(start_bitrate));
  rate_control_.SetStartBitrate(start_bitrate);
}

void DelayBasedBwe::SetMinBitrate(DataRate min_bitrate) {
  // Called from both the configuration thread and the network thread. Shouldn't
  // be called from the network thread in the future.
  rate_control_.SetMinBitrate(min_bitrate);
}

TimeDelta DelayBasedBwe::GetExpectedBwePeriod() const {
  return rate_control_.GetExpectedBandwidthPeriod();
}

}  // namespace webrtc
}  // namespace minirtc
