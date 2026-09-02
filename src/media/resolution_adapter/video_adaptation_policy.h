/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-01
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_ADAPTATION_POLICY_H_
#define _VIDEO_ADAPTATION_POLICY_H_

#include <array>
#include <cstddef>
#include <cstdint>

namespace minirtc {

// Timing and network thresholds shared by the screen-content adaptation
// state machine. Keep the pure decisions here so policy changes can be unit
// tested without constructing an ICE transport.
class VideoAdaptationPolicy {
public:
  struct FrameHealthSignals {
    bool encoded_frame_rate_low = false;
    bool capture_frame_rate_low = false;
    bool pacer_rejection_high = false;
    bool encode_queue_drop_high = false;

    bool Any() const {
      return encoded_frame_rate_low || capture_frame_rate_low ||
             pacer_rejection_high || encode_queue_drop_high;
    }
  };

  static constexpr int64_t kBandwidthMappingStabilityMs = 2000;
  static constexpr int64_t kBandwidthResolutionStartupGraceMs = 5000;
  static constexpr int64_t kBandwidthResolutionCooldownMs = 5000;
  static constexpr int64_t kStaticContentEnterHoldMs = 1000;
  static constexpr int64_t kStaticContentExitHoldMs = 3000;
  static constexpr int64_t kFrameHealthWindowMs = 1000;
  static constexpr int64_t kFrameHealthSustainMs = 1500;

  static constexpr float kStaticContentMaxLossRate = 0.02f;
  static constexpr int64_t kStaticContentMaxRttMs = 150;
  static constexpr float kStaticContentCriticalLossRate = 0.05f;
  static constexpr int64_t kStaticContentCriticalRttMs = 300;

  // Ignore small bandwidth-estimate movements. A spatial downgrade is only
  // useful when the mapped area is at least 20% below the current area.
  static constexpr int kMeaningfulDownscaleAreaPercent = 80;
  static constexpr int kPacerRejectionThresholdPercent = 10;
  static constexpr int kEncodeQueueDropThresholdPercent = 5;
  static constexpr int kMinimumPacerAdmissionSamples = 15;
  static constexpr size_t kLowFrameRateWindowCount = 3;
  static constexpr size_t kLowFrameRateRequiredWindows = 2;

  static int MinimumFrameRate(int configured_frame_rate) {
    return configured_frame_rate <= 30 ? 25 : 45;
  }

  static int UpgradeFrameRate(int configured_frame_rate) {
    // Keep substantially more headroom than the downgrade floor before
    // probing a higher spatial rung. A 60 fps stream that only just reaches
    // 45 fps at the lower resolution is very likely to fall below the floor
    // immediately after an upgrade.
    return configured_frame_rate <= 30 ? 28 : 55;
  }

  static bool IsUpgradeFrameRateHealthy(int configured_frame_rate,
                                        int measured_frame_rate) {
    return measured_frame_rate >= UpgradeFrameRate(configured_frame_rate);
  }

  static size_t CountLowFrameRateWindows(
      int configured_frame_rate,
      const std::array<int, kLowFrameRateWindowCount>& frame_rates,
      size_t valid_window_count) {
    const size_t count =
        valid_window_count < frame_rates.size() ? valid_window_count
                                                 : frame_rates.size();
    const int minimum_frame_rate = MinimumFrameRate(configured_frame_rate);
    size_t low_window_count = 0;
    for (size_t i = 0; i < count; ++i) {
      if (frame_rates[i] < minimum_frame_rate) {
        ++low_window_count;
      }
    }
    return low_window_count;
  }

  static bool IsEncodedFrameRatePersistentlyLow(
      int configured_frame_rate,
      const std::array<int, kLowFrameRateWindowCount>& frame_rates,
      size_t valid_window_count) {
    return valid_window_count >= kLowFrameRateWindowCount &&
           CountLowFrameRateWindows(configured_frame_rate, frame_rates,
                                    valid_window_count) >=
               kLowFrameRateRequiredWindows;
  }

  static FrameHealthSignals EvaluateFrameHealth(
      int configured_frame_rate, bool encoded_frame_rate_persistently_low,
      bool admission_metrics_ready, int capture_frame_rate,
      uint64_t capture_samples, uint64_t pacer_rejected_samples,
      uint64_t encode_queue_dropped_samples) {
    const int minimum_frame_rate = MinimumFrameRate(configured_frame_rate);
    FrameHealthSignals signals;
    signals.encoded_frame_rate_low = encoded_frame_rate_persistently_low;
    signals.capture_frame_rate_low =
        admission_metrics_ready && capture_frame_rate < minimum_frame_rate;
    signals.pacer_rejection_high =
        admission_metrics_ready &&
        capture_samples >= kMinimumPacerAdmissionSamples &&
        pacer_rejected_samples * 100 >=
            capture_samples * kPacerRejectionThresholdPercent;
    signals.encode_queue_drop_high =
        admission_metrics_ready &&
        capture_samples >= kMinimumPacerAdmissionSamples &&
        encode_queue_dropped_samples * 100 >=
            capture_samples * kEncodeQueueDropThresholdPercent;
    return signals;
  }

  static bool IsFrameHealthPressureSustained(int64_t now_ms,
                                             int64_t pressure_since_ms) {
    return pressure_since_ms > 0 && now_ms >= pressure_since_ms &&
           now_ms - pressure_since_ms >= kFrameHealthSustainMs;
  }

  static bool IsStaticContentCandidate(bool is_screen_content, bool in_alr,
                                       float loss_rate, int64_t rtt_ms) {
    return is_screen_content && in_alr &&
           loss_rate <= kStaticContentMaxLossRate &&
           rtt_ms <= kStaticContentMaxRttMs;
  }

  static bool IsStaticContentNetworkCritical(float loss_rate, int64_t rtt_ms) {
    return loss_rate >= kStaticContentCriticalLossRate ||
           rtt_ms >= kStaticContentCriticalRttMs;
  }

  static bool IsBandwidthMappingStable(int64_t now_ms,
                                       int64_t candidate_since_ms) {
    return candidate_since_ms > 0 && now_ms >= candidate_since_ms &&
           now_ms - candidate_since_ms >= kBandwidthMappingStabilityMs;
  }

  static bool IsBandwidthStartupGraceElapsed(int64_t now_ms,
                                             int64_t source_started_ms) {
    return source_started_ms > 0 && now_ms >= source_started_ms &&
           now_ms - source_started_ms >= kBandwidthResolutionStartupGraceMs;
  }

  static bool IsMeaningfulBandwidthDownscale(int64_t current_area,
                                             int64_t target_area) {
    return current_area > 0 && target_area > 0 && target_area < current_area &&
           target_area * 100 <= current_area * kMeaningfulDownscaleAreaPercent;
  }

  static bool ShouldApplyBandwidthResolutionDowngrade(
      bool allow_spatial_downgrade, int64_t now_ms, int64_t source_started_ms,
      int64_t last_resolution_change_ms, int64_t current_area,
      int64_t target_area) {
    return allow_spatial_downgrade &&
           IsMeaningfulBandwidthDownscale(current_area, target_area) &&
           IsBandwidthStartupGraceElapsed(now_ms, source_started_ms) &&
           now_ms >= last_resolution_change_ms &&
           now_ms - last_resolution_change_ms >= kBandwidthResolutionCooldownMs;
  }
};

} // namespace minirtc

#endif
