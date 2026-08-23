#include "rtp_packet_history.h"

#include "log.h"
#include "sequence_number_compare.h"

namespace minirtc {

RtpPacketHistory::StoredPacket::StoredPacket(
    std::unique_ptr<webrtc::RtpPacketToSend> packet,
    webrtc::Timestamp send_time)
    : packet_(std::move(packet)),
      pending_transmission_(false),
      send_time_(send_time),
      times_retransmitted_(0) {}

RtpPacketHistory::StoredPacket::StoredPacket(StoredPacket&&) = default;
RtpPacketHistory::StoredPacket& RtpPacketHistory::StoredPacket::operator=(
    RtpPacketHistory::StoredPacket&&) = default;
RtpPacketHistory::StoredPacket::~StoredPacket() = default;

void RtpPacketHistory::StoredPacket::IncrementTimesRetransmitted() {
  ++times_retransmitted_;
}

RtpPacketHistory::RtpPacketHistory(std::shared_ptr<SystemClock> clock)
    : clock_(webrtc::Clock::GetWebrtcClockShared(clock)),
      rtt_(webrtc::TimeDelta::Millis(100)),
      number_to_store_(kMaxCapacity) {}

RtpPacketHistory::~RtpPacketHistory() {}

void RtpPacketHistory::SetRtt(webrtc::TimeDelta rtt) {
  rtt_ = rtt;
  RemoveDeadPackets();
}

void RtpPacketHistory::PutRtpPacket(
    std::unique_ptr<webrtc::RtpPacketToSend> rtp_packet, int64_t send_time) {
  RemoveDeadPackets();
  const uint16_t rtp_seq_no = rtp_packet->SequenceNumber();
  int packet_index = GetPacketIndex(rtp_packet->SequenceNumber());
  if (packet_index >= 0 &&
      static_cast<size_t>(packet_index) < packet_history_.size() &&
      packet_history_[packet_index].packet_ != nullptr) {
    LOG_WARN("Duplicate packet inserted: {}", rtp_seq_no);
    // Remove previous packet to avoid inconsistent state.
    RemovePacket(packet_index);
    packet_index = GetPacketIndex(rtp_seq_no);
  }

  // Packet to be inserted ahead of first packet, expand front.
  for (; packet_index < 0; ++packet_index) {
    packet_history_.emplace_front();
  }
  // Packet to be inserted behind last packet, expand back.
  while (static_cast<int>(packet_history_.size()) <= packet_index) {
    packet_history_.emplace_back();
  }

  packet_history_[packet_index] = {
      std::move(rtp_packet), webrtc::Timestamp::Micros(send_time)};
  // A discontinuous sequence jump can expand the deque by more than one slot
  // in this insertion. Cull again so the hard capacity also holds for that
  // case, not only for the normal contiguous path.
  RemoveDeadPackets();
}

void RtpPacketHistory::RemoveDeadPackets() {
  webrtc::Timestamp now = clock_->CurrentTime();
  webrtc::TimeDelta packet_duration =
      rtt_.IsFinite()
          ? (std::max)(kMinPacketDurationRtt * rtt_, kMinPacketDuration)
          : kMinPacketDuration;
  while (!packet_history_.empty()) {
    const StoredPacket& stored_packet = packet_history_.front();
    if (packet_history_.size() > kMaxCapacity) {
      // The pacer owns a complete RTX copy, so even a pending oldest entry can
      // be evicted without invalidating the queued datagram. Keeping it instead
      // would let one lost completion callback disable the capacity limit
      // forever.
      RemovePacket(0);
      continue;
    }

    if (stored_packet.pending_transmission_) {
      // Don't remove packets in the pacer queue, pending tranmission.
      return;
    }

    if (stored_packet.send_time() + packet_duration > now) {
      // Don't cull packets too early to avoid failed retransmission requests.
      return;
    }

    if (packet_history_.size() >= number_to_store_ ||
        stored_packet.send_time() +
                (packet_duration * kPacketCullingDelayFactor) <=
            now) {
      // Too many packets in history, or this packet has timed out. Remove it
      // and continue.
      RemovePacket(0);
    } else {
      // No more packets can be removed right now.
      return;
    }
  }
}

std::unique_ptr<webrtc::RtpPacketToSend>
RtpPacketHistory::GetPacketAndMarkAsPending(uint16_t sequence_number) {
  return GetPacketAndMarkAsPending(
      sequence_number, [](const webrtc::RtpPacketToSend& packet) {
        return std::make_unique<webrtc::RtpPacketToSend>(packet);
      });
}

std::unique_ptr<webrtc::RtpPacketToSend>
RtpPacketHistory::GetPacketAndMarkAsPending(
    uint16_t sequence_number,
    std::function<std::unique_ptr<webrtc::RtpPacketToSend>(
        const webrtc::RtpPacketToSend&)>
        encapsulate) {
  StoredPacket* packet = GetStoredPacket(sequence_number);
  if (packet == nullptr) {
    return nullptr;
  }

  if (packet->pending_transmission_) {
    // Packet already in pacer queue, ignore this request.
    return nullptr;
  }

  if (!VerifyRtt(*packet)) {
    // Packet already resent within too short a time window, ignore.
    return nullptr;
  }

  // Copy and/or encapsulate packet.
  std::unique_ptr<webrtc::RtpPacketToSend> encapsulated_packet =
      encapsulate(*packet->packet_);
  if (encapsulated_packet) {
    packet->pending_transmission_ = true;
  }

  return encapsulated_packet;
}

void RtpPacketHistory::MarkPacketAsSent(uint16_t sequence_number) {
  StoredPacket* packet = GetStoredPacket(sequence_number);
  if (packet == nullptr) {
    return;
  }

  // Update send-time, mark as no longer in pacer queue, and increment
  // transmission count.
  packet->set_send_time(clock_->CurrentTime());
  packet->pending_transmission_ = false;
  packet->IncrementTimesRetransmitted();
}

void RtpPacketHistory::MarkPacketAsAborted(uint16_t sequence_number) {
  StoredPacket* packet = GetStoredPacket(sequence_number);
  if (packet == nullptr) {
    return;
  }

  // The retransmission never reached the transport. Allow a later NACK to
  // create a fresh RTX packet without counting this attempt or delaying it by
  // an RTT.
  packet->pending_transmission_ = false;
}

bool RtpPacketHistory::VerifyRtt(
    const RtpPacketHistory::StoredPacket& packet) const {
  if (packet.times_retransmitted() > 0 &&
      clock_->CurrentTime() - packet.send_time() < rtt_) {
    // This packet has already been retransmitted once, and the time since
    // that even is lower than on RTT. Ignore request as this packet is
    // likely already in the network pipe.
    return false;
  }

  return true;
}

std::unique_ptr<webrtc::RtpPacketToSend> RtpPacketHistory::RemovePacket(
    int packet_index) {
  // Move the packet out from the StoredPacket container.
  std::unique_ptr<webrtc::RtpPacketToSend> rtp_packet =
      std::move(packet_history_[packet_index].packet_);
  if (packet_index == 0) {
    while (!packet_history_.empty() &&
           packet_history_.front().packet_ == nullptr) {
      packet_history_.pop_front();
    }
  }

  return rtp_packet;
}

int RtpPacketHistory::GetPacketIndex(uint16_t sequence_number) const {
  if (packet_history_.empty()) {
    return 0;
  }

  int first_seq = packet_history_.front().packet_->SequenceNumber();
  if (first_seq == sequence_number) {
    return 0;
  }

  int packet_index = sequence_number - first_seq;
  constexpr int kSeqNumSpan = std::numeric_limits<uint16_t>::max() + 1;

  if (IsNewerSequenceNumber(sequence_number, first_seq)) {
    if (sequence_number < first_seq) {
      // Forward wrap.
      packet_index += kSeqNumSpan;
    }
  } else if (sequence_number > first_seq) {
    // Backwards wrap.
    packet_index -= kSeqNumSpan;
  }

  return packet_index;
}

RtpPacketHistory::StoredPacket* RtpPacketHistory::GetStoredPacket(
    uint16_t sequence_number) {
  int index = GetPacketIndex(sequence_number);
  if (index < 0 || static_cast<size_t>(index) >= packet_history_.size() ||
      packet_history_[index].packet_ == nullptr) {
    return nullptr;
  }
  return &packet_history_[index];
}
}  // namespace minirtc
