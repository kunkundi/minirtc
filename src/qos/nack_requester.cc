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

#include <algorithm>

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
constexpr TimeDelta kFailedSendRetryDelay = TimeDelta::Millis(20);
// constexpr TimeDelta kDefaultSendNackDelay = TimeDelta::Zero();
constexpr TimeDelta kDefaultSendNackDelay = TimeDelta::Millis(10);
}  // namespace

NackRequester::NackInfo::NackInfo(uint16_t seq_num, uint16_t send_at_seq_num,
                                  Timestamp created_at_time)
    : seq_num(seq_num),
      send_at_seq_num(send_at_seq_num),
      created_at_time(created_at_time),
      sent_at_time(Timestamp::MinusInfinity()),
      last_send_attempt_time(Timestamp::MinusInfinity()),
      retries(0),
      send_pending(false) {}

NackRequester::NackRequester(std::shared_ptr<Clock> clock)
    : clock_(clock),
      reordering_histogram_(kNumReorderingBuckets, kMaxReorderedPackets),
      initialized_(false),
      rtt_(kDefaultRtt),
      has_rtt_sample_(false),
      keyframe_request_pending_(false),
      newest_seq_num_(0),
      send_nack_delay_(kDefaultSendNackDelay) {}

NackRequester::~NackRequester() {}

std::vector<uint16_t> NackRequester::ProcessNacks() {
  return GetNackBatch(kTimeOnly);
}

std::vector<uint16_t> NackRequester::OnReceivedPacket(uint16_t seq_num,
                                                      bool is_recovered) {
  if (!initialized_) {
    newest_seq_num_ = seq_num;
    initialized_ = true;
    return {};
  }

  if (seq_num == newest_seq_num_) return {};

  if (AheadOf(newest_seq_num_, seq_num)) {
    // An out of order packet has been received.
    auto nack_list_it = nack_list_.find(seq_num);
    if (nack_list_it != nack_list_.end()) {
      // Karn's algorithm: an RTX received after more than one successful NACK
      // cannot be attributed to a specific request. Sampling from the first
      // request in that case folds retransmission backoff into the RTT and can
      // keep subsequent frames blocked for seconds.
      if (is_recovered && nack_list_it->second.retries == 1 &&
          !nack_list_it->second.sent_at_time.IsInfinite()) {
        const int64_t rtt_sample_ms =
            (clock_->CurrentTime() - nack_list_it->second.sent_at_time).ms();
        UpdateRtt(rtt_sample_ms);
      }
      nack_list_.erase(nack_list_it);
    }
    if (!is_recovered) {
      UpdateReorderingStatistics(seq_num);
    }
    return {};
  }

  if (is_recovered) {
    recovered_list_.insert(seq_num);

    // Remove old ones so we don't accumulate recovered packets.
    auto it = recovered_list_.lower_bound(seq_num - kMaxPacketAge);
    if (it != recovered_list_.begin())
      recovered_list_.erase(recovered_list_.begin(), it);

    // Do not send nack for packets recovered by FEC or RTX.
    return {};
  }

  AddPacketsToNack(newest_seq_num_ + 1, seq_num);
  newest_seq_num_ = seq_num;

  // Are there any nacks that are waiting for this seq_num.
  return GetNackBatch(kSeqNumOnly);
}

void NackRequester::OnNackBatchSent(
    const std::vector<uint16_t>& nack_batch, bool send_successful) {
  const Timestamp sent_at = clock_->CurrentTime();
  for (uint16_t sequence_number : nack_batch) {
    auto it = nack_list_.find(sequence_number);
    if (it == nack_list_.end() || !it->second.send_pending) {
      continue;
    }

    it->second.send_pending = false;
    it->second.last_send_attempt_time = sent_at;
    if (!send_successful) {
      continue;
    }

    ++it->second.retries;
    it->second.sent_at_time = sent_at;
    if (it->second.retries >= kMaxNackRetries) {
      nack_list_.erase(it);
    }
  }
}

bool NackRequester::ConsumeKeyFrameRequest() {
  const bool request_keyframe = keyframe_request_pending_;
  keyframe_request_pending_ = false;
  return request_keyframe;
}

void NackRequester::ClearUpTo(uint16_t seq_num) {
  nack_list_.erase(nack_list_.begin(), nack_list_.upper_bound(seq_num));
  recovered_list_.erase(recovered_list_.begin(),
                        recovered_list_.upper_bound(seq_num));
}

bool NackRequester::HasPendingNacksUpTo(uint16_t seq_num) const {
  return nack_list_.begin() != nack_list_.upper_bound(seq_num);
}

void NackRequester::UpdateRtt(int64_t rtt_ms) {
  if (rtt_ms <= 0 || rtt_ms > 2000) {
    return;
  }
  if (has_rtt_sample_) {
    rtt_ = TimeDelta::Millis((3 * rtt_.ms() + rtt_ms) / 4);
  } else {
    rtt_ = TimeDelta::Millis(rtt_ms);
    has_rtt_sample_ = true;
  }
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
    keyframe_request_pending_ = true;
    return;
  }

  for (uint16_t seq_num = seq_num_start; seq_num != seq_num_end; ++seq_num) {
    // Do not send nack for packets that are already recovered by FEC or RTX
    if (recovered_list_.find(seq_num) != recovered_list_.end()) continue;
    nack_list_.insert_or_assign(
        seq_num, NackInfo(seq_num, seq_num + WaitNumberOfPackets(0.5),
                          clock_->CurrentTime()));
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
    if (it->second.send_pending) {
      ++it;
      continue;
    }
    bool delay_timed_out = now - it->second.created_at_time >= send_nack_delay_;
    TimeDelta retry_delay = rtt_;
    if (it->second.retries > 0) {
      // One RTT is enough for the first retry when a measured RTT is
      // available. Back later retries off to two and four RTTs so an RTX
      // packet delayed behind a TURN burst does not trigger a train of
      // duplicate retransmissions. Retain the more conservative bootstrap
      // behavior until a Karn-safe RTT sample exists.
      const int backoff_shift =
          has_rtt_sample_ ? std::min(it->second.retries - 1, 2)
                          : std::min(it->second.retries, 3);
      retry_delay = rtt_ * (1 << backoff_shift);
    }
    bool nack_on_rtt_passed =
        now - it->second.sent_at_time >= retry_delay;
    bool send_attempt_backoff_passed =
        now - it->second.last_send_attempt_time >= kFailedSendRetryDelay;
    bool nack_on_seq_num_passed =
        it->second.sent_at_time.IsInfinite() &&
        AheadOrAt(newest_seq_num_, it->second.send_at_seq_num);
    if (delay_timed_out && send_attempt_backoff_passed &&
        ((consider_seq_num && nack_on_seq_num_passed) ||
         (consider_timestamp && nack_on_rtt_passed))) {
      nack_batch.emplace_back(it->second.seq_num);
      // Commit retry counters and send time only after the transport reports
      // that the RTCP datagram was accepted.
      it->second.send_pending = true;
      ++it;
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
