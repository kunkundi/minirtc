
/*
 *  Copyright (c) 2024 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "congestion_control_feedback_tracker.h"

#include <cstdint>
#include <tuple>
#include <vector>

#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "congestion_control_feedback.h"
#include "rtp_packet_received.h"

namespace minirtc {
namespace webrtc {

void CongestionControlFeedbackTracker::ReceivedPacket(
    const RtpPacketReceived& packet) {
  int64_t unwrapped_sequence_number =
      unwrapper_.Unwrap(packet.SequenceNumber());
  if (last_sequence_number_in_feedback_ &&
      unwrapped_sequence_number < *last_sequence_number_in_feedback_ + 1) {
    // LOG_INFO(
    //     "Received packet unordered between feedback. SSRC: {} Seq: {} last "
    //     "feedback: {}",
    //     packet.Ssrc(), packet.SequenceNumber(),
    //     static_cast<uint16_t>(*last_sequence_number_in_feedback_));

    last_sequence_number_in_feedback_ = unwrapped_sequence_number - 1;
  }
  auto it = history_.find(unwrapped_sequence_number);
  if (it == history_.end()) {
    history_.emplace(unwrapped_sequence_number,
                     PacketInfo{packet.Ssrc(), unwrapped_sequence_number,
                                packet.arrival_time(), packet.ecn()});
    history_order_.push_back(unwrapped_sequence_number);
    if (history_order_.size() > kMaxHistorySize) {
      int64_t seq_to_remove = history_order_.front();
      history_order_.pop_front();
      history_.erase(seq_to_remove);
    }
  } else {
    if (packet.arrival_time() < it->second.arrival_time) {
      it->second.arrival_time = packet.arrival_time();
    }
    if (packet.ecn() == EcnMarking::kCe) {
      it->second.ecn = EcnMarking::kCe;
    }
  }
  packets_.push_back({packet.Ssrc(), unwrapped_sequence_number,
                      packet.arrival_time(), packet.ecn()});
}

void CongestionControlFeedbackTracker::AddPacketsToFeedback(
    Timestamp feedback_time,
    std::vector<rtcp::CongestionControlFeedback::PacketInfo>& packet_feedback) {
  if (packets_.empty()) {
    return;
  }
  std::sort(packets_.begin(), packets_.end(),
            [](const PacketInfo& a, const PacketInfo& b) {
              return std::tie(a.unwrapped_sequence_number, a.arrival_time) <
                     std::tie(b.unwrapped_sequence_number, b.arrival_time);
            });
  if (!last_sequence_number_in_feedback_) {
    last_sequence_number_in_feedback_ =
        packets_.front().unwrapped_sequence_number - 1;
  }

  auto packet_it = packets_.begin();
  uint32_t ssrc = packet_it->ssrc;
  for (int64_t sequence_number = *last_sequence_number_in_feedback_ + 1;
       sequence_number <= packets_.back().unwrapped_sequence_number;
       ++sequence_number) {
    EcnMarking ecn = EcnMarking::kNotEct;
    TimeDelta arrival_time_offset = TimeDelta::MinusInfinity();
    auto hist = history_.find(sequence_number);
    if (hist != history_.end()) {
      arrival_time_offset = feedback_time - hist->second.arrival_time;
      ecn = hist->second.ecn;
    }

    if (sequence_number == packet_it->unwrapped_sequence_number) {
      TimeDelta offset_current = feedback_time - packet_it->arrival_time;
      if (arrival_time_offset.IsFinite()) {
        arrival_time_offset = std::min(arrival_time_offset, offset_current);
      } else {
        arrival_time_offset = offset_current;
      }
      if (packet_it->ecn == EcnMarking::kCe) {
        ecn = EcnMarking::kCe;
      } else if (ecn == EcnMarking::kNotEct) {
        ecn = packet_it->ecn;
      }
      ++packet_it;
      while (packet_it != packets_.end() &&
             packet_it->unwrapped_sequence_number == sequence_number) {
        // According to RFC 8888:
        // If duplicate copies of a particular RTP packet are received, then
        // the arrival time of the first copy to arrive MUST be reported. If
        // any of the copies of the duplicated packet are ECN-CE marked, then
        // an ECN-CE mark MUST be reported for that packet; otherwise, the ECN
        // mark of the first copy to arrive is reported.
        TimeDelta candidate_offset = feedback_time - packet_it->arrival_time;
        if (arrival_time_offset.IsFinite()) {
          arrival_time_offset = std::min(arrival_time_offset, candidate_offset);
        } else {
          arrival_time_offset = candidate_offset;
        }
        if (packet_it->ecn == EcnMarking::kCe) {
          ecn = EcnMarking::kCe;
        }
        LOG_INFO("Received duplicate packet ssrc:{} seq:{} ecn:{}", ssrc,
                 static_cast<uint16_t>(sequence_number), static_cast<int>(ecn));
        ++packet_it;
      }
    }  // else - the packet has not been received yet.
    packet_feedback.push_back({ssrc, static_cast<uint16_t>(sequence_number),
                               arrival_time_offset, ecn});
  }
  last_sequence_number_in_feedback_ = packets_.back().unwrapped_sequence_number;
  packets_.clear();
}

}  // namespace webrtc
}  // namespace minirtc
