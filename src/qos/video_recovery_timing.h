/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-04
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_RECOVERY_TIMING_H_
#define _VIDEO_RECOVERY_TIMING_H_

#include <algorithm>
#include <cstdint>

namespace minirtc {
namespace video_recovery {

// `successful_nack_count` is the number of successful NACK transmissions for
// one missing packet.  Once an RTT sample exists, keep the early recovery
// attempts close together: a 1x/1x/2x schedule gives a lossy TURN path four
// transmission opportunities before the frame recovery deadline.  Capping at
// 2x RTT keeps the later duplicate-request rate bounded.
constexpr int NackRetryRttMultiplier(bool has_rtt_sample,
                                     int successful_nack_count) {
  if (successful_nack_count <= 0) {
    return 1;
  }
  if (has_rtt_sample) {
    return successful_nack_count <= 2 ? 1 : 2;
  }

  // Before a Karn-safe RTT sample exists, preserve the conservative bootstrap
  // behavior based on the 100 ms default RTT.
  const int backoff_shift = std::min(successful_nack_count, 3);
  return 1 << backoff_shift;
}

constexpr int64_t SoftFrameRecoveryDeadlineMs(int64_t rtt_ms) {
  return std::clamp<int64_t>(2 * rtt_ms + 30, 100, 500);
}

constexpr int64_t HardFrameRecoveryDeadlineMs(int64_t rtt_ms) {
  // The measured-RTT retry schedule sends at approximately 0x, 1x, 2x and 4x
  // RTT.  Reserve one more RTT plus processing margin for the final RTX to
  // return before abandoning the frame.  The previous 4x+80 deadline could
  // expire before that last recovery response arrived.
  return std::clamp<int64_t>(5 * rtt_ms + 80, 350, 1000);
}

}  // namespace video_recovery
}  // namespace minirtc

#endif