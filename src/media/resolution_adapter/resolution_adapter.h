/*
 * @Author: DI JUNKUN
 * @Date: 2025-03-06
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RESOLUTION_ADAPTER_H_
#define _RESOLUTION_ADAPTER_H_

#include <utility>
#include <vector>

#include "minirtc.h"
#include "raw_frame.h"
#include "resolution_bitrate_limits.h"

namespace minirtc {

class ResolutionAdapter {
 public:
  ResolutionAdapter(VideoQuality video_quality, int video_frame_rate,
                    VideoContentType video_content_type =
                        VideoContentType::ScreenContent,
                    VideoDegradationPreference video_degradation_preference =
                        VideoDegradationPreference::MaintainResolution);
  ~ResolutionAdapter();

 public:
  int GetResolution(int target_bitrate, int current_width, int current_height,
                    int* target_width, int* target_height);

  int ResolutionDowngrade(const MiniRtcVideoFrame* video_frame, int target_width,
                          int target_height, MiniRtcVideoFrame* scaled_frame);

  int ResolutionDowngrade(const RawFrame& video_frame, int target_width,
                          int target_height, RawFrame& scaled_frame);

  std::pair<int, int> GetNextLowerResolution(int current_w, int current_h,
                                             int source_w, int source_h);
  std::pair<int, int> GetNextHigherResolution(int current_w, int current_h,
                                              int source_w, int source_h);

 public:
  std::vector<ResolutionBitrateLimits> GetBitrateLimits() const;

  int SetTargetBitrate(int bitrate);

 private:
  // Compute bitrate limits for a single resolution tier.
  // |is_highest| marks the top tier whose max_bitrate is set to INT_MAX,
  // signalling "no upper cap" so the encoder is never forced to upgrade.
  ResolutionBitrateLimits ComputeBitrateLimitsForResolution(
      int w, int h, bool is_highest) const;

  int GetMaxPixelsForQuality() const;

  float GetBitrateCoefficient() const;

  float GetBitrateAlpha() const;

 private:
  std::vector<uint8_t> tmp_buffer_;
  std::vector<uint8_t> scale_scratch_buffer_;
  VideoQuality video_quality_;
  int video_frame_rate_;
  VideoContentType video_content_type_;
  VideoDegradationPreference video_degradation_preference_;
};
}  // namespace minirtc

#endif
