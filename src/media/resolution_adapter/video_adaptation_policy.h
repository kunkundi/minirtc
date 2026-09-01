/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-01
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_ADAPTATION_POLICY_H_
#define _VIDEO_ADAPTATION_POLICY_H_

#include <cstdint>

namespace minirtc {

// Timing and network thresholds shared by the screen-content adaptation
// state machine. Keep the pure decisions here so policy changes can be unit
// tested without constructing an ICE transport.
class VideoAdaptationPolicy {
public:
  static constexpr int64_t kBandwidthMappingStabilityMs = 2000;
  static constexpr int64_t kBandwidthResolutionStartupGraceMs = 5000;
  static constexpr int64_t kBandwidthResolutionCooldownMs = 5000;
  static constexpr int64_t kStaticContentEnterHoldMs = 1000;
  static constexpr int64_t kStaticContentExitHoldMs = 3000;

  static constexpr float kStaticContentMaxLossRate = 0.02f;
  static constexpr int64_t kStaticContentMaxRttMs = 150;
  static constexpr float kStaticContentCriticalLossRate = 0.05f;
  static constexpr int64_t kStaticContentCriticalRttMs = 300;

  // Ignore small bandwidth-estimate movements. A spatial downgrade is only
  // useful when the mapped area is at least 20% below the current area.
  static constexpr int kMeaningfulDownscaleAreaPercent = 80;

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
