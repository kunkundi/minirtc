/*
 * @Author: DI JUNKUN
 * @Date: 2025-02-12
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _NACK_REQUESTER_H_
#define _NACK_REQUESTER_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "api/clock/clock.h"
#include "api/units/timestamp.h"
#include "histogram.h"
#include "rtc_base/numerics/sequence_number_util.h"

namespace minirtc {

using namespace webrtc;

class NackRequester {
 private:
  // Which fields to consider when deciding which packet to nack in
  // GetNackBatch.
  enum NackFilterOptions { kSeqNumOnly, kTimeOnly };

  struct NackInfo {
    NackInfo(uint16_t seq_num, uint16_t send_at_seq_num,
             Timestamp created_at_time);

    uint16_t seq_num;
    uint16_t send_at_seq_num;
    Timestamp created_at_time;
    Timestamp sent_at_time;
    Timestamp last_send_attempt_time;
    int retries;
    bool send_pending;
  };

 public:
  explicit NackRequester(std::shared_ptr<Clock> clock);
  ~NackRequester();

 public:
  std::vector<uint16_t> OnReceivedPacket(uint16_t seq_num, bool is_recovered);

  std::vector<uint16_t> ProcessNacks();
  void OnNackBatchSent(const std::vector<uint16_t>& nack_batch,
                       bool send_successful);
  bool ConsumeKeyFrameRequest();
  bool HasPendingNacks() const { return !nack_list_.empty(); }
  bool HasPendingNacksUpTo(uint16_t seq_num) const;
  int64_t RttMs() const { return rtt_.ms(); }
  bool HasRttSample() const { return has_rtt_sample_; }
  void UpdateRtt(int64_t rtt_ms);
  void ClearUpTo(uint16_t seq_num);

 private:
  void AddPacketsToNack(uint16_t seq_num_start, uint16_t seq_num_end);
  std::vector<uint16_t> GetNackBatch(NackFilterOptions options);
  void UpdateReorderingStatistics(uint16_t seq_num);
  int WaitNumberOfPackets(float probability) const;

 private:
  std::shared_ptr<Clock> clock_;
  std::map<uint16_t, NackInfo, DescendingSeqNumComp<uint16_t>> nack_list_;
  std::set<uint16_t, DescendingSeqNumComp<uint16_t>> recovered_list_;
  Histogram reordering_histogram_;
  bool initialized_;
  TimeDelta rtt_;
  bool has_rtt_sample_;
  bool keyframe_request_pending_;
  uint16_t newest_seq_num_;
  const TimeDelta send_nack_delay_;
};
}  // namespace minirtc

#endif
