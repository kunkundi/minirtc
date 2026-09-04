/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-04
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_TIMESTAMP_H_
#define _RTP_TIMESTAMP_H_

#include <cstdint>
#include <mutex>
#include <optional>

#include "rtc_base/numerics/sequence_number_unwrapper.h"

namespace minirtc {

// Maps a monotonic capture clock to the 32-bit RTP timestamp space owned by a
// single SSRC. RTP wraparound is intentional and follows uint32_t arithmetic.
class RtpTimestampGenerator {
 public:
  RtpTimestampGenerator(uint32_t clock_rate, uint32_t base_timestamp)
      : clock_rate_(clock_rate), base_timestamp_(base_timestamp) {}

  uint32_t TimestampForCaptureTimeUs(int64_t capture_time_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!first_capture_time_us_) {
      first_capture_time_us_ = capture_time_us;
      return base_timestamp_;
    }

    return TimestampForTimeUsLocked(capture_time_us);
  }

  // Padding can be requested before the first media frame. In that case use
  // the base without moving the media timeline's first-capture anchor.
  uint32_t TimestampForPaddingTimeUs(int64_t time_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!first_capture_time_us_) {
      return base_timestamp_;
    }
    return TimestampForTimeUsLocked(time_us);
  }

 private:
  uint32_t TimestampForTimeUsLocked(int64_t time_us) const {
    const int64_t elapsed_us = time_us - *first_capture_time_us_;

    if (elapsed_us <= 0) {
      return base_timestamp_;
    }

    // Split seconds and microseconds so long-running streams cannot overflow
    // while evaluating elapsed_us * clock_rate_. Fractional ticks are rounded
    // down; the mapping stays anchored to the first capture timestamp.
    const uint64_t elapsed = static_cast<uint64_t>(elapsed_us);
    const uint64_t elapsed_ticks =
        (elapsed / kMicrosecondsPerSecond) * clock_rate_ +
        ((elapsed % kMicrosecondsPerSecond) * clock_rate_) /
            kMicrosecondsPerSecond;
    return base_timestamp_ + static_cast<uint32_t>(elapsed_ticks);
  }

  static constexpr uint64_t kMicrosecondsPerSecond = 1'000'000;

  const uint32_t clock_rate_;
  const uint32_t base_timestamp_;
  std::optional<int64_t> first_capture_time_us_;
  std::mutex mutex_;
};

// Generates RTP timestamps for sample-clocked media such as Opus. One sample
// per channel advances the 48 kHz Opus RTP clock by one tick. The counter is
// owned by one SSRC and intentionally wraps in uint32_t space.
class RtpSampleTimestampGenerator {
 public:
  explicit RtpSampleTimestampGenerator(uint32_t base_timestamp)
      : base_timestamp_(base_timestamp) {}

  uint32_t NextTimestamp(uint32_t samples_per_channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t timestamp =
        base_timestamp_ + static_cast<uint32_t>(sample_count_);
    sample_count_ += samples_per_channel;
    return timestamp;
  }

 private:
  const uint32_t base_timestamp_;
  uint64_t sample_count_ = 0;
  std::mutex mutex_;
};

// Reconstructs a stable local presentation timeline from a remote RTP clock.
// The sender's random RTP base has no absolute-time meaning, so the first
// complete frame is anchored to its local monotonic arrival time.
class RtpTimestampMapper {
 public:
  explicit RtpTimestampMapper(uint32_t clock_rate) : clock_rate_(clock_rate) {}

  int64_t ToLocalTimeUs(uint32_t rtp_timestamp, int64_t arrival_time_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t unwrapped_timestamp = unwrapper_.Unwrap(rtp_timestamp);
    if (!first_unwrapped_timestamp_) {
      first_unwrapped_timestamp_ = unwrapped_timestamp;
      first_local_time_us_ = arrival_time_us;
      return arrival_time_us;
    }

    const int64_t elapsed_ticks =
        unwrapped_timestamp - *first_unwrapped_timestamp_;
    return first_local_time_us_ + TicksToMicroseconds(elapsed_ticks);
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    unwrapper_.Reset();
    first_unwrapped_timestamp_.reset();
    first_local_time_us_ = 0;
  }

 private:
  int64_t TicksToMicroseconds(int64_t ticks) const {
    if (clock_rate_ == 0) {
      return 0;
    }
    return (ticks / clock_rate_) *
               static_cast<int64_t>(kMicrosecondsPerSecond) +
           (ticks % clock_rate_) *
               static_cast<int64_t>(kMicrosecondsPerSecond) / clock_rate_;
  }

  static constexpr uint64_t kMicrosecondsPerSecond = 1'000'000;

  const uint32_t clock_rate_;
  webrtc::RtpTimestampUnwrapper unwrapper_;
  std::optional<int64_t> first_unwrapped_timestamp_;
  int64_t first_local_time_us_ = 0;
  std::mutex mutex_;
};

}  // namespace minirtc

#endif