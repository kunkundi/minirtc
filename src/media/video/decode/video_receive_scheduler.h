#ifndef MINIRTC_VIDEO_RECEIVE_SCHEDULER_H_
#define MINIRTC_VIDEO_RECEIVE_SCHEDULER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "received_frame.h"
#include "rtc_base/numerics/sequence_number_unwrapper.h"

namespace minirtc {

// A small playout scheduler placed after RTP/RTX reassembly and before video
// decode. It owns no worker thread; callers drive Enqueue() and Poll() from a
// serialized scheduling queue.
class VideoReceiveScheduler {
public:
  struct PollResult {
    std::unique_ptr<ReceivedFrame> frame;
    int64_t next_delay_us = -1;
    bool request_key_frame = false;
    size_t dropped_frames = 0;
  };

  struct Metrics {
    size_t buffered_frames = 0;
    size_t max_buffered_frames = 0;
    int target_buffer_frames = 1;
    int64_t estimated_frame_interval_us = 16667;
    int64_t estimated_jitter_us = 0;
    int64_t oldest_frame_wait_us = 0;
    int64_t playout_lag_us = 0;
    uint64_t enqueued_frames = 0;
    uint64_t released_frames = 0;
    uint64_t dropped_frames = 0;
    uint64_t catch_up_count = 0;
    uint64_t key_frame_requests = 0;
    uint64_t timestamp_fallbacks = 0;
    bool awaiting_key_frame = false;
  };

  explicit VideoReceiveScheduler(int nominal_frame_rate);

  void Enqueue(std::unique_ptr<ReceivedFrame> frame, int64_t now_us);
  PollResult Poll(int64_t now_us, bool decoder_busy);
  Metrics GetMetrics(int64_t now_us) const;

private:
  struct ScheduledFrame {
    std::unique_ptr<ReceivedFrame> frame;
    int64_t media_timestamp_us = 0;
    int64_t arrival_timestamp_us = 0;
    bool is_key_frame = false;
  };

  int64_t MediaTimestampUs(const ReceivedFrame &frame);
  void UpdateTimingEstimate(int64_t media_timestamp_us,
                            int64_t arrival_timestamp_us);
  void UpdateTargetBufferFrames(int desired_target);
  bool IsSeverelyBehind(int64_t now_us) const;
  size_t DropBeforeNewestKeyFrame();

  mutable std::mutex mutex_;
  std::deque<ScheduledFrame> frames_;
  webrtc::RtpTimestampUnwrapper rtp_timestamp_unwrapper_;

  int64_t nominal_frame_interval_us_ = 16667;
  int64_t estimated_frame_interval_us_ = 16667;
  int64_t estimated_jitter_us_ = 0;
  int target_buffer_frames_ = 1;
  int target_decrease_samples_ = 0;

  bool timing_sample_initialized_ = false;
  int64_t last_observed_media_timestamp_us_ = 0;
  int64_t last_observed_arrival_timestamp_us_ = 0;

  bool playout_started_ = false;
  int64_t last_release_local_timestamp_us_ = 0;
  int64_t last_released_media_timestamp_us_ = 0;
  int64_t last_key_frame_request_us_ = 0;
  bool awaiting_key_frame_ = false;

  size_t max_buffered_frames_ = 0;
  uint64_t enqueued_frames_ = 0;
  uint64_t released_frames_ = 0;
  uint64_t dropped_frames_ = 0;
  uint64_t catch_up_count_ = 0;
  uint64_t key_frame_requests_ = 0;
  uint64_t timestamp_fallbacks_ = 0;
};

} // namespace minirtc

#endif // MINIRTC_VIDEO_RECEIVE_SCHEDULER_H_
