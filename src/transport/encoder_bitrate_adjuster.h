/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-15
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ENCODER_BITRATE_ADJUSTER_H_
#define _ENCODER_BITRATE_ADJUSTER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace minirtc {

// Single-stream adaptation of WebRTC's EncoderOvershootDetector and
// EncoderBitrateAdjuster (2e61fea6e375). The virtual queue charges sustained
// overshoot once; an isolated large frame can drain without reducing the rate.
// Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
// Upstream portions are governed by the BSD-style license in
// thirdparty/webrtc/LICENSE and the additional rights grant in PATENTS.
// No simulcast or optional network-headroom policy.
class EncoderBitrateAdjuster {
 public:
  void SetTargetRate(int bps, int fps, int64_t now_ms) {
    Leak(now_ms);
    target_bps_ = std::max(0, bps);
    fps_ = std::max(1, fps);
  }

  void SetFrameCadence(int fps, int cwnd_drop_interval) {
    fps_ = std::max(1, fps);
    allow_frame_credit_ = cwnd_drop_interval > 0;
    if (allow_frame_credit_)
      fps_ = std::max(1, fps_ * (cwnd_drop_interval - 1) / cwnd_drop_interval);
    else
      buffer_bits_ = std::max(0.0, buffer_bits_);
  }

  void OnEncodedFrame(size_t bytes, bool keyframe, int64_t now_ms) {
    Leak(now_ms);
    const double ideal_bits = double(target_bps_) / fps_;
    double factor = 1;
    if (ideal_bits > 0) {
      const double bits = double(bytes) * 8;
      const double excess = std::max(
          0.0, std::min(buffer_bits_,
                        std::max(0.0, buffer_bits_ + bits - ideal_bits)));
      // Do not interpret the initial IDR as sustained encoder overshoot.
      factor = samples_.empty() && !keyframe ? std::max(1.0, bits / ideal_bits)
                                             : 1.0 + excess / ideal_bits;
      buffer_bits_ += bits - excess;
    }
    samples_.push_back({now_ms, bytes, factor});
    if (keyframe) last_keyframe_bytes_ = bytes;
    Cull(now_ms);
  }

  void OnRateDroppedFrame(int64_t now_ms) {
    // Rate-control drops must not hide encoder overshoot by providing extra
    // virtual credit. Pacer admission drops, by contrast, let its queue drain.
    last_update_ms_ = std::max(last_update_ms_, now_ms);
  }

  double Utilization(int64_t now_ms) {
    Cull(now_ms);
    if (samples_.size() < 30 || now_ms - samples_.front().time_ms < 500)
      return 1;
    double sum = 0;
    for (const auto& sample : samples_) sum += sample.factor;
    return std::clamp(sum / samples_.size(), 1.0, 2.0);
  }

  int OutputBitrate(int64_t now_ms) const {
    int64_t bytes = 0;
    for (const auto& sample : samples_)
      if (now_ms - sample.time_ms < 1000) bytes += sample.bytes;
    return static_cast<int>(std::min<int64_t>(bytes * 8, 1000000000));
  }
  size_t last_keyframe_bytes() const { return last_keyframe_bytes_; }

  void Reset() {
    samples_.clear();
    buffer_bits_ = 0;
    last_update_ms_ = -1;
    last_keyframe_bytes_ = 0;
  }

 private:
  void Leak(int64_t now_ms) {
    if (last_update_ms_ >= 0 && now_ms < last_update_ms_) Reset();
    if (last_update_ms_ >= 0)
      buffer_bits_ = std::max(
          allow_frame_credit_ ? -double(target_bps_) * std::min(5, fps_) / fps_
                              : 0.0,
          buffer_bits_ -
              double(target_bps_) * (now_ms - last_update_ms_) / 1000);
    last_update_ms_ = now_ms;
  }
  void Cull(int64_t now_ms) {
    while (!samples_.empty() && now_ms - samples_.front().time_ms > 3000)
      samples_.pop_front();
  }
  struct Sample {
    int64_t time_ms;
    size_t bytes;
    double factor;
  };
  std::deque<Sample> samples_;
  double buffer_bits_ = 0;
  int target_bps_ = 0, fps_ = 60;
  // WebRTC's bounded media-buffer credit prevents intentional CWND gaps from
  // making an otherwise on-budget encoder look like a persistent overshooter.
  bool allow_frame_credit_ = false;
  int64_t last_update_ms_ = -1;
  size_t last_keyframe_bytes_ = 0;
};

}  // namespace minirtc

#endif
