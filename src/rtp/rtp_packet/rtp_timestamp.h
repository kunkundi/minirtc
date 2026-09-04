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

namespace minirtc {

constexpr int64_t kRtpTimestampMicrosecondsPerSecond = 1'000'000;

inline int64_t RtpTicksToMicroseconds(int64_t ticks, uint32_t clock_rate) {
  if (clock_rate == 0) {
    return 0;
  }
  return (ticks / clock_rate) * kRtpTimestampMicrosecondsPerSecond +
         (ticks % clock_rate) * kRtpTimestampMicrosecondsPerSecond /
             clock_rate;
}

inline uint32_t ExtrapolateRtpTimestamp(uint32_t anchor_rtp_timestamp,
                                        int64_t anchor_time_us,
                                        int64_t target_time_us,
                                        uint32_t clock_rate) {
  const int64_t elapsed_us = target_time_us - anchor_time_us;
  const uint64_t absolute_elapsed_us =
      elapsed_us >= 0 ? static_cast<uint64_t>(elapsed_us)
                      : static_cast<uint64_t>(-(elapsed_us + 1)) + 1;
  const uint64_t elapsed_ticks =
      (absolute_elapsed_us / kRtpTimestampMicrosecondsPerSecond) *
          clock_rate +
      ((absolute_elapsed_us % kRtpTimestampMicrosecondsPerSecond) *
       clock_rate) /
          kRtpTimestampMicrosecondsPerSecond;
  return elapsed_us >= 0
             ? anchor_rtp_timestamp + static_cast<uint32_t>(elapsed_ticks)
             : anchor_rtp_timestamp - static_cast<uint32_t>(elapsed_ticks);
}

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
    return ExtrapolateRtpTimestamp(base_timestamp_,
                                   *first_capture_time_us_, time_us,
                                   clock_rate_);
  }

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
  struct TimestampSample {
    uint32_t rtp_timestamp;
    int64_t media_time_us;
  };

  explicit RtpSampleTimestampGenerator(uint32_t base_timestamp)
      : base_timestamp_(base_timestamp) {}

  TimestampSample NextTimestamp(uint32_t samples_per_channel,
                                int64_t capture_time_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!first_capture_time_us_) {
      first_capture_time_us_ = capture_time_us;
    }

    const uint32_t timestamp =
        base_timestamp_ + static_cast<uint32_t>(sample_count_);
    const int64_t media_time_us =
        *first_capture_time_us_ +
        RtpTicksToMicroseconds(static_cast<int64_t>(sample_count_),
                              kOpusRtpClockRate);
    sample_count_ += samples_per_channel;
    return {timestamp, media_time_us};
  }

 private:
  static constexpr uint32_t kOpusRtpClockRate = 48'000;

  const uint32_t base_timestamp_;
  uint64_t sample_count_ = 0;
  std::optional<int64_t> first_capture_time_us_;
  std::mutex mutex_;
};

// Reconstructs a local monotonic media timeline from a remote RTP clock. Until
// a Sender Report arrives, the first complete frame is anchored to its local
// arrival time. An SR replaces that fallback with the sender's RTP-to-NTP
// mapping.
class RtpTimestampMapper {
 public:
  explicit RtpTimestampMapper(uint32_t clock_rate) : clock_rate_(clock_rate) {}

  int64_t ToLocalTimeUs(uint32_t rtp_timestamp, int64_t arrival_time_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t unwrapped_timestamp = unwrapper_.Unwrap(rtp_timestamp);
    if (sender_report_unwrapped_timestamp_) {
      const int64_t sender_report_delta =
          unwrapped_timestamp - *sender_report_unwrapped_timestamp_;
      return sender_report_local_time_us_ +
             RtpTicksToMicroseconds(sender_report_delta, clock_rate_);
    }
    if (!first_unwrapped_timestamp_) {
      first_unwrapped_timestamp_ = unwrapped_timestamp;
      first_local_time_us_ = arrival_time_us;
      return arrival_time_us;
    }

    const int64_t elapsed_ticks =
        unwrapped_timestamp - *first_unwrapped_timestamp_;
    return first_local_time_us_ +
           RtpTicksToMicroseconds(elapsed_ticks, clock_rate_);
  }

  void UpdateFromSenderReport(uint32_t rtp_timestamp,
                              int64_t sender_report_local_time_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    sender_report_unwrapped_timestamp_ = unwrapper_.Unwrap(rtp_timestamp);
    sender_report_local_time_us_ = sender_report_local_time_us;
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    unwrapper_.Reset();
    first_unwrapped_timestamp_.reset();
    first_local_time_us_ = 0;
    sender_report_unwrapped_timestamp_.reset();
    sender_report_local_time_us_ = 0;
  }

 private:
  class TimestampUnwrapper {
   public:
    int64_t Unwrap(uint32_t timestamp) {
      if (!last_timestamp_) {
        last_timestamp_ = timestamp;
        last_unwrapped_timestamp_ = timestamp;
        return last_unwrapped_timestamp_;
      }

      const uint32_t wrapped_delta = timestamp - *last_timestamp_;
      constexpr uint64_t kTimestampRange = uint64_t{1} << 32;
      const int64_t delta =
          wrapped_delta <= 0x7fffffff
              ? static_cast<int64_t>(wrapped_delta)
              : static_cast<int64_t>(wrapped_delta) -
                    static_cast<int64_t>(kTimestampRange);
      last_timestamp_ = timestamp;
      last_unwrapped_timestamp_ += delta;
      return last_unwrapped_timestamp_;
    }

    void Reset() {
      last_timestamp_.reset();
      last_unwrapped_timestamp_ = 0;
    }

   private:
    std::optional<uint32_t> last_timestamp_;
    int64_t last_unwrapped_timestamp_ = 0;
  };

  const uint32_t clock_rate_;
  TimestampUnwrapper unwrapper_;
  std::optional<int64_t> first_unwrapped_timestamp_;
  int64_t first_local_time_us_ = 0;
  std::optional<int64_t> sender_report_unwrapped_timestamp_;
  int64_t sender_report_local_time_us_ = 0;
  std::mutex mutex_;
};

}  // namespace minirtc

#endif