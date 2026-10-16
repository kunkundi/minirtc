#include "web_qos_controller.h"

#include <algorithm>
#include <cmath>

#include "bitrate_limits.h"

namespace minirtc {
namespace {
constexpr int kMinNetworkBps = 64000;
constexpr int kFallbackBps = 500000;
using webrtc::DataRate;
using webrtc::TimeDelta;
using webrtc::Timestamp;
}  // namespace

WebQosController::WebQosController(int64_t now_ms)
    : started_ms_(now_ms), last_feedback_ms_(now_ms), last_update_ms_(now_ms) {
  estimator_.SetBitrates(DataRate::BitsPerSec(budget_.network_bps),
                         DataRate::BitsPerSec(kMinNetworkBps),
                         DataRate::BitsPerSec(kDefaultMaxNetworkBitrateBps),
                         Timestamp::Millis(now_ms));
}

void WebQosController::OnReceiverEstimate(int bitrate_bps, int64_t now_ms) {
  if (bitrate_bps <= 0) return;
  if (budget_.receiver_bps > 0 && bitrate_bps < budget_.receiver_bps * 0.8) {
    healthy_since_ms_ = -1;
    probe_floor_bps_ = 0;
  }
  budget_.receiver_bps = bitrate_bps;
  last_feedback_ms_ = last_remb_ms_ = now_ms;
  ApplyReceiverLimit(now_ms);
}

void WebQosController::ApplyReceiverLimit(int64_t now_ms) {
  if (last_remb_ms_ < 0) return;
  estimator_.UpdateReceiverEstimate(
      Timestamp::Millis(now_ms),
      DataRate::BitsPerSec(
          std::clamp(std::max(budget_.receiver_bps, probe_floor_bps_),
                     kMinNetworkBps, kDefaultMaxNetworkBitrateBps)));
}

void WebQosController::OnReceiverReport(int64_t lost, int64_t expected,
                                        int rtt_ms, int64_t now_ms) {
  last_feedback_ms_ = last_report_ms_ = now_ms;
  if (expected > 0 && expected <= 1000000) {
    lost = std::clamp<int64_t>(lost, 0, expected);
    budget_.loss_ratio = static_cast<double>(lost) / expected;
    if (budget_.loss_ratio > 0.02) healthy_since_ms_ = -1;
    estimator_.UpdatePacketsLost(lost, expected, Timestamp::Millis(now_ms));
  }
  if (rtt_ms > 0 && rtt_ms <= 10000) {
    min_rtt_ms_ = min_rtt_ms_ ? std::min(min_rtt_ms_, rtt_ms) : rtt_ms;
    if (rtt_ms > min_rtt_ms_ + 100 && rtt_ms > min_rtt_ms_ * 1.5)
      BackOff(0.85, now_ms);
    budget_.rtt_ms = rtt_ms;
    estimator_.UpdateRtt(TimeDelta::Millis(rtt_ms), Timestamp::Millis(now_ms));
    estimator_.UpdatePropagationRtt(Timestamp::Millis(now_ms),
                                    TimeDelta::Millis(rtt_ms));
  }
}

void WebQosController::BackOff(double factor, int64_t now_ms) {
  healthy_since_ms_ = -1;
  probe_floor_bps_ = 0;
  if (last_backoff_ms_ >= 0 && now_ms - last_backoff_ms_ < 1000) return;
  last_backoff_ms_ = now_ms;
  estimator_.SetSendBitrate(
      DataRate::BitsPerSec(std::max(
          kMinNetworkBps, static_cast<int>(budget_.network_bps * factor))),
      Timestamp::Millis(now_ms));
  budget_.network_bps =
      std::min(budget_.network_bps, estimator_.target_rate().bps<int>());
}

void WebQosController::OnTelemetry(int64_t lost, int64_t expected, int rtt_ms,
                                   double jitter_buffer_ms, double decode_ms,
                                   double fps, bool frozen, int64_t now_ms) {
  if (!std::isfinite(jitter_buffer_ms) || !std::isfinite(decode_ms) ||
      !std::isfinite(fps) || jitter_buffer_ms < 0 || jitter_buffer_ms > 10000 ||
      decode_ms < 0 || decode_ms > 10000 || fps < 0 || fps > 1000)
    return;
  // Do not count the same loss interval again when standard RR is available.
  if (last_report_ms_ < 0 || now_ms - last_report_ms_ > 2500) {
    OnReceiverReport(lost, expected, rtt_ms, now_ms);
    last_report_ms_ =
        -1;  // Telemetry must not suppress the next fallback sample.
  }
  last_feedback_ms_ = now_ms;
  // At 10 fps, a frame interval alone is 100 ms. Do not interpret the normal
  // low-frame-rate playout cadence as a growing network queue.
  const double excess_buffer_ms =
      std::max(0.0, jitter_buffer_ms - 1000.0 / std::max(5.0, fps));
  const bool growing_buffer =
      excess_buffer_ms > 100 && excess_buffer_ms > previous_jitter_ms_ + 20;
  const bool slow_decode = fps >= 5 && decode_ms * fps > 750;
  if (fps > 0) last_decode_progress_ms_ = now_ms;
  const bool healthy = !frozen && !slow_decode && excess_buffer_ms < 80 &&
                       last_decode_progress_ms_ >= 0 &&
                       now_ms - last_decode_progress_ms_ < 1500 &&
                       lost <= expected * 0.02 && budget_.loss_ratio <= 0.02 &&
                       (min_rtt_ms_ == 0 || budget_.rtt_ms <= min_rtt_ms_ + 50);
  if (!healthy)
    healthy_since_ms_ = -1;
  else if (healthy_since_ms_ < 0)
    healthy_since_ms_ = now_ms;
  // Missing reference frames also freeze playback. Requesting an IDR belongs
  // to the media handler; a freeze alone proves neither congestion nor a slow
  // decoder. Avoid a freeze -> resize -> keyframe -> freeze feedback loop.
  if (growing_buffer) BackOff(0.8, now_ms);
  if (slow_decode &&
      (last_pressure_ms_ < 0 || now_ms - last_pressure_ms_ >= 1000)) {
    budget_.decode_scale = std::max(0.25, budget_.decode_scale * 0.75);
    last_pressure_ms_ = now_ms;
  } else if (!slow_decode && !frozen && !growing_buffer &&
             (last_pressure_ms_ < 0 || now_ms - last_pressure_ms_ > 5000) &&
             (last_telemetry_ms_ < 0 || now_ms - last_telemetry_ms_ >= 500)) {
    budget_.decode_scale = std::min(1.0, budget_.decode_scale + 0.025);
  }
  previous_jitter_ms_ = excess_buffer_ms;
  last_telemetry_ms_ = now_ms;
}

WebQosController::Budget WebQosController::Update(int64_t now_ms,
                                                  int queue_ms) {
  budget_.feedback_age_ms = std::max<int64_t>(0, now_ms - last_feedback_ms_);
  // A stale low REMB must not cap a recovered route forever. With no fresh
  // feedback, the fallback below still prevents an uncontrolled ramp-up.
  if (last_remb_ms_ >= 0 && now_ms - last_remb_ms_ > 5000) {
    estimator_.UpdateReceiverEstimate(Timestamp::Millis(now_ms),
                                      DataRate::PlusInfinity());
    last_remb_ms_ = -1;
  }
  // Local encoder overshoot needs fewer/smaller frames, not a lower network
  // drain rate. Reducing both would reinforce the backlog and trap low rates.
  if (queue_ms <= 150)
    queue_pressure_since_ms_ = -1;
  else if (queue_pressure_since_ms_ < 0)
    queue_pressure_since_ms_ = now_ms;
  // Short keyframe bursts are expected and already bounded by frame deadlines.
  // Only sustained local backlog should lower the spatial budget.
  if (queue_pressure_since_ms_ >= 0 &&
      now_ms - queue_pressure_since_ms_ >= 1000 &&
      (last_pressure_ms_ < 0 || now_ms - last_pressure_ms_ >= 1000)) {
    budget_.decode_scale = std::max(0.25, budget_.decode_scale * 0.75);
    last_pressure_ms_ = now_ms;
  }
  // Desktop content is often application-limited. REMB can stay below the
  // actual healthy receive rate after a freeze or sparse low-resolution
  // updates. Probe slowly only with three seconds of fresh, healthy decoder,
  // loss and RTT feedback; any renewed pressure restores the receiver cap.
  const bool can_probe =
      healthy_since_ms_ >= 0 && now_ms - healthy_since_ms_ >= 3000 &&
      last_telemetry_ms_ >= 0 && now_ms - last_telemetry_ms_ <= 1500 &&
      (last_backoff_ms_ < 0 || now_ms - last_backoff_ms_ >= 3000);
  if (!can_probe)
    probe_floor_bps_ = 0;
  else if (last_probe_ms_ < 0 || now_ms - last_probe_ms_ >= 1000) {
    probe_floor_bps_ = static_cast<int>(budget_.network_bps * 1.08) + 8000;
    last_probe_ms_ = now_ms;
  }
  ApplyReceiverLimit(now_ms);
  estimator_.UpdateEstimate(Timestamp::Millis(now_ms));
  int desired = estimator_.target_rate().bps<int>();
  if (budget_.feedback_age_ms > 5000 ||
      (last_remb_ms_ < 0 && last_report_ms_ < 0 && last_telemetry_ms_ < 0 &&
       now_ms - started_ms_ > 2000)) {
    desired = std::min(desired, kFallbackBps);
  }
  // Apply decreases immediately; never jump to a large REMB during startup.
  const double seconds =
      std::clamp((now_ms - last_update_ms_) / 1000.0, 0.0, 1.0);
  const int increase =
      static_cast<int>((budget_.network_bps * 0.15 + 10000) * seconds);
  budget_.network_bps =
      std::clamp(std::min(desired, budget_.network_bps + increase),
                 kMinNetworkBps, kDefaultMaxNetworkBitrateBps);
  // Ten-millisecond Opus packets include substantial per-packet overhead.
  // Bound audio too on very weak links so it cannot starve screen updates.
  budget_.audio_bps = std::min(96000, budget_.network_bps * 2 / 5);
  // Reserve audio/control and RTP/SRTP overhead + retransmissions. Video can
  // fall below the old 1 Mbit/s encoder default on constrained connections.
  budget_.video_bps = std::max(
      16000, static_cast<int>(budget_.network_bps * 0.85) - budget_.audio_bps);
  budget_.max_fps = budget_.video_bps < 250000   ? 10
                    : budget_.video_bps < 700000 ? 20
                    : budget_.video_bps < 1800000 ? 30
                                                : kMaxFrameRate;
  if (budget_.decode_scale < 0.75)
    budget_.max_fps = std::min(budget_.max_fps, 30);
  last_update_ms_ = now_ms;
  return budget_;
}

}  // namespace minirtc
