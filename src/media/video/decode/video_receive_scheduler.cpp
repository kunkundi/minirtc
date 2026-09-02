#include "video_receive_scheduler.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

#include "rtp_defines.h"

namespace minirtc {
namespace {

constexpr int64_t kMinimumFrameIntervalUs = 5000;
constexpr int64_t kMaximumFrameIntervalUs = 100000;
constexpr int64_t kMaximumTimingSampleGapUs = 1000000;
constexpr int64_t kMinimumSevereLagUs = 150000;
constexpr int64_t kKeyFrameRequestCooldownUs = 500000;
constexpr size_t kExtraFramesBeforeCatchUp = 6;

int64_t EstimateFrameSteps(int64_t delta_us, int64_t frame_interval_us) {
  if (delta_us <= 0 || frame_interval_us <= 0) {
    return 1;
  }
  return std::clamp((delta_us + frame_interval_us / 2) /
                        frame_interval_us,
                    int64_t{1}, int64_t{120});
}

} // namespace

VideoReceiveScheduler::VideoReceiveScheduler(int nominal_frame_rate) {
  const int frame_rate = std::clamp(nominal_frame_rate, 10, 120);
  nominal_frame_interval_us_ = 1000000 / frame_rate;
  estimated_frame_interval_us_ = nominal_frame_interval_us_;
}

int64_t VideoReceiveScheduler::MediaTimestampUs(const ReceivedFrame &frame) {
  if (frame.HasRtpTimestamp()) {
    const int64_t unwrapped_timestamp =
        rtp_timestamp_unwrapper_.Unwrap(frame.RtpTimestamp());
    return unwrapped_timestamp * 1000000LL / rtp::kVideoPayloadTypeFrequency;
  }
  if (frame.CapturedTimestamp() != 0) {
    return frame.CapturedTimestamp();
  }
  return frame.ReceivedTimestamp();
}

void VideoReceiveScheduler::UpdateTimingEstimate(int64_t media_timestamp_us,
                                                 int64_t arrival_timestamp_us) {
  if (!timing_sample_initialized_) {
    timing_sample_initialized_ = true;
    last_observed_media_timestamp_us_ = media_timestamp_us;
    last_observed_arrival_timestamp_us_ = arrival_timestamp_us;
    return;
  }

  const int64_t media_delta_us =
      media_timestamp_us - last_observed_media_timestamp_us_;
  const int64_t arrival_delta_us =
      arrival_timestamp_us - last_observed_arrival_timestamp_us_;
  if (media_delta_us <= 0) {
    ++timestamp_fallbacks_;
    return;
  }

  const bool media_delta_valid =
      media_delta_us <= kMaximumTimingSampleGapUs;
  const bool arrival_delta_valid =
      arrival_delta_us > 0 &&
      arrival_delta_us <= kMaximumTimingSampleGapUs;
  if (!media_delta_valid && !arrival_delta_valid) {
    ++timestamp_fallbacks_;
    last_observed_media_timestamp_us_ = media_timestamp_us;
    last_observed_arrival_timestamp_us_ = arrival_timestamp_us;
    return;
  }

  const int64_t expected_interval_us = std::clamp(
      estimated_frame_interval_us_, kMinimumFrameIntervalUs,
      kMaximumFrameIntervalUs);
  const int64_t media_steps =
      media_delta_valid
          ? EstimateFrameSteps(media_delta_us, expected_interval_us)
          : int64_t{1};
  const int64_t arrival_steps =
      arrival_delta_valid
          ? EstimateFrameSteps(arrival_delta_us, expected_interval_us)
          : media_steps;

  // A legacy sender used truncated microseconds directly as the RTP timestamp.
  // At 60 fps that looks like eleven missing 90 kHz frames for every actual
  // frame. Detect a disagreement between media time and arrival cadence and
  // use arrival cadence only for the estimator. RTP time remains the ordering
  // key, so this fallback cannot reorder frames.
  const bool timestamp_scale_mismatch =
      arrival_delta_valid && media_delta_valid &&
      (media_steps > arrival_steps * 3 || arrival_steps > media_steps * 3);
  const bool use_arrival_fallback =
      arrival_delta_valid && (!media_delta_valid || timestamp_scale_mismatch);
  if (use_arrival_fallback) {
    ++timestamp_fallbacks_;
  }

  const int64_t sample_steps =
      use_arrival_fallback ? arrival_steps : media_steps;
  const int64_t sample_delta_us =
      use_arrival_fallback ? arrival_delta_us : media_delta_us;
  const int64_t bounded_interval_us =
      std::clamp(sample_delta_us / std::max<int64_t>(1, sample_steps),
                 kMinimumFrameIntervalUs, kMaximumFrameIntervalUs);
  estimated_frame_interval_us_ =
      (estimated_frame_interval_us_ * 7 + bounded_interval_us) / 8;

  if (arrival_delta_valid) {
    const int64_t arrival_interval_us =
        arrival_delta_us / std::max<int64_t>(1, sample_steps);
    const int64_t transit_variation_us =
        std::llabs(arrival_interval_us - bounded_interval_us);
    estimated_jitter_us_ =
        (estimated_jitter_us_ * 15 + transit_variation_us) / 16;
  }

  int desired_target = 1;
  if (estimated_jitter_us_ * 4 >= estimated_frame_interval_us_ * 3) {
    desired_target = 3;
  } else if (estimated_jitter_us_ * 4 >= estimated_frame_interval_us_) {
    desired_target = 2;
  }
  UpdateTargetBufferFrames(desired_target);

  last_observed_media_timestamp_us_ = media_timestamp_us;
  last_observed_arrival_timestamp_us_ = arrival_timestamp_us;
}

void VideoReceiveScheduler::UpdateTargetBufferFrames(int desired_target) {
  desired_target = std::clamp(desired_target, 1, 3);
  if (desired_target > target_buffer_frames_) {
    target_buffer_frames_ = desired_target;
    target_decrease_samples_ = 0;
    return;
  }

  if (desired_target == target_buffer_frames_) {
    target_decrease_samples_ = 0;
    return;
  }

  // Increase latency quickly when jitter rises, but require roughly one
  // second of stable arrivals before removing a buffered frame.
  if (++target_decrease_samples_ < 60) {
    return;
  }
  --target_buffer_frames_;
  target_decrease_samples_ = 0;
}

void VideoReceiveScheduler::Enqueue(std::unique_ptr<ReceivedFrame> frame,
                                    int64_t now_us) {
  if (!frame) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const int64_t media_timestamp_us = MediaTimestampUs(*frame);
  const int64_t arrival_timestamp_us =
      frame->ReceivedTimestamp() != 0 ? frame->ReceivedTimestamp() : now_us;
  const bool is_key_frame = frame->IsKeyFrame();

  ++enqueued_frames_;
  UpdateTimingEstimate(media_timestamp_us, arrival_timestamp_us);

  if (awaiting_key_frame_) {
    if (!is_key_frame) {
      ++dropped_frames_;
      return;
    }
    dropped_frames_ += frames_.size();
    frames_.clear();
    awaiting_key_frame_ = false;
    playout_started_ = false;
    last_release_local_timestamp_us_ = 0;
    last_released_media_timestamp_us_ = 0;
  }

  if (last_released_media_timestamp_us_ != 0 &&
      media_timestamp_us <= last_released_media_timestamp_us_) {
    ++dropped_frames_;
    return;
  }

  ScheduledFrame scheduled_frame{std::move(frame), media_timestamp_us,
                                 arrival_timestamp_us, is_key_frame};
  const auto insert_at =
      std::upper_bound(frames_.begin(), frames_.end(), media_timestamp_us,
                       [](int64_t timestamp, const ScheduledFrame &queued) {
                         return timestamp < queued.media_timestamp_us;
                       });
  frames_.insert(insert_at, std::move(scheduled_frame));
  max_buffered_frames_ = std::max(max_buffered_frames_, frames_.size());
}

bool VideoReceiveScheduler::IsSeverelyBehind(int64_t now_us) const {
  if (frames_.empty()) {
    return false;
  }
  const int64_t oldest_age_us =
      std::max<int64_t>(0, now_us - frames_.front().arrival_timestamp_us);
  const int64_t severe_lag_us = std::max(
      kMinimumSevereLagUs, estimated_frame_interval_us_ *
                               static_cast<int64_t>(target_buffer_frames_ + 5));
  if (oldest_age_us > severe_lag_us) {
    return true;
  }
  return frames_.size() >
             static_cast<size_t>(target_buffer_frames_) +
                 kExtraFramesBeforeCatchUp &&
         oldest_age_us >
             estimated_frame_interval_us_ * target_buffer_frames_;
}

size_t VideoReceiveScheduler::DropBeforeNewestKeyFrame() {
  const auto reverse_key_frame = std::find_if(
      frames_.rbegin(), frames_.rend(),
      [](const ScheduledFrame &frame) { return frame.is_key_frame; });
  if (reverse_key_frame == frames_.rend()) {
    return 0;
  }

  const auto key_frame = std::prev(reverse_key_frame.base());
  const size_t dropped =
      static_cast<size_t>(std::distance(frames_.begin(), key_frame));
  frames_.erase(frames_.begin(), key_frame);
  return dropped;
}

VideoReceiveScheduler::PollResult
VideoReceiveScheduler::Poll(int64_t now_us, bool decoder_busy) {
  std::lock_guard<std::mutex> lock(mutex_);
  PollResult result;
  if (frames_.empty() || decoder_busy) {
    return result;
  }

  if (IsSeverelyBehind(now_us)) {
    const size_t dropped_before_key_frame = DropBeforeNewestKeyFrame();
    if (dropped_before_key_frame > 0 ||
        (!frames_.empty() && frames_.front().is_key_frame)) {
      result.dropped_frames += dropped_before_key_frame;
      dropped_frames_ += dropped_before_key_frame;
      ++catch_up_count_;
      playout_started_ = false;
      last_release_local_timestamp_us_ = 0;
      last_released_media_timestamp_us_ = 0;
    } else {
      const bool key_frame_request_allowed =
          last_key_frame_request_us_ == 0 ||
          now_us - last_key_frame_request_us_ >=
              kKeyFrameRequestCooldownUs;
      if (key_frame_request_allowed) {
        result.dropped_frames = frames_.size();
        dropped_frames_ += frames_.size();
        frames_.clear();
        awaiting_key_frame_ = true;
        playout_started_ = false;
        ++catch_up_count_;
        ++key_frame_requests_;
        last_key_frame_request_us_ = now_us;
        result.request_key_frame = true;
        return result;
      }
    }
  }

  if (!playout_started_) {
    const int64_t target_buffer_time_us =
        estimated_frame_interval_us_ * target_buffer_frames_;
    const int64_t oldest_age_us =
        std::max<int64_t>(0, now_us - frames_.front().arrival_timestamp_us);
    if (frames_.size() <= static_cast<size_t>(target_buffer_frames_) &&
        oldest_age_us < target_buffer_time_us) {
      result.next_delay_us = target_buffer_time_us - oldest_age_us;
      return result;
    }
    playout_started_ = true;
  }

  const int64_t release_time_us =
      last_release_local_timestamp_us_ != 0
          ? last_release_local_timestamp_us_ + estimated_frame_interval_us_
          : now_us;
  if (release_time_us > now_us) {
    result.next_delay_us = release_time_us - now_us;
    return result;
  }

  result.frame = std::move(frames_.front().frame);
  last_released_media_timestamp_us_ = frames_.front().media_timestamp_us;
  frames_.pop_front();
  last_release_local_timestamp_us_ = now_us;
  ++released_frames_;
  return result;
}

VideoReceiveScheduler::Metrics
VideoReceiveScheduler::GetMetrics(int64_t now_us) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Metrics metrics;
  metrics.buffered_frames = frames_.size();
  metrics.max_buffered_frames = max_buffered_frames_;
  metrics.target_buffer_frames = target_buffer_frames_;
  metrics.estimated_frame_interval_us = estimated_frame_interval_us_;
  metrics.estimated_jitter_us = estimated_jitter_us_;
  metrics.enqueued_frames = enqueued_frames_;
  metrics.released_frames = released_frames_;
  metrics.dropped_frames = dropped_frames_;
  metrics.catch_up_count = catch_up_count_;
  metrics.key_frame_requests = key_frame_requests_;
  metrics.timestamp_fallbacks = timestamp_fallbacks_;
  metrics.awaiting_key_frame = awaiting_key_frame_;
  if (!frames_.empty()) {
    metrics.oldest_frame_wait_us =
        std::max<int64_t>(0, now_us - frames_.front().arrival_timestamp_us);
    const int64_t target_buffer_time_us =
        estimated_frame_interval_us_ * target_buffer_frames_;
    metrics.playout_lag_us = std::max<int64_t>(
        0, metrics.oldest_frame_wait_us - target_buffer_time_us);
  }
  return metrics;
}

} // namespace minirtc
