/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-15
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_ENCODER_RATE_CONTROL_H_
#define _VIDEO_ENCODER_RATE_CONTROL_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "frame_dropper.h"

namespace minirtc {

// Accessed under the stream-senders lock, including asynchronous encoder output.
class VideoEncoderRateControl {
 public:
  int SetRates(int media_bitrate_bps, int min_bitrate_bps,
               double cwnd_reduce_ratio) {
    target_bitrate_bps_ = std::max(0, media_bitrate_bps);
    cwnd_drop_interval_ = 0;
    const int min_bitrate = std::max(0, min_bitrate_bps);
    if (cwnd_reduce_ratio > 0.01 && target_bitrate_bps_ > min_bitrate) {
      const int reduction = std::min(
          static_cast<int>(target_bitrate_bps_ *
                           std::clamp(cwnd_reduce_ratio, 0.0, 1.0)),
          target_bitrate_bps_ - min_bitrate);
      if (reduction > 0) {
        // Match WebRTC's separate pushback stage: at most every second frame,
        // with the same reduction published to the encoder and overshoot guard.
        cwnd_drop_interval_ = std::max(2, target_bitrate_bps_ / reduction);
        target_bitrate_bps_ -= target_bitrate_bps_ / cwnd_drop_interval_;
      }
    }
    return target_bitrate_bps_;
  }

  bool DropFrame(int64_t now_us, int max_frame_rate, bool keyframe_pending) {
    if (last_capture_us_ < 0 || now_us < last_capture_us_ ||
        now_us - last_capture_us_ > 1000000) {
      Reset();
    }
    last_capture_us_ = now_us;
    if (cwnd_drop_interval_ > 0 &&
        cwnd_frame_counter_++ % cwnd_drop_interval_ == 0 && !keyframe_pending) {
      ++cwnd_dropped_frames_;
      return true;
    }

    const int64_t elapsed = now_us - last_dropper_input_us_;
    const int input_fps = last_dropper_input_us_ >= 0 && elapsed > 0
                              ? std::clamp<int64_t>(1000000 / elapsed, 1,
                                                    max_frame_rate)
                              : max_frame_rate;
    last_dropper_input_us_ = now_us;
    frame_dropper_.SetRates(target_bitrate_bps_ / 1000.0, input_fps);
    frame_dropper_.Leak(input_fps);
    if (frame_dropper_.DropFrame() && !keyframe_pending) {
      ++rate_dropped_frames_;
      return true;
    }
    return false;
  }

  void Fill(size_t bytes, bool keyframe) { frame_dropper_.Fill(bytes, !keyframe); }

  void Reset() {
    frame_dropper_.Reset();
    last_capture_us_ = -1;
    last_dropper_input_us_ = -1;
    cwnd_frame_counter_ = 0;
  }

  int target_bitrate_bps() const { return target_bitrate_bps_; }
  int cwnd_drop_interval() const { return cwnd_drop_interval_; }
  uint64_t rate_dropped_frames() const { return rate_dropped_frames_; }
  uint64_t cwnd_dropped_frames() const { return cwnd_dropped_frames_; }

 private:
  webrtc::FrameDropper frame_dropper_;
  int target_bitrate_bps_ = 0;
  int cwnd_drop_interval_ = 0;
  uint64_t cwnd_frame_counter_ = 0;
  int64_t last_capture_us_ = -1;
  int64_t last_dropper_input_us_ = -1;
  uint64_t rate_dropped_frames_ = 0;
  uint64_t cwnd_dropped_frames_ = 0;
};

}  // namespace minirtc

#endif
