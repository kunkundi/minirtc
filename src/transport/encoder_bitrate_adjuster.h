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
#include <iterator>
#include <optional>

namespace minirtc {

// Single-stream adaptation of WebRTC's EncoderOvershootDetector and
// RateUtilizationTracker (2e61fea6e375). Desktop policy corrects sustained
// media overshoot; the bounded pacer handles burst pressure independently.
// Unlike upstream's optional headroom allocation, a burst factor alone never
// permanently reduces the codec's average target.
// Copyright (c) 2019, 2024 The WebRTC project authors. All Rights Reserved.
// Upstream portions are governed by the BSD-style license in
// thirdparty/webrtc/LICENSE and the additional rights grant in PATENTS.
class EncoderBitrateAdjuster {
 public:
  void SetTargetRate(int bps, int fps, int64_t now_ms) {
    Advance(now_ms);
    target_bps_ = std::max(0, bps);
    configured_fps_ = std::max(1, fps);
    UpdateFrameCadence(now_ms);
  }

  void SetFrameCadence(int fps, int cwnd_drop_interval) {
    configured_fps_ = std::max(1, fps);
    cwnd_drop_interval_ = cwnd_drop_interval;
    UpdateFrameCadence(last_update_ms_);
  }

  void OnEncoderInput(int64_t now_ms) {
    if (!input_times_.empty() && now_ms < input_times_.back()) Reset();
    input_times_.push_back(now_ms);
    while (!input_times_.empty() &&
           (now_ms - input_times_.front() > 1000 || input_times_.size() > 240))
      input_times_.pop_front();
    UpdateFrameCadence(now_ms);
  }

  // Only FrameDropper drops remove budget. Otherwise the guard could hide a
  // persistent 2x encoder overshoot by dropping half its inputs. Intentional
  // CWND gaps and external admission gaps retain their elapsed-time budget.
  void OnFrameOpportunity(int64_t now_ms, bool rate_dropped) {
    Advance(now_ms);
    if (rate_dropped && last_opportunity_ms_ >= 0 &&
        now_ms >= last_opportunity_ms_ &&
        now_ms - last_opportunity_ms_ <= 1000) {
      rate_drops_.push_back(
          {now_ms, last_opportunity_budget_bits_, allocated_bits_});
    }
    last_opportunity_ms_ = now_ms;
    last_opportunity_budget_bits_ = allocated_bits_;
    Cull(now_ms);
  }

  void OnEncodedFrame(size_t bytes, bool keyframe, int64_t now_ms) {
    Advance(now_ms);
    const double ideal_bits = double(target_bps_) / fps_;
    double factor = 1;
    if (ideal_bits > 0) {
      const double bits = double(bytes) * 8;
      const double excess =
          std::min(network_buffer_bits_,
                   std::max(0.0, network_buffer_bits_ + bits - ideal_bits));
      factor = samples_.empty() && !keyframe ? std::max(1.0, bits / ideal_bits)
                                             : 1.0 + excess / ideal_bits;
      network_buffer_bits_ += bits - excess;
    }
    samples_.push_back({now_ms, bytes, factor, allocated_bits_, keyframe});
    if (keyframe) last_keyframe_bytes_ = bytes;
    Cull(now_ms);
  }

  double Utilization(int64_t now_ms) {
    Advance(now_ms);
    Cull(now_ms);
    if (samples_.size() < 3 || now_ms - samples_.front().time_ms < 500)
      return 1;
    double produced_bits = 0;
    for (const auto& sample : samples_)
      produced_bits += double(sample.bytes) * 8;
    // As in RateUtilizationTracker, allow the final produced frame enough
    // budget to be sent. A single large frame is not evidence of overshoot.
    const auto& last = samples_.back();
    double last_bits = 0;
    for (auto sample = samples_.rbegin();
         sample != samples_.rend() && sample->time_ms == last.time_ms; ++sample)
      last_bits += double(sample->bytes) * 8;
    const double budget =
        BudgetSince(samples_.front().allocated_bits) +
        std::max(0.0, last_bits - BudgetSince(last.allocated_bits));
    return budget > 0 ? std::clamp(produced_bits / budget, 1.0, 2.0) : 1;
  }

