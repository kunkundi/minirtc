/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "packet_loss_tracker.h"

#include <algorithm>

namespace minirtc {
namespace webrtc {

void PacketLossTracker::Reset(Timestamp now) {
  *this = PacketLossTracker();
  epoch_ = now;
}

TimeDelta PacketLossTracker::GracePeriod(TimeDelta rtt) {
  if (!rtt.IsFinite()) rtt = TimeDelta::Millis(100);
  return std::clamp(rtt * 2, TimeDelta::Millis(150), TimeDelta::Millis(500));
}

void PacketLossTracker::OnFeedback(const TransportPacketsFeedback& report,
                                   bool include_probes) {
  for (const auto& packet : report.packet_feedbacks) {
    const auto& sent = packet.sent_packet;
    if (!sent.send_time.IsFinite() || sent.send_time < epoch_ ||
        (!include_probes &&
         sent.pacing_info.probe_cluster_id != PacedPacketInfo::kNotAProbe))
      continue;
    auto it = observations_.find(sent.sequence_number);
    if (it != observations_.end()) {
      if (!it->second.settled && packet.IsReceived())
        it->second.received = true;
      continue;
    }
    // Retained entries deduplicate overlapping reports. The sequence watermark
    // also prevents old reports from reintroducing entries after eviction.
    if (sent.sequence_number <= retired_sequence_ ||
        observations_.size() >= 32768)
      continue;
    observations_.emplace(
        sent.sequence_number,
        Observation{report.feedback_time, packet.IsReceived()});
    pending_.push_back(sent.sequence_number);
    history_.push_back(sent.sequence_number);
  }
}

PacketLossTracker::Sample PacketLossTracker::Advance(Timestamp now,
                                                     TimeDelta rtt) {
  Sample result;
  const auto grace = GracePeriod(rtt);
  while (!pending_.empty()) {
    auto& observation = observations_.at(pending_.front());
    if (now - observation.first_feedback < grace) break;
    observation.settled = true;
    ++result.packets;
    if (!observation.received) ++result.lost;
    pending_.pop_front();
  }
  if (result.packets) {
    samples_.push_back({now, result});
    recent_.packets += result.packets;
    recent_.lost += result.lost;
  }
  while (!samples_.empty() &&
         now - samples_.front().at >= TimeDelta::Seconds(1)) {
    recent_.packets -= samples_.front().sample.packets;
    recent_.lost -= samples_.front().sample.lost;
    samples_.pop_front();
  }
  while (!history_.empty()) {
    auto it = observations_.find(history_.front());
    if (!it->second.settled ||
        now - it->second.first_feedback < TimeDelta::Seconds(3))
      break;
    retired_sequence_ = std::max(retired_sequence_, it->first);
    observations_.erase(it);
    history_.pop_front();
  }
  return result;
}

}  // namespace webrtc
}  // namespace minirtc
