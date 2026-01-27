/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "nack_requester.h"

#include "log.h"

namespace minirtc {
namespace {
constexpr int kMaxPacketAge = 10'000;
constexpr int kMaxNackPackets = 1000;
constexpr TimeDelta kDefaultRtt = TimeDelta::Millis(100);
// Number of times a packet can be nacked before giving up. Nack is sent at most
// every RTT.
constexpr int kMaxNackRetries = 100;
constexpr int kMaxReorderedPackets = 128;
constexpr int kNumReorderingBuckets = 10;
// constexpr TimeDelta kDefaultSendNackDelay = TimeDelta::Zero();
constexpr TimeDelta kDefaultSendNackDelay = TimeDelta::Millis(10);
}  // namespace

NackRequester::NackInfo::NackInfo()
    : seq_num(0),
      send_at_seq_num(0),
      created_at_time(Timestamp::MinusInfinity()),
      sent_at_time(Timestamp::MinusInfinity()),
      retries(0) {}

NackRequester::NackInfo::NackInfo(uint16_t seq_num, uint16_t send_at_seq_num,
                                  Timestamp created_at_time)
    : seq_num(seq_num),
      send_at_seq_num(send_at_seq_num),
      created_at_time(created_at_time),
      sent_at_time(Timestamp::MinusInfinity()),
      retries(0) {}

NackRequester::NackRequester(std::shared_ptr<Clock> clock,
                             NackSender* nack_sender,
                             KeyFrameRequestSender* keyframe_request_sender)
    : clock_(clock),
      nack_sender_(nack_sender),
      keyframe_request_sender_(keyframe_request_sender),
      reordering_histogram_(kNumReorderingBuckets, kMaxReorderedPackets),
      initialized_(false),
      rtt_(kDefaultRtt),
      newest_seq_num_(0),
      send_nack_delay_(kDefaultSendNackDelay) {}

NackRequester::~NackRequester() {}

int NackRequester::OnReceivedPacket(uint16_t seq_num) {
  return OnReceivedPacket(seq_num, false);
}

void NackRequester::ProcessNacks() {
  std::vector<uint16_t> nack_batch = GetNackBatch(kTimeOnly);
  if (!nack_batch.empty()) {
    // This batch of NACKs is triggered externally; there is no external
    // initiator who can batch them with other feedback messages.
    nack_sender_->SendNack(nack_batch, /*buffering_allowed=*/false);
  }
}

int NackRequester::OnReceivedPacket(uint16_t seq_num, bool is_recovered) {
  bool is_retransmitted = true;

  if (!initialized_) {
    newest_seq_num_ = seq_num;
    initialized_ = true;
    return 0;
  }

  if (seq_num == newest_seq_num_) return 0;

  if (AheadOf(newest_seq_num_, seq_num)) {
    // An out of order packet has been received.
    auto nack_list_it = nack_list_.find(seq_num);
    int nacks_sent_for_packet = 0;
    if (nack_list_it != nack_list_.end()) {
      nacks_sent_for_packet = nack_list_it->second.retries;
      nack_list_.erase(nack_list_it);
    }
    if (!is_retransmitted) UpdateReorderingStatistics(seq_num);
    return nacks_sent_for_packet;
  }

  if (is_recovered) {
    recovered_list_.insert(seq_num);

    // Remove old ones so we don't accumulate recovered packets.
    auto it = recovered_list_.lower_bound(seq_num - kMaxPacketAge);
    if (it != recovered_list_.begin())
      recovered_list_.erase(recovered_list_.begin(), it);

    // Do not send nack for packets recovered by FEC or RTX.
    return 0;
  }

  AddPacketsToNack(newest_seq_num_ + 1, seq_num);
  newest_seq_num_ = seq_num;

  // Are there any nacks that are waiting for this seq_num.
  std::vector<uint16_t> nack_batch = GetNackBatch(kSeqNumOnly);
  if (!nack_batch.empty()) {
    nack_sender_->SendNack(nack_batch, false);
  }

  return 0;
}

void NackRequester::ClearUpTo(uint16_t seq_num) {
  nack_list_.erase(nack_list_.begin(), nack_list_.lower_bound(seq_num));
  recovered_list_.erase(recovered_list_.begin(),
                        recovered_list_.lower_bound(seq_num));
}

void NackRequester::UpdateRtt(int64_t rtt_ms) {
  rtt_ = TimeDelta::Millis(rtt_ms);
}

void NackRequester::AddPacketsToNack(uint16_t seq_num_start,
                                     uint16_t seq_num_end) {
  // Remove old packets.
  auto it = nack_list_.lower_bound(seq_num_end - kMaxPacketAge);
  nack_list_.erase(nack_list_.begin(), it);

  uint16_t num_new_nacks = ForwardDiff(seq_num_start, seq_num_end);
  if (nack_list_.size() + num_new_nacks > kMaxNackPackets) {
    nack_list_.clear();
    LOG_WARN("NACK list full, clearing NACK list and requesting keyframe.");
    keyframe_request_sender_->RequestKeyFrame();
    return;
  }

  for (uint16_t seq_num = seq_num_start; seq_num != seq_num_end; ++seq_num) {
    // Do not send nack for packets that are already recovered by FEC or RTX
    if (recovered_list_.find(seq_num) != recovered_list_.end()) continue;
    NackInfo nack_info(seq_num, seq_num + WaitNumberOfPackets(0.5),
                       clock_->CurrentTime());
    nack_list_[seq_num] = nack_info;
  }
}

std::vector<uint16_t> NackRequester::GetNackBatch(NackFilterOptions options) {
  // Called on worker_thread_.

  bool consider_seq_num = options != kTimeOnly;
  bool consider_timestamp = options != kSeqNumOnly;
  Timestamp now = clock_->CurrentTime();
  std::vector<uint16_t> nack_batch;
  auto it = nack_list_.begin();
  while (it != nack_list_.end()) {
    bool delay_timed_out = now - it->second.created_at_time >= send_nack_delay_;
    bool nack_on_rtt_passed = now - it->second.sent_at_time >= rtt_;
    bool nack_on_seq_num_passed =
        it->second.sent_at_time.IsInfinite() &&
        AheadOrAt(newest_seq_num_, it->second.send_at_seq_num);
    if (delay_timed_out && ((consider_seq_num && nack_on_seq_num_passed) ||
                            (consider_timestamp && nack_on_rtt_passed))) {
      nack_batch.emplace_back(it->second.seq_num);
      ++it->second.retries;
      it->second.sent_at_time = now;
      if (it->second.retries >= kMaxNackRetries) {
        // LOG_WARN(
        //     "Sequence number {} removed from NACK list due to max retries.",
        //     it->second.seq_num);
        it = nack_list_.erase(it);
      } else {
        ++it;
      }
      continue;
    }
    ++it;
  }
  return nack_batch;
}

void NackRequester::UpdateReorderingStatistics(uint16_t seq_num) {
  uint16_t diff = ReverseDiff(newest_seq_num_, seq_num);
  reordering_histogram_.Add(diff);
}

int NackRequester::WaitNumberOfPackets(float probability) const {
  if (reordering_histogram_.NumValues() == 0) return 0;
  return reordering_histogram_.InverseCdf(probability);
}
}  // namespace minirtc