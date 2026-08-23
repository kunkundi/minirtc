/*
 * @Author: DI JUNKUN
 * @Date: 2025-02-18
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTCP_REPORT_BLOCK_H_
#define _RTCP_REPORT_BLOCK_H_

#include <stddef.h>
#include <stdint.h>

#include "log.h"

namespace minirtc {

class RtcpReportBlock {
 public:
  static constexpr size_t kLength = 24;

  RtcpReportBlock();
  ~RtcpReportBlock() {}

 public:
  void SetMediaSsrc(uint32_t ssrc) { source_ssrc_ = ssrc; }
  void SetFractionLost(uint8_t fraction_lost) {
    fraction_lost_ = fraction_lost;
  }
  bool SetCumulativeLost(int32_t cumulative_lost) {
    // We have only 3 bytes to store it, and it's a signed value.
    if (cumulative_lost >= (1 << 23) || cumulative_lost < -(1 << 23)) {
      LOG_WARN("Cumulative lost is too big to fit into Report Block");
      return false;
    }
    cumulative_lost_ = cumulative_lost;
    return true;
  }
  void SetExtHighestSeqNum(uint32_t ext_highest_seq_num) {
    extended_high_seq_num_ = ext_highest_seq_num;
  }
  void SetJitter(uint32_t jitter) { jitter_ = jitter; }
  void SetLastSr(uint32_t last_sr) { last_sr_ = last_sr; }
  void SetDelayLastSr(uint32_t delay_last_sr) {
    delay_since_last_sr_ = delay_last_sr;
  }

 public:
  size_t Create(uint8_t* buffer) const;
  size_t Parse(const uint8_t* buffer);
  void SetReportBlock(uint32_t sender_ssrc, const RtcpReportBlock& report_block,
                      int64_t report_block_timestamp_utc,
                      int64_t report_block_timestamp);
  void AddRoundTripTimeSample(int64_t rtt);

 public:
  uint32_t SourceSsrc() const { return source_ssrc_; }
  uint8_t FractionLost() const { return fraction_lost_; }
  int32_t CumulativeLost() const { return cumulative_lost_; }
  uint32_t ExtendedHighSeqNum() const { return extended_high_seq_num_; }
  uint32_t Jitter() const { return jitter_; }
  uint32_t LastSr() const { return last_sr_; }
  uint32_t DelaySinceLastSr() const { return delay_since_last_sr_; }
  bool HasRtt() const { return num_rtts_ != 0; }
  int64_t LastRtt() const { return last_rtt_; }

 private:
  uint32_t sender_ssrc_;
  uint32_t source_ssrc_;     // 32 bits
  uint8_t fraction_lost_;    // 8 bits representing a fixed point value 0..1
  int32_t cumulative_lost_;  // Signed 24-bit value
  uint32_t extended_high_seq_num_;  // 32 bits
  uint32_t jitter_;                 // 32 bits
  uint32_t last_sr_;                // 32 bits
  uint32_t delay_since_last_sr_;    // 32 bits, units of 1/65536 seconds

  int64_t report_block_timestamp_utc_;
  int64_t report_block_timestamp_;

  int64_t last_rtt_;
  int64_t sum_rtt_;
  size_t num_rtts_;
};
}  // namespace minirtc

#endif
