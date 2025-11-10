/*
 * @Author: DI JUNKUN
 * @Date: 2025-03-06
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RESOLUTION_ADAPTER_H_
#define _RESOLUTION_ADAPTER_H_

#include <vector>

#include "minirtc.h"
#include "raw_frame.h"
#include "resolution_bitrate_limits.h"

namespace minirtc {

class ResolutionAdapter {
 public:
  ResolutionAdapter();
  ~ResolutionAdapter();

 public:
  int GetResolution(int target_bitrate, int current_width, int current_height,
                    int* target_width, int* target_height);

  int ResolutionDowngrade(const XVideoFrame* video_frame, int target_width,
                          int target_height, XVideoFrame* scaled_frame);

  int ResolutionDowngrade(const RawFrame& video_frame, int target_width,
                          int target_height, RawFrame& scaled_frame);

 public:
  std::vector<ResolutionBitrateLimits> GetBitrateLimits() {
    return {
        {320, 180, 30'000, 80'000, 150'000},            // 180p
        {480, 270, 80'000, 150'000, 250'000},           // 270p
        {640, 360, 150'000, 250'000, 400'000},          // 360p
        {960, 540, 300'000, 500'000, 800'000},          // 540p
        {1280, 720, 500'000, 800'000, 1'200'000},       // 720p
        {1920, 1080, 800'000, 1'200'000, 2'000'000},    // 1080p
        {2560, 1440, 1'500'000, 2'500'000, 4'000'000},  // 1440p
        {3840, 2160, 4'000'000, 8'000'000, 12'000'000}  // 4K
    };
  }

  int SetTargetBitrate(int bitrate);

 private:
  std::vector<uint8_t> tmp_buffer_;
};
}  // namespace minirtc

#endif