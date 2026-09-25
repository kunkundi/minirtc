/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-26
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _REMOTE_CAPTURE_TIME_ESTIMATOR_H_
#define _REMOTE_CAPTURE_TIME_ESTIMATOR_H_

#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace minirtc {

// Maps remote RTP capture timestamps to the local monotonic clock. Clock
// offset comes from a matched RTT exchange, assuming symmetric paths, rather
// than SR arrival times whose independent queueing would hide video delay.
// Unknown/stale mappings never masquerade as zero-latency arrival timestamps.
class RemoteCaptureTimeEstimator {
 public:
  void UpdateClockOffset(int64_t offset_us, int64_t rtt_us, int64_t now_us) {
    if (rtt_us < 0 || rtt_us > 2'000'000) return;
    std::lock_guard lock(mutex_);
    if (!offsets_.empty() && now_us < offsets_.back().time_us)
      offsets_.clear();
    offsets_.push_back({offset_us, rtt_us, now_us});
    while (offsets_.size() > 8 ||
           now_us - offsets_.front().time_us > kMaxAgeUs)
      offsets_.pop_front();
  }

  void UpdateSenderReport(uint32_t rtp, int64_t remote_us, int64_t arrival_us) {
    std::lock_guard lock(mutex_);
    if (!samples_.empty()) {
      const auto& last = samples_.back();
      if (rtp == last.rtp || remote_us == last.remote_us) return;
      const int64_t delta = RtpDelta(rtp, last.rtp);
      const int64_t elapsed = remote_us - last.remote_us;
      // Reset on stream/clock discontinuity rather than fitting across it.
      if (delta <= 0 || elapsed <= 0 || elapsed > kMaxAgeUs ||
          std::abs(elapsed - delta * (1'000'000.0 / 90'000)) > 100'000) {
        samples_.clear();
        // A long gap alone doesn't change the peer's clock. Discard the
        // calibration too if the sender's timelines no longer agree.
        if (elapsed <= 0 ||
            std::abs(elapsed - delta * (1'000'000.0 / 90'000)) > 100'000)
          offsets_.clear();
      }
    }
    samples_.push_back({rtp, remote_us, arrival_us});
    if (samples_.size() > 20) samples_.pop_front();
  }

  std::optional<int64_t> Estimate(uint32_t rtp, int64_t now_us) const {
    std::lock_guard lock(mutex_);
    if (samples_.size() < 3 ||
        now_us < samples_.back().arrival_us ||
        now_us - samples_.back().arrival_us > kMaxAgeUs)
      return std::nullopt;
    // The least queued recent exchange gives the most reliable offset. Prefer
    // the newer sample for ties so calibration follows slow clock drift.
    const Offset* best = nullptr;
    for (const auto& offset : offsets_) {
      if (now_us < offset.time_us || now_us - offset.time_us > kMaxAgeUs)
        continue;
      if (!best || offset.rtt_us <= best->rtt_us) best = &offset;
    }
    if (!best) return std::nullopt;
    const auto& anchor = samples_.back();
    // Center both axes before fitting to preserve precision for UTC values.
    double x_mean = 0, y_mean = 0;
    for (const auto& s : samples_) {
      x_mean += RtpDelta(s.rtp, anchor.rtp);
      y_mean += s.remote_us - anchor.remote_us;
    }
    x_mean /= samples_.size();
    y_mean /= samples_.size();
    double variance = 0, covariance = 0;
    for (const auto& s : samples_) {
      const double x = RtpDelta(s.rtp, anchor.rtp) - x_mean;
      variance += x * x;
      covariance += x * (s.remote_us - anchor.remote_us - y_mean);
    }
    if (variance == 0) return std::nullopt;
    const double slope = covariance / variance;
    // Video RTP uses 90 kHz. Reject malformed reports and implausible drift.
    if (slope < 11.0 || slope > 11.23) return std::nullopt;
    const int64_t mapped =
        anchor.remote_us + best->offset_us +
        static_cast<int64_t>(std::llround(
            y_mean + slope * (RtpDelta(rtp, anchor.rtp) - x_mean)));
    if (mapped <= 0 || mapped > now_us || now_us - mapped > kMaxAgeUs)
      return std::nullopt;
    return mapped;
  }

  void Reset() {
    std::lock_guard lock(mutex_);
    samples_.clear();
    offsets_.clear();
  }

  void ResetClockOffset() {
    std::lock_guard lock(mutex_);
    offsets_.clear();
  }

 private:
  static int64_t RtpDelta(uint32_t a, uint32_t b) {
    const uint32_t delta = a - b;
    return delta <= 0x7fffffff ? delta : int64_t(delta) - (int64_t{1} << 32);
  }
  static constexpr int64_t kMaxAgeUs = 5'000'000;
  struct Sample {
    uint32_t rtp;
    int64_t remote_us;
    int64_t arrival_us;
  };
  struct Offset {
    int64_t offset_us;
    int64_t rtt_us;
    int64_t time_us;
  };
  mutable std::mutex mutex_;
  std::deque<Sample> samples_;
  std::deque<Offset> offsets_;
};
}  // namespace minirtc

#endif
