/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "probe_bitrate_estimator.h"

#include <algorithm>
#include <memory>
#include <optional>

#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "log.h"

namespace minirtc {
namespace webrtc {
namespace {
// The minumum number of probes we need to receive feedback about in percent
// in order to have a valid estimate.
constexpr double kMinReceivedProbesRatio = .80;

// The minumum number of bytes we need to receive feedback about in percent
// in order to have a valid estimate.
constexpr double kMinReceivedBytesRatio = .80;

// The maximum |receive rate| / |send rate| ratio for a valid estimate.
constexpr float kMaxValidRatio = 2.0f;

// The minimum |receive rate| / |send rate| ratio assuming that the link is
// not saturated, i.e. we assume that we will receive at least
// kMinRatioForUnsaturatedLink * |send rate| if |send rate| is less than the
// link capacity.
constexpr float kMinRatioForUnsaturatedLink = 0.9f;

// The target utilization of the link. If we know true link capacity
// we'd like to send at 95% of that rate.
constexpr float kTargetUtilizationFraction = 0.95f;

// The maximum time period over which the cluster history is retained.
// This is also the maximum time period beyond which a probing burst is not
// expected to last.
constexpr TimeDelta kMaxClusterHistory = TimeDelta::Seconds(1);

// The maximum time interval between first and the last probe on a cluster
// on the sender side as well as the receive side.
constexpr TimeDelta kMaxProbeInterval = TimeDelta::Seconds(1);

}  // namespace

ProbeBitrateEstimator::ProbeBitrateEstimator() {}

ProbeBitrateEstimator::~ProbeBitrateEstimator() = default;

void ProbeBitrateEstimator::OnProbePacketSent(int cluster_id, DataSize size,
                                             Timestamp at_time) {
  if (size <= DataSize::Zero() ||
      (max_expired_cluster_id_ && cluster_id <= *max_expired_cluster_id_))
    return;
  auto& cluster = clusters_[cluster_id];
  if (cluster.send_aborted) return;
  cluster.sent_size += size;
  ++cluster.sent_packets;
  cluster.last_activity_at = std::max(cluster.last_activity_at, at_time);
  // A late local send must never retain an earlier completion claim.
  cluster.send_complete = false;
}

std::optional<double> ProbeBitrateEstimator::OnProbeCompleted(
    int cluster_id, int sent_packets, Timestamp at_time) {
  auto cluster = clusters_.find(cluster_id);
  if (cluster == clusters_.end() ||
      cluster->second.sent_size <= DataSize::Zero() ||
      cluster->second.send_aborted)
    return std::nullopt;
  if (cluster->second.sent_packets != sent_packets) {
    // The pacer counts attempted sends; transport feedback tracks only
    // successful sends. A failed datagram makes this cluster incomplete.
    LOG_DEBUG("Probe completion rejected: id={} sent_packets={} "
              "attempted_packets={}",
              cluster_id, cluster->second.sent_packets, sent_packets);
    OnProbeAborted(cluster_id);
    return std::nullopt;
  }
  cluster->second.send_complete = true;
  cluster->second.last_activity_at =
      std::max(cluster->second.last_activity_at, at_time);
  LOG_DEBUG("Probe send completed: id={} sent_packets={} sent_wire_bytes={} "
            "received_wire_bytes={} received_fraction={}",
            cluster_id, sent_packets, cluster->second.sent_size.bytes(),
            cluster->second.size_total.bytes(),
            ReceivedFraction(cluster->second));
  if (!cluster->second.result_reported || cluster->second.rejection_reason)
    return std::nullopt;
  return ReceivedFraction(cluster->second);
}

void ProbeBitrateEstimator::OnProbeAborted(int cluster_id) {
  if (max_expired_cluster_id_ && cluster_id <= *max_expired_cluster_id_) return;
  auto& cluster = clusters_[cluster_id];
  cluster.send_complete = false;
  cluster.send_aborted = true;
  cluster.rejection_reason = "send_aborted";
  rejected_cluster_id_ = std::max(rejected_cluster_id_, cluster_id);
  if (estimated_cluster_id_ == cluster_id) estimated_data_rate_.reset();
}

double ProbeBitrateEstimator::ReceivedFraction(
    const AggregatedCluster& cluster) {
  if (!cluster.send_complete || cluster.sent_size <= DataSize::Zero() ||
      cluster.size_total > cluster.sent_size)
    return 0;
  return cluster.size_total / cluster.sent_size;
}

std::optional<DataRate> ProbeBitrateEstimator::HandleProbeAndEstimateBitrate(
    const PacketResult& packet_feedback, Timestamp feedback_time) {
  int cluster_id = packet_feedback.sent_packet.pacing_info.probe_cluster_id;

  // Receiver timestamps describe packet spacing, not the sender's clock.
  // Expire state only with local feedback receipt/process timestamps.
  if (!feedback_time.IsFinite())
    feedback_time = packet_feedback.sent_packet.send_time;
  RemoveExpiredClusters(feedback_time);

  auto cluster_it = clusters_.find(cluster_id);
  if (cluster_it == clusters_.end()) {
    if (max_expired_cluster_id_ && cluster_id <= *max_expired_cluster_id_) {
      LOG_DEBUG("Probe feedback ignored: id={} reason=expired_cluster",
                cluster_id);
      return std::nullopt;
    }
    cluster_it = clusters_.try_emplace(cluster_id).first;
  }
  AggregatedCluster* cluster = &cluster_it->second;
  if (cluster->send_aborted) return std::nullopt;
  cluster->last_activity_at =
      std::max(cluster->last_activity_at, feedback_time);
  cluster->target_probes =
      packet_feedback.sent_packet.pacing_info.probe_cluster_min_probes;
  cluster->target_size = DataSize::Bytes(
      packet_feedback.sent_packet.pacing_info.probe_cluster_min_bytes);

  if (packet_feedback.sent_packet.send_time < cluster->first_send) {
    cluster->first_send = packet_feedback.sent_packet.send_time;
  }
  if (packet_feedback.sent_packet.send_time > cluster->last_send) {
    cluster->last_send = packet_feedback.sent_packet.send_time;
    cluster->size_last_send = packet_feedback.sent_packet.size;
  }
  if (packet_feedback.receive_time < cluster->first_receive) {
    cluster->first_receive = packet_feedback.receive_time;
    cluster->size_first_receive = packet_feedback.sent_packet.size;
  }
  if (packet_feedback.receive_time > cluster->last_receive) {
    cluster->last_receive = packet_feedback.receive_time;
  }
  cluster->size_total += packet_feedback.sent_packet.size;
  cluster->num_probes += 1;

  int min_probes =
      packet_feedback.sent_packet.pacing_info.probe_cluster_min_probes *
      kMinReceivedProbesRatio;
  DataSize min_size =
      DataSize::Bytes(
          packet_feedback.sent_packet.pacing_info.probe_cluster_min_bytes) *
      kMinReceivedBytesRatio;
  if (cluster->num_probes < min_probes || cluster->size_total < min_size) {
    cluster->rejection_reason = "insufficient_feedback";
    return std::nullopt;
  }

  TimeDelta send_interval = cluster->last_send - cluster->first_send;
  TimeDelta receive_interval = cluster->last_receive - cluster->first_receive;

  if (send_interval <= TimeDelta::Zero() || send_interval > kMaxProbeInterval ||
      receive_interval <= TimeDelta::Zero() ||
      receive_interval > kMaxProbeInterval) {
    cluster->rejection_reason = "invalid_interval";
    rejected_cluster_id_ = std::max(rejected_cluster_id_, cluster_id);
    if (estimated_cluster_id_ == cluster_id) estimated_data_rate_.reset();
    LOG_DEBUG(
        "Probe feedback rejected: id={} reason=invalid_interval "
        "actual_packets={} target_packets={} actual_bytes={} "
        "target_bytes={} send_interval_ms={} receive_interval_ms={}",
        cluster_id, cluster->num_probes, cluster->target_probes,
        cluster->size_total.bytes(), cluster->target_size.bytes(),
        send_interval.ms(), receive_interval.ms());

    return std::nullopt;
  }
  // Since the `send_interval` does not include the time it takes to actually
  // send the last packet the size of the last sent packet should not be
  // included when calculating the send bitrate.
  DataSize send_size = cluster->size_total - cluster->size_last_send;
  DataRate send_rate = send_size / send_interval;

  // Since the `receive_interval` does not include the time it takes to
  // actually receive the first packet the size of the first received packet
  // should not be included when calculating the receive bitrate.
  DataSize receive_size = cluster->size_total - cluster->size_first_receive;
  DataRate receive_rate = receive_size / receive_interval;

  double ratio = receive_rate / send_rate;
  if (ratio > kMaxValidRatio) {
    cluster->rejection_reason = "receive_send_ratio";
    rejected_cluster_id_ = std::max(rejected_cluster_id_, cluster_id);
    if (estimated_cluster_id_ == cluster_id) estimated_data_rate_.reset();
    LOG_DEBUG(
        "Probe feedback rejected: id={} reason=receive_send_ratio "
        "actual_packets={} target_packets={} actual_bytes={} "
        "target_bytes={} send_bitrate_bps={} receive_bitrate_bps={} ratio={}",
        cluster_id, cluster->num_probes, cluster->target_probes,
        cluster->size_total.bytes(), cluster->target_size.bytes(),
        send_rate.bps(), receive_rate.bps(), ratio);
    return std::nullopt;
  }
  if (!cluster->result_reported) {
    LOG_DEBUG(
        "Probe feedback accepted: id={} stage=first_valid actual_packets={} "
        "target_packets={} "
        "actual_bytes={} target_bytes={} send_bitrate_bps={} "
        "receive_bitrate_bps={}",
        cluster_id, cluster->num_probes, cluster->target_probes,
        cluster->size_total.bytes(), cluster->target_size.bytes(),
        send_rate.bps(), receive_rate.bps());
    cluster->result_reported = true;
  }
  cluster->rejection_reason = nullptr;

  DataRate res = std::min(send_rate, receive_rate);
  // If we're receiving at significantly lower bitrate than we were sending at,
  // it suggests that we've found the true capacity of the link. In this case,
  // set the target bitrate slightly lower to not immediately overuse.
  if (receive_rate < kMinRatioForUnsaturatedLink * send_rate) {
    res = kTargetUtilizationFraction * receive_rate;
  }

  // Late feedback may revise an older cluster after a newer one was consumed.
  // Report that revision to its own observer, but never adopt it as the newest
  // network estimate.
  if (cluster_id >= estimated_cluster_id_) {
    estimated_data_rate_ = res;
    estimated_cluster_id_ = cluster_id;
  }
  if (cluster_id >= rejected_cluster_id_) rejected_cluster_id_ = -1;
  return res;
}

std::optional<DataRate>
ProbeBitrateEstimator::FetchAndResetLastEstimatedBitrate(
    int* cluster_id, double* received_fraction, int* rejected_cluster_id) {
  if (cluster_id)
    *cluster_id = estimated_data_rate_ ? estimated_cluster_id_ : -1;
  if (received_fraction) {
    const auto cluster = clusters_.find(estimated_cluster_id_);
    *received_fraction = estimated_data_rate_ && cluster != clusters_.end()
                             ? ReceivedFraction(cluster->second)
                             : 0;
  }
  std::optional<DataRate> estimated_data_rate = estimated_data_rate_;
  if (rejected_cluster_id) *rejected_cluster_id = rejected_cluster_id_;
  rejected_cluster_id_ = -1;
  estimated_data_rate_.reset();
  return estimated_data_rate;
}

void ProbeBitrateEstimator::RemoveExpiredClusters(Timestamp timestamp) {
  for (auto it = clusters_.begin(); it != clusters_.end();) {
    if (it->second.last_activity_at + kMaxClusterHistory < timestamp) {
      if (!it->second.result_reported) {
        const int min_probes = static_cast<int>(
            it->second.target_probes * kMinReceivedProbesRatio);
        const DataSize min_size =
            it->second.target_size * kMinReceivedBytesRatio;
        const bool insufficient_feedback =
            it->second.num_probes < min_probes ||
            it->second.size_total < min_size;
        const char* reason =
            insufficient_feedback
                ? "insufficient_feedback"
                : (it->second.rejection_reason
                       ? it->second.rejection_reason
                       : "no_valid_estimate");
        LOG_DEBUG(
            "Probe feedback expired without estimate: id={} reason={} "
            "actual_packets={} target_packets={} actual_bytes={} "
            "target_bytes={}",
            it->first, reason, it->second.num_probes,
            it->second.target_probes, it->second.size_total.bytes(),
            it->second.target_size.bytes());
      }
      if (!max_expired_cluster_id_ ||
          it->first > *max_expired_cluster_id_) {
        max_expired_cluster_id_ = it->first;
      }
      if (estimated_cluster_id_ == it->first) estimated_data_rate_.reset();
      it = clusters_.erase(it);
    } else {
      ++it;
    }
  }
}
}  // namespace webrtc
}  // namespace minirtc
