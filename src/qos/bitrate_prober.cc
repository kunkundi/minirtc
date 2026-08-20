/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "bitrate_prober.h"

#include <algorithm>
#include <cstddef>
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
constexpr TimeDelta kProbeClusterTimeout = TimeDelta::Seconds(5);
constexpr size_t kMaxPendingProbeClusters = 5;

}  // namespace

BitrateProberConfig::BitrateProberConfig()
    : max_probe_delay(TimeDelta::Millis(10)),
      min_packet_size(DataSize::Bytes(200)) {}

BitrateProber::BitrateProber()
    : probing_state_(ProbingState::kDisabled),
      next_probe_time_(Timestamp::PlusInfinity()) {
  SetEnabled(true);
}

void BitrateProber::SetEnabled(bool enable) {
  if (enable) {
    if (probing_state_ == ProbingState::kDisabled) {
      probing_state_ = ProbingState::kInactive;
      LOG_INFO("Bandwidth probing enabled, set to inactive");
    }
  } else {
    probing_state_ = ProbingState::kDisabled;
    LOG_INFO("Bandwidth probing disabled");
  }
}

void BitrateProber::SetAllowProbeWithoutMediaPacket(bool allow) {
  config_.allow_start_probing_immediately = allow;
  MaybeSetActiveState(/*packet_size=*/DataSize::Zero());
}

void BitrateProber::AbortProbing(const char* reason) {
  while (!clusters_.empty()) {
    const ProbeCluster& cluster = clusters_.front();
    LOG_INFO(
        "Probe cluster aborted: id={} reason={} actual_bytes={} "
        "actual_packets={} actual_probe_batches={} target_bytes={} "
        "target_probe_batches={}",
        cluster.pace_info.probe_cluster_id, reason, cluster.sent_bytes,
        cluster.sent_packets, cluster.sent_probes,
        cluster.pace_info.probe_cluster_min_bytes,
        cluster.pace_info.probe_cluster_min_probes);
    clusters_.pop();
  }
  next_probe_time_ = Timestamp::PlusInfinity();
  if (probing_state_ != ProbingState::kDisabled) {
    probing_state_ = ProbingState::kInactive;
  }
}

void BitrateProber::MaybeSetActiveState(DataSize packet_size) {
  if (ReadyToSetActiveState(packet_size)) {
    next_probe_time_ = Timestamp::MinusInfinity();
    probing_state_ = ProbingState::kActive;
  }
}

bool BitrateProber::ReadyToSetActiveState(DataSize packet_size) const {
  if (clusters_.empty()) {
    return false;
  }
  switch (probing_state_) {
    case ProbingState::kDisabled:
      return false;
    case ProbingState::kActive:
      return false;
    case ProbingState::kInactive:
      if (config_.allow_start_probing_immediately) {
        return true;
      }
      // If config_.min_packet_size > 0, a "large enough" packet must be
      // sent first, before a probe can be generated and sent. Otherwise,
      // send the probe asap.
      return packet_size >=
             std::min(RecommendedMinProbeSize(), config_.min_packet_size);
  }

  return false;
}

void BitrateProber::OnIncomingPacket(DataSize packet_size) {
  MaybeSetActiveState(packet_size);
}

void BitrateProber::CreateProbeCluster(
    const ProbeClusterConfig& cluster_config) {
  RemoveExpiredClusters(cluster_config.at_time);
  while (!clusters_.empty() &&
         clusters_.size() >= kMaxPendingProbeClusters) {
    const ProbeCluster& cluster = clusters_.front();
    LOG_WARN(
        "Probe cluster discarded: id={} reason=pending_queue_limit "
        "actual_bytes={} "
        "actual_packets={} target_bytes={} target_probe_batches={}",
        cluster.pace_info.probe_cluster_id, cluster.sent_bytes,
        cluster.sent_packets, cluster.pace_info.probe_cluster_min_bytes,
        cluster.pace_info.probe_cluster_min_probes);
    clusters_.pop();
  }

  ProbeCluster cluster;
  cluster.requested_at = cluster_config.at_time;
  cluster.pace_info.probe_cluster_min_probes =
      cluster_config.target_probe_count;
  cluster.pace_info.probe_cluster_min_bytes =
      (cluster_config.target_data_rate * cluster_config.target_duration)
          .bytes();
  cluster.min_probe_delta = cluster_config.min_probe_delta;
  cluster.pace_info.send_bitrate = cluster_config.target_data_rate;
  cluster.pace_info.probe_cluster_id = cluster_config.id;
  clusters_.push(cluster);

  LOG_INFO(
      "Probe cluster queued: id={} target_bitrate_bps={} target_bytes={} "
      "target_probe_batches={} min_probe_delta_ms={} "
      "allow_without_media={}",
      cluster_config.id, cluster_config.target_data_rate.bps(),
      cluster.pace_info.probe_cluster_min_bytes,
      cluster.pace_info.probe_cluster_min_probes,
      cluster.min_probe_delta.ms(),
      config_.allow_start_probing_immediately ? "true" : "false");

  MaybeSetActiveState(/*packet_size=*/DataSize::Zero());
}