  double NetworkUtilization(int64_t now_ms) {
    Cull(now_ms);
    if (samples_.empty()) return 1;
    double sum = 0;
    for (const auto& sample : samples_) sum += sample.network_factor;
    return std::max(1.0, sum / samples_.size());
  }

  int InputFrameRate(int64_t now_ms) const {
    const auto first = std::lower_bound(input_times_.begin(),
                                        input_times_.end(), now_ms - 1000);
    const auto count = std::distance(first, input_times_.end());
    if (count < 2 || now_ms < input_times_.back()) return 0;
    const int64_t span_ms = now_ms - *first;
    return span_ms > 0 ? std::min<int64_t>(configured_fps_,
                                           (count - 1) * 1000 / span_ms)
                       : configured_fps_;
  }
  int OutputBitrate(int64_t now_ms) const {
    int64_t bytes = 0;
    for (const auto& sample : samples_)
      if (now_ms - sample.time_ms < 1000) bytes += sample.bytes;
    return static_cast<int>(std::min<int64_t>(bytes * 8, 1000000000));
  }
  size_t MaxFrameBytes(int64_t now_ms,
                      std::optional<bool> keyframe = std::nullopt) const {
    size_t bytes = 0;
    for (const auto& sample : samples_)
      if (now_ms - sample.time_ms < 1000 &&
          (!keyframe || sample.keyframe == *keyframe))
        bytes = std::max(bytes, sample.bytes);
    return bytes;
  }
  size_t last_keyframe_bytes() const { return last_keyframe_bytes_; }

  void Reset() {
    samples_.clear();
    rate_drops_.clear();
    input_times_.clear();
    network_buffer_bits_ = 0;
    allocated_bits_ = 0;
    last_opportunity_budget_bits_ = 0;
    last_update_ms_ = last_opportunity_ms_ = -1;
    last_keyframe_bytes_ = 0;
  }

 private:
  double BudgetSince(double start_bits) const {
    double budget = allocated_bits_ - start_bits;
    // Keep gross allocation monotone. Encoder callbacks may land inside a
    // subsequently dropped interval, so subtract only its overlapping budget.
    for (const auto& drop : rate_drops_)
      budget -=
          std::max(0.0, drop.end_bits - std::max(drop.start_bits, start_bits));
    return std::max(0.0, budget);
  }
  void Advance(int64_t now_ms) {
    if (last_update_ms_ >= 0 && now_ms < last_update_ms_) Reset();
    if (last_update_ms_ >= 0) {
      const double bits =
          double(target_bps_) * (now_ms - last_update_ms_) / 1000;
      allocated_bits_ += bits;
      network_buffer_bits_ = std::max(0.0, network_buffer_bits_ - bits);
    }
    last_update_ms_ = now_ms;
  }
  void UpdateFrameCadence(int64_t now_ms) {
    const int measured_fps = InputFrameRate(now_ms);
    fps_ = measured_fps > 0 ? measured_fps : configured_fps_;
    if (measured_fps == 0 && cwnd_drop_interval_ > 0)
      fps_ =
          std::max(1, fps_ * (cwnd_drop_interval_ - 1) / cwnd_drop_interval_);
  }
  void Cull(int64_t now_ms) {
    // Retain the full three-second window even at the supported 240 fps.
    while (!samples_.empty() &&
           (now_ms - samples_.front().time_ms > 3000 || samples_.size() > 720))
      samples_.pop_front();
    while (!rate_drops_.empty() &&
           (now_ms - rate_drops_.front().time_ms > 3000 ||
            rate_drops_.size() > 720))
      rate_drops_.pop_front();
  }
  struct Sample {
    int64_t time_ms;
    size_t bytes;
    double network_factor;
    double allocated_bits;
    bool keyframe;
  };
  struct DroppedOpportunity {
    int64_t time_ms;
    double start_bits;
    double end_bits;
  };
  std::deque<Sample> samples_;
  std::deque<DroppedOpportunity> rate_drops_;
  std::deque<int64_t> input_times_;
  double network_buffer_bits_ = 0, allocated_bits_ = 0;
  double last_opportunity_budget_bits_ = 0;
  int target_bps_ = 0, configured_fps_ = 60, fps_ = 60;
  int cwnd_drop_interval_ = 0;
  int64_t last_update_ms_ = -1, last_opportunity_ms_ = -1;
  size_t last_keyframe_bytes_ = 0;
};

}  // namespace minirtc

#endif
