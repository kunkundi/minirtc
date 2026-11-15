/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-15
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ENCODER_INPUT_TIMING_H_
#define _ENCODER_INPUT_TIMING_H_

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>

namespace minirtc {

// Observe frames that actually reach Encode(), after all admission drops. The
// configured ceiling remains independent of this hint, so a lower measurement
// never prevents the encoder from returning to the requested frame rate.
class EncoderInputCadence {
public:
  std::optional<int> Observe(int64_t now_ms, int max_frame_rate) {
    const int ceiling = std::clamp(max_frame_rate, 1, 240);
    if (times_.empty() || now_ms < times_.back() ||
        now_ms - times_.back() > 2000 || ceiling != ceiling_) {
      Reset();
      ceiling_ = ceiling;
      times_.push_back(now_ms);
      last_update_ms_ = now_ms;
      return ceiling;
    }
    times_.push_back(now_ms);
    while (times_.size() > 2 &&
           (now_ms - times_.front() > 1000 || times_.size() > 241))
      times_.pop_front();
    if (now_ms - last_update_ms_ < 1000 || times_.size() < 2)
      return std::nullopt;
    last_update_ms_ = now_ms;
    const int64_t span = now_ms - times_.front();
    if (span <= 0)
      return std::nullopt;
    const int fps = std::clamp<int64_t>(
        ((times_.size() - 1) * 1000 + span / 2) / span, 1, ceiling);
    // Republish stable measurements too, so a transient codec failure does
    // not permanently suppress the same hint on later frames.
    return fps;
  }

  void Reset() {
    times_.clear();
    last_update_ms_ = -1;
    ceiling_ = 0;
  }

private:
  std::deque<int64_t> times_;
  int64_t last_update_ms_ = -1;
  int ceiling_ = 0;
};

// Media Foundation sample times use 100 ns units. Preserve elapsed capture
// time across dropped inputs and idle periods instead of compressing every
// submitted frame into one nominal 1/max_fps slot.
class EncoderSampleClock {
public:
  struct Sample {
    int64_t time_100ns;
    int64_t duration_100ns;
  };

  Sample Next(int64_t capture_us, int64_t fallback_us, int input_frame_rate) {
    const int64_t duration = 10000000 / std::clamp(input_frame_rate, 1, 240);
    const bool has_capture = capture_us > 0;
    const int64_t source_us = capture_us > 0 ? capture_us : fallback_us;
    if (last_source_us_) {
      // A missing/backward/duplicate source timestamp must not make an MFT's
      // sample timeline regress. Rebase to the new clock after one frame.
      time_100ns_ +=
          has_capture == last_had_capture_ && source_us > *last_source_us_
              ? (source_us - *last_source_us_) * 10
              : last_duration_100ns_;
    }
    last_source_us_ = source_us;
    last_had_capture_ = has_capture;
    last_duration_100ns_ = duration;
    return {time_100ns_, duration};
  }

  void Reset() {
    last_source_us_.reset();
    time_100ns_ = 0;
    last_duration_100ns_ = 0;
    last_had_capture_ = false;
  }

private:
  std::optional<int64_t> last_source_us_;
  int64_t time_100ns_ = 0;
  int64_t last_duration_100ns_ = 0;
  bool last_had_capture_ = false;
};

} // namespace minirtc

#endif
