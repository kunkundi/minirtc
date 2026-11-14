/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _PACKET_LOSS_TRACKER_H_
#define _PACKET_LOSS_TRACKER_H_

#include <deque>
#include <unordered_map>

#include "api/transport/network_types.h"

namespace minirtc {
namespace webrtc {

// RFC 8888 reports can repeat a missing packet and later report its arrival.
// Settle receipts and losses over the same interval before computing a ratio.
class PacketLossTracker {
 public:
  struct Sample {
    int packets = 0;
    int lost = 0;
  };

  void Reset(Timestamp now);
  void OnFeedback(const TransportPacketsFeedback& report,
                  bool include_probes = true);
  Sample Advance(Timestamp now, TimeDelta rtt);
  Sample recent() const { return recent_; }
  static TimeDelta GracePeriod(TimeDelta rtt);

 private:
  struct Observation {
    Timestamp first_feedback;
    bool received;
    bool settled = false;
  };
  struct TimedSample {
    Timestamp at;
    Sample sample;
  };
  Timestamp epoch_ = Timestamp::MinusInfinity();
  int64_t retired_sequence_ = -1;
  std::unordered_map<int64_t, Observation> observations_;
  std::deque<int64_t> pending_;
  std::deque<int64_t> history_;
  std::deque<TimedSample> samples_;
  Sample recent_;
};

}  // namespace webrtc
}  // namespace minirtc

#endif
