/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "transport_feedback_adapter.h"

#include <stdlib.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "api/ntp/ntp_time_util.h"
#include "api/transport/ecn_marking.h"
#include "api/units/time_delta.h"
#include "congestion_control_feedback.h"
#include "log.h"
#include "rtp_packet_to_send.h"

namespace minirtc {
namespace webrtc {

constexpr TimeDelta kSendTimeHistoryWindow = TimeDelta::Seconds(60);

void InFlightBytesTracker::AddInFlightPacketBytes(
    const PacketFeedback& packet) {
  auto it = in_flight_data_.find(packet.network_route);
  if (it != in_flight_data_.end()) {
    it->second += packet.sent.size;
  } else {
    in_flight_data_.insert({packet.network_route, packet.sent.size});
  }
}

void InFlightBytesTracker::RemoveInFlightPacketBytes(
    const PacketFeedback& packet) {
  if (packet.sent.send_time.IsInfinite()) return;
  auto it = in_flight_data_.find(packet.network_route);
  if (it != in_flight_data_.end()) {
    it->second -= packet.sent.size;
    if (it->second.IsZero()) in_flight_data_.erase(it);
  }
}

DataSize InFlightBytesTracker::GetOutstandingData(
    const rtc::NetworkRoute& network_route) const {
  auto it = in_flight_data_.find(network_route);
  if (it != in_flight_data_.end()) {
    return it->second;
  } else {
    return DataSize::Zero();
  }
}

// Comparator for consistent map with NetworkRoute as key.
bool InFlightBytesTracker::NetworkRouteComparator::operator()(
    const rtc::NetworkRoute& a, const rtc::NetworkRoute& b) const {
  if (a.local.network_id() != b.local.network_id())
    return a.local.network_id() < b.local.network_id();
  if (a.remote.network_id() != b.remote.network_id())
    return a.remote.network_id() < b.remote.network_id();

  if (a.local.adapter_id() != b.local.adapter_id())
    return a.local.adapter_id() < b.local.adapter_id();
  if (a.remote.adapter_id() != b.remote.adapter_id())
    return a.remote.adapter_id() < b.remote.adapter_id();

  if (a.local.uses_turn() != b.local.uses_turn())
    return a.local.uses_turn() < b.local.uses_turn();
  if (a.remote.uses_turn() != b.remote.uses_turn())
    return a.remote.uses_turn() < b.remote.uses_turn();

  return a.connected < b.connected;
}

TransportFeedbackAdapter::TransportFeedbackAdapter() = default;

void TransportFeedbackAdapter::AddPacket(const RtpPacketToSend& packet_to_send,
                                         const PacedPacketInfo& pacing_info,
                                         size_t overhead_bytes,
                                         Timestamp creation_time) {
  if (!packet_to_send.transport_sequence_number().has_value()) {
    // Transport sequence number is required for per-packet feedback tracking.
    // If missing, do not insert into history (using a fake value like 0 would
    // corrupt state and break BWE/in-flight tracking).
    LOG_WARN(
        "AddPacket called without transport_sequence_number (ssrc={}, "
        "rtp_seq={}). "
        "Packet will not be tracked for feedback.",
        packet_to_send.Ssrc(), packet_to_send.SequenceNumber());
    return;
  }

  PruneRecentlyAcknowledgedPackets(creation_time);

  PacketFeedback feedback;

  feedback.creation_time = creation_time;
  // Note, if transport sequence number header extension is used, transport
  // sequence numbers are wrapped to 16 bit. See
  // RtpSenderEgress::CompleteSendPacket.
  feedback.sent.sequence_number =
      seq_num_unwrapper_.Unwrap(*packet_to_send.transport_sequence_number());
  feedback.sent.size = DataSize::Bytes(packet_to_send.size() + overhead_bytes);
  feedback.sent.audio =
      packet_to_send.packet_type() == RtpPacketMediaType::kAudio;
  feedback.network_route = network_route_;
  feedback.sent.pacing_info = pacing_info;
  feedback.ssrc = packet_to_send.Ssrc();
  feedback.rtp_sequence_number = packet_to_send.SequenceNumber();

  while (!history_.empty() &&
         creation_time - history_.begin()->second.creation_time >
             kSendTimeHistoryWindow) {
    // TODO(sprang): Warn if erasing (too many) old items?
    if (history_.begin()->second.sent.sequence_number > last_ack_seq_num_)
      in_flight_.RemoveInFlightPacketBytes(history_.begin()->second);

    const PacketFeedback& packet = history_.begin()->second;
    rtp_to_transport_sequence_number_.erase(
        {packet.ssrc, packet.rtp_sequence_number});
    history_.erase(history_.begin());
  }
  // Note that it can happen that the same SSRC and sequence number is sent
  // again. e.g, audio retransmission.
  // A new send reusing the same RTP key must not be classified as duplicate
  // feedback for the previous send.
  recently_acknowledged_packets_.erase(
      {feedback.ssrc, feedback.rtp_sequence_number});
  rtp_to_transport_sequence_number_.emplace(
      SsrcAndRtpSequencenumber({feedback.ssrc, feedback.rtp_sequence_number}),
      feedback.sent.sequence_number);
  history_.emplace(feedback.sent.sequence_number, feedback);
}

bool TransportFeedbackAdapter::RemovePacket(
    const RtpPacketToSend& packet_to_send) {
  const SsrcAndRtpSequencenumber key = {packet_to_send.Ssrc(),
                                        packet_to_send.SequenceNumber()};
  auto mapping_it = rtp_to_transport_sequence_number_.find(key);
  if (mapping_it == rtp_to_transport_sequence_number_.end()) {
    return false;
  }

  auto history_it = history_.find(mapping_it->second);
  if (history_it != history_.end()) {
    if (history_it->second.sent.send_time.IsFinite()) {
      in_flight_.RemoveInFlightPacketBytes(history_it->second);
    }
    history_.erase(history_it);
  }
  rtp_to_transport_sequence_number_.erase(mapping_it);
  return true;
}

std::optional<SentPacket> TransportFeedbackAdapter::ProcessSentPacket(
    const rtc::SentPacket& sent_packet) {
  auto send_time = Timestamp::Millis(sent_packet.send_time_ms);
  // TODO(srte): Only use one way to indicate that packet feedback is used.
  if (sent_packet.info.included_in_feedback || sent_packet.packet_id != -1) {
    int64_t unwrapped_seq_num =
        seq_num_unwrapper_.Unwrap(sent_packet.packet_id);
    auto it = history_.find(unwrapped_seq_num);
    if (it != history_.end()) {
      bool packet_retransmit = it->second.sent.send_time.IsFinite();
      it->second.sent.send_time = send_time;
      last_send_time_ = std::max(last_send_time_, send_time);
      // TODO(srte): Don't do this on retransmit.
      if (!pending_untracked_size_.IsZero()) {
        if (send_time < last_untracked_send_time_)
          LOG_WARN(
              "appending acknowledged data for out of order packet. (Diff: {} "
              "ms.)",
              ToString(last_untracked_send_time_ - send_time));
        it->second.sent.prior_unacked_data += pending_untracked_size_;
        pending_untracked_size_ = DataSize::Zero();
      }
      if (!packet_retransmit) {
        if (it->second.sent.sequence_number > last_ack_seq_num_)
          in_flight_.AddInFlightPacketBytes(it->second);
        it->second.sent.data_in_flight = GetOutstandingData();
        return it->second.sent;
      }
    }
  } else if (sent_packet.info.included_in_allocation) {
    if (send_time < last_send_time_) {
      LOG_WARN("ignoring untracked data for out of order packet.");
    }
    pending_untracked_size_ +=
        DataSize::Bytes(sent_packet.info.packet_size_bytes);
    last_untracked_send_time_ = std::max(last_untracked_send_time_, send_time);
  }
  return std::nullopt;
}

std::optional<TransportPacketsFeedback>
TransportFeedbackAdapter::ProcessCongestionControlFeedback(
    const rtcp::CongestionControlFeedback& feedback,
    Timestamp feedback_receive_time) {
  PruneRecentlyAcknowledgedPackets(feedback_receive_time);
  if (feedback.packets().empty()) {
    LOG_INFO("Empty congestion control feedback packet received.");
    return std::nullopt;
  }
  if (current_offset_.IsInfinite()) {
    current_offset_ = feedback_receive_time;
  }
  TimeDelta feedback_delta = last_feedback_compact_ntp_time_
                                 ? CompactNtpIntervalToTimeDelta(
                                       feedback.report_timestamp_compact_ntp() -
                                       *last_feedback_compact_ntp_time_)
                                 : TimeDelta::Zero();
  last_feedback_compact_ntp_time_ = feedback.report_timestamp_compact_ntp();
  if (feedback_delta < TimeDelta::Zero()) {
    LOG_WARN("Unexpected feedback ntp time delta {}", ToString(feedback_delta));
    current_offset_ = feedback_receive_time;
  } else {
    current_offset_ += feedback_delta;
  }

  int ignored_packets = 0;
  int failed_lookups = 0;
  int duplicate_feedbacks = 0;
  bool supports_ecn = true;
  std::vector<PacketResult> packet_result_vector;
  for (const rtcp::CongestionControlFeedback::PacketInfo& packet_info :
       feedback.packets()) {
    const SsrcAndRtpSequencenumber key = {packet_info.ssrc,
                                         packet_info.sequence_number};
    const bool received = packet_info.arrival_time_offset.IsFinite();
    std::optional<PacketFeedback> packet_feedback =
        RetrievePacketFeedback(key, received);
    if (!packet_feedback) {
      if (received && WasRecentlyAcknowledged(key)) {
        ++duplicate_feedbacks;
      } else {
        ++failed_lookups;
      }
      continue;
    }
    if (received) {
      RememberAcknowledgedPacket(key, feedback_receive_time);
    }
    if (packet_feedback->network_route != network_route_) {
      ++ignored_packets;
      continue;
    }
    PacketResult result;
    result.sent_packet = packet_feedback->sent;
    if (packet_info.arrival_time_offset.IsFinite()) {
      result.receive_time = current_offset_ - packet_info.arrival_time_offset;
      supports_ecn &= packet_info.ecn != EcnMarking::kNotEct;
    }
    result.ecn = packet_info.ecn;
    packet_result_vector.push_back(result);
  }

  if (failed_lookups > 0) {
    LOG_WARN(
        "Failed to lookup send time for {} packet{}. Packets reordered or "
        "send time history too small?",
        failed_lookups, (failed_lookups > 1 ? "s" : ""));
  }
  if (duplicate_feedbacks > 0) {
    LOG_INFO("Ignoring {} duplicate packet feedback{}.", duplicate_feedbacks,
             (duplicate_feedbacks > 1 ? "s" : ""));
  }
  if (ignored_packets > 0) {
    LOG_INFO("Ignoring {} packets because they were sent on a different route.",
             ignored_packets);
  }

  // Feedback is expected to be sorted in send order.
  std::sort(packet_result_vector.begin(), packet_result_vector.end(),
            [](const PacketResult& lhs, const PacketResult& rhs) {
              return lhs.sent_packet.sequence_number <
                     rhs.sent_packet.sequence_number;
            });
  return ToTransportFeedback(std::move(packet_result_vector),
                             feedback_receive_time, supports_ecn);
}

std::optional<TransportPacketsFeedback>
TransportFeedbackAdapter::ToTransportFeedback(
    std::vector<PacketResult> packet_results, Timestamp feedback_receive_time,
    bool supports_ecn) {
  TransportPacketsFeedback msg;
  msg.feedback_time = feedback_receive_time;
  if (packet_results.empty()) {
    return std::nullopt;
  }
  msg.packet_feedbacks = std::move(packet_results);
  msg.data_in_flight = in_flight_.GetOutstandingData(network_route_);
  msg.transport_supports_ecn = supports_ecn;

  return msg;
}

void TransportFeedbackAdapter::SetNetworkRoute(
    const rtc::NetworkRoute& network_route) {
  network_route_ = network_route;
}

DataSize TransportFeedbackAdapter::GetOutstandingData() const {
  return in_flight_.GetOutstandingData(network_route_);
}

void TransportFeedbackAdapter::PruneRecentlyAcknowledgedPackets(
    Timestamp now) {
  while (!recently_acknowledged_packets_in_order_.empty() &&
         now - recently_acknowledged_packets_in_order_.front()
                   .acknowledgment_time >
             kSendTimeHistoryWindow) {
    const RecentlyAcknowledgedPacket& acknowledged_packet =
        recently_acknowledged_packets_in_order_.front();
    auto it = recently_acknowledged_packets_.find(acknowledged_packet.key);
    if (it != recently_acknowledged_packets_.end() &&
        it->second == acknowledged_packet.generation) {
      recently_acknowledged_packets_.erase(it);
    }
    recently_acknowledged_packets_in_order_.pop_front();
  }
}

void TransportFeedbackAdapter::RememberAcknowledgedPacket(
    const SsrcAndRtpSequencenumber& key, Timestamp acknowledgment_time) {
  const uint64_t generation = ++acknowledgment_generation_;
  recently_acknowledged_packets_[key] = generation;
  recently_acknowledged_packets_in_order_.push_back(
      {key, acknowledgment_time, generation});
}

bool TransportFeedbackAdapter::WasRecentlyAcknowledged(
    const SsrcAndRtpSequencenumber& key) const {
  return recently_acknowledged_packets_.find(key) !=
         recently_acknowledged_packets_.end();
}

std::optional<PacketFeedback> TransportFeedbackAdapter::RetrievePacketFeedback(
    const SsrcAndRtpSequencenumber& key, bool received) {
  auto it = rtp_to_transport_sequence_number_.find(key);
  if (it == rtp_to_transport_sequence_number_.end()) {
    return std::nullopt;
  }
  return RetrievePacketFeedback(it->second, received);
}

std::optional<PacketFeedback> TransportFeedbackAdapter::RetrievePacketFeedback(
    int64_t transport_seq_num, bool received) {
  if (transport_seq_num > last_ack_seq_num_) {
    // Starts at history_.begin() if last_ack_seq_num_ < 0, since any
    // valid sequence number is >= 0.
    for (auto it = history_.upper_bound(last_ack_seq_num_);
         it != history_.upper_bound(transport_seq_num); ++it) {
      in_flight_.RemoveInFlightPacketBytes(it->second);
    }
    last_ack_seq_num_ = transport_seq_num;
  }

  auto it = history_.find(transport_seq_num);
  if (it == history_.end()) {
    LOG_WARN(
        "Failed to lookup send time for packet with transport_seq_num {}. Send "
        "time history too "
        "small?",
        transport_seq_num);
    return std::nullopt;
  }

  if (it->second.sent.send_time.IsInfinite()) {
    // TODO(srte): Fix the tests that makes this happen and make this a
    // DCHECK.
    LOG_ERROR("Received feedback before packet was indicated as sent");
    return std::nullopt;
  }

  PacketFeedback packet_feedback = it->second;
  if (received) {
    // Note: Lost packets are not removed from history because they might
    // be reported as received by a later feedback.
    rtp_to_transport_sequence_number_.erase(
        {packet_feedback.ssrc, packet_feedback.rtp_sequence_number});
    history_.erase(it);
  }
  return packet_feedback;
}

}  // namespace webrtc
}  // namespace minirtc