bool BitrateProber::RemoveExpiredClusters(Timestamp now) {
  bool removed_cluster = false;
  while (!clusters_.empty() &&
         now - clusters_.front().requested_at >= kProbeClusterTimeout) {
    const ProbeCluster& cluster = clusters_.front();
    const char* reason = probing_state_ == ProbingState::kActive
                             ? "send_timeout"
                             : "pending_timeout";
    LOG_WARN(
        "Probe cluster aborted: id={} reason={} actual_bytes={} "
        "actual_packets={} actual_probe_batches={} target_bytes={} "
        "target_probe_batches={} age_ms={}",
        cluster.pace_info.probe_cluster_id, reason, cluster.sent_bytes,
        cluster.sent_packets, cluster.sent_probes,
        cluster.pace_info.probe_cluster_min_bytes,
        cluster.pace_info.probe_cluster_min_probes,
        (now - cluster.requested_at).ms());
    clusters_.pop();
    removed_cluster = true;
  }

  if (!removed_cluster) {
    return false;
  }

  if (clusters_.empty()) {
    next_probe_time_ = Timestamp::PlusInfinity();
    if (probing_state_ != ProbingState::kDisabled) {
      probing_state_ = ProbingState::kInactive;
    }
  } else if (probing_state_ == ProbingState::kActive) {
    next_probe_time_ = Timestamp::MinusInfinity();
  }
  return true;
}

Timestamp BitrateProber::NextClusterExpiration() const {
  if (clusters_.empty()) {
    return Timestamp::PlusInfinity();
  }
  return clusters_.front().requested_at + kProbeClusterTimeout;
}

Timestamp BitrateProber::NextProbeTime(Timestamp /* now */) const {
  // Probing is not active or probing is already complete.
  if (probing_state_ != ProbingState::kActive || clusters_.empty()) {
    return Timestamp::PlusInfinity();
  }

  return next_probe_time_;
}

std::optional<PacedPacketInfo> BitrateProber::CurrentCluster(Timestamp now) {
  if (clusters_.empty() || probing_state_ != ProbingState::kActive) {
    return std::nullopt;
  }

  if (next_probe_time_.IsFinite() &&
      now - next_probe_time_ > config_.max_probe_delay) {
    const ProbeCluster& cluster = clusters_.front();
    LOG_WARN(
        "Probe cluster aborted: id={} reason=schedule_delay delay_ms={} "
        "actual_bytes={} actual_packets={} actual_probe_batches={} "
        "target_bytes={} target_probe_batches={}",
        cluster.pace_info.probe_cluster_id,
        (now - next_probe_time_).ms(), cluster.sent_bytes,
        cluster.sent_packets, cluster.sent_probes,
        cluster.pace_info.probe_cluster_min_bytes,
        cluster.pace_info.probe_cluster_min_probes);
    clusters_.pop();
    if (clusters_.empty()) {
      probing_state_ = ProbingState::kInactive;
      return std::nullopt;
    }
  }

  PacedPacketInfo info = clusters_.front().pace_info;
  info.probe_cluster_bytes_sent = clusters_.front().sent_bytes;
  return info;
}

DataSize BitrateProber::RecommendedMinProbeSize() const {
  if (clusters_.empty()) {
    return DataSize::Zero();
  }
  DataRate send_rate = clusters_.front().pace_info.send_bitrate;
  return send_rate * clusters_.front().min_probe_delta;
}

void BitrateProber::ProbeSent(Timestamp now, DataSize size, int packet_count) {
  if (!clusters_.empty()) {
    ProbeCluster* cluster = &clusters_.front();
    if (cluster->sent_probes == 0) {
      cluster->started_at = now;
    }
    cluster->sent_bytes += size.bytes<int>();
    cluster->sent_packets += packet_count;
    cluster->sent_probes += 1;
    next_probe_time_ = CalculateNextProbeTime(*cluster);
    if (cluster->sent_bytes >= cluster->pace_info.probe_cluster_min_bytes &&
        cluster->sent_probes >= cluster->pace_info.probe_cluster_min_probes) {
      LOG_INFO(
          "Probe cluster sent: id={} actual_bitrate_bps={} actual_bytes={} "
          "actual_packets={} actual_probe_batches={} target_bitrate_bps={} "
          "target_bytes={} target_probe_batches={} duration_ms={}",
          cluster->pace_info.probe_cluster_id,
          (DataSize::Bytes(cluster->sent_bytes) /
           std::max(now - cluster->started_at, TimeDelta::Micros(1)))
              .bps(),
          cluster->sent_bytes, cluster->sent_packets, cluster->sent_probes,
          cluster->pace_info.send_bitrate.bps(),
          cluster->pace_info.probe_cluster_min_bytes,
          cluster->pace_info.probe_cluster_min_probes,
          (now - cluster->started_at).ms());
      clusters_.pop();
    }
    if (clusters_.empty()) {
      probing_state_ = ProbingState::kInactive;
    }
  }
}

Timestamp BitrateProber::CalculateNextProbeTime(
    const ProbeCluster& cluster) const {
  // Compute the time delta from the cluster start to ensure probe bitrate stays
  // close to the target bitrate. Result is in milliseconds.
  DataSize sent_bytes = DataSize::Bytes(cluster.sent_bytes);
  DataRate send_bitrate = cluster.pace_info.send_bitrate;

  TimeDelta delta = sent_bytes / send_bitrate;
  return cluster.started_at + delta;
}

}  // namespace webrtc
}  // namespace minirtc
