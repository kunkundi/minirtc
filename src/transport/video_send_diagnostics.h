/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-15
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_SEND_DIAGNOSTICS_H_
#define _VIDEO_SEND_DIAGNOSTICS_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace minirtc {

// Diagnostic only: bounded frame associations never delay admission or sends.
// All callers already hold the stream-context lock before this private mutex.
class VideoSendDiagnostics {
 public:
  struct Samples {
    uint64_t count = 0;
    int64_t total_ms = 0;
    int64_t max_ms = 0;
    void Add(int64_t ms) {
      if (ms < 0) return;
      ++count;
      total_ms += ms;
      max_ms = std::max(max_ms, ms);
    }
  };
  struct FrameTimes {
    uint64_t encoded = 0;
    Samples first_packet, last_packet, serialization;
  };
  struct Snapshot {
    int64_t interval_ms = 0;
    std::array<FrameTimes, 2> frames;  // delta, key
    uint64_t discarded = 0, expired = 0, untracked = 0;
    size_t pending = 0;
    uint64_t rejected = 0;
    // Observed span from first to last rejection, never extended through idle
    // capture time. An accepted admission or explicit suspension ends the run.
    int64_t reject_run_max_ms = 0, reject_ongoing_ms = 0;
    // Queue observations at rejected admissions: <80, 80–149, 150–299, >=300ms.
    std::array<uint64_t, 4> expected_queue{}, oldest_wait{};
    int64_t expected_queue_max_ms = 0, oldest_wait_max_ms = 0;
    bool HasActivity() const {
      return frames[0].encoded || frames[1].encoded ||
             frames[0].first_packet.count || frames[1].first_packet.count ||
             frames[0].last_packet.count || frames[1].last_packet.count ||
             discarded || expired || untracked || pending || rejected;
    }
  };

  void OnEncoded(int64_t capture_us, int64_t now_ms, bool keyframe) {
    std::lock_guard lock(mutex_);
    BeginWindow(now_ms);
    Expire(now_ms);
    ++window_.frames[keyframe ? 1 : 0].encoded;
    if (capture_us <= 0) {
      ++window_.untracked;
      return;
    }
    if (pending_.erase(capture_us)) ++window_.untracked;
    if (pending_.size() >= 256) {
      pending_.erase(pending_.begin());
      ++window_.untracked;
    }
    pending_.emplace(capture_us, Frame{now_ms, std::nullopt, keyframe});
  }

  // Call only after successful original-media sends, never for RTX/FEC.
  // The RTP marker identifies the last packet. A locally discarded packet
  // removes the association, so a partial frame cannot count as fully sent.
  void OnPacketSent(int64_t capture_us, int64_t now_ms, bool first, bool last) {
    std::lock_guard lock(mutex_);
    auto it = pending_.find(capture_us);
    if (it == pending_.end()) return;
    auto& frame = it->second;
    auto& timing = window_.frames[frame.keyframe ? 1 : 0];
    if (first && !frame.first_ms && now_ms >= frame.encoded_ms) {
      frame.first_ms = now_ms;
      timing.first_packet.Add(now_ms - frame.encoded_ms);
    }
    if (last) {
      if (frame.first_ms && now_ms >= *frame.first_ms) {
        timing.last_packet.Add(now_ms - frame.encoded_ms);
        timing.serialization.Add(now_ms - *frame.first_ms);
      } else {
        ++window_.untracked;
      }
      pending_.erase(it);
    }
  }

  void OnPacketDiscarded(int64_t capture_us) {
    std::lock_guard lock(mutex_);
    if (pending_.erase(capture_us)) ++window_.discarded;
  }

  void OnAdmission(int64_t now_ms, bool rejected, int64_t expected_ms,
                   int64_t oldest_ms) {
    std::lock_guard lock(mutex_);
    BeginWindow(now_ms);
    if (rejected) {
      if (!reject_started_ms_) reject_started_ms_ = now_ms;
      reject_last_ms_ = now_ms;
      window_.reject_run_max_ms = std::max(
          window_.reject_run_max_ms, now_ms - *reject_started_ms_);
      ++window_.rejected;
      ++window_.expected_queue[QueueBucket(expected_ms)];
      ++window_.oldest_wait[QueueBucket(oldest_ms)];
      window_.expected_queue_max_ms =
          std::max(window_.expected_queue_max_ms, expected_ms);
      window_.oldest_wait_max_ms =
          std::max(window_.oldest_wait_max_ms, oldest_ms);
    } else if (reject_started_ms_) {
      reject_started_ms_.reset();
    }
  }

  void EndAdmissionRun() {
    std::lock_guard lock(mutex_);
    reject_started_ms_.reset();
  }

  Snapshot Take(int64_t now_ms) {
    std::lock_guard lock(mutex_);
    Expire(now_ms);
    window_.interval_ms = window_started_ms_
        ? std::max<int64_t>(0, now_ms - *window_started_ms_) : 0;
    if (reject_started_ms_) {
      window_.reject_ongoing_ms =
          std::max<int64_t>(0, reject_last_ms_ - *reject_started_ms_);
      window_.reject_run_max_ms = std::max(
          window_.reject_run_max_ms, window_.reject_ongoing_ms);
    }
    window_.pending = pending_.size();
    auto result = window_;
    window_ = {};
    window_started_ms_ = now_ms;
    return result;
  }

  void Reset() {
    std::lock_guard lock(mutex_);
    window_ = {};
    pending_.clear();
    reject_started_ms_.reset();
    window_started_ms_.reset();
  }

 private:
  struct Frame {
    int64_t encoded_ms;
    std::optional<int64_t> first_ms;
    bool keyframe;
  };
  static size_t QueueBucket(int64_t ms) {
    return ms < 80 ? 0 : ms < 150 ? 1 : ms < 300 ? 2 : 3;
  }
  void BeginWindow(int64_t now_ms) {
    if (!window_started_ms_) window_started_ms_ = now_ms;
  }
  void Expire(int64_t now_ms) {
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (now_ms - it->second.encoded_ms >= 5000) {
        it = pending_.erase(it);
        ++window_.expired;
      } else {
        ++it;
      }
    }
  }

  std::mutex mutex_;
  std::map<int64_t, Frame> pending_;
  std::optional<int64_t> window_started_ms_, reject_started_ms_;
  int64_t reject_last_ms_ = 0;
  Snapshot window_;
};

}  // namespace minirtc

#endif
