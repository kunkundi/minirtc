/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-12
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_RESOLUTION_LIMIT_H_
#define _VIDEO_RESOLUTION_LIMIT_H_

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace minirtc {
inline constexpr int kMaxVideoPixels = 3840 * 2160;

struct VideoResolutionLimits {
  int max_width = std::numeric_limits<int>::max();
  int max_height = std::numeric_limits<int>::max();
};

// NV12/encoder dimensions must be even. Apply the bound to the original
// aspect ratio, including portrait displays, without enlarging the source.
inline std::pair<int, int> FitVideoResolution(int source_width, int source_height,
                                             int width, int height,
                                             VideoResolutionLimits limits = {},
                                             int max_pixels = kMaxVideoPixels) {
  if (source_width < 2 || source_height < 2 || limits.max_width < 2 ||
      limits.max_height < 2 || max_pixels < 4) return {0, 0};
  if (width > 0 && height > 0) {
    limits.max_width = std::min(limits.max_width, width);
    limits.max_height = std::min(limits.max_height, height);
  }
  // Even the narrowest NV12 frame uses two rows or columns.
  const int max_width = std::min(limits.max_width, max_pixels / 2);
  const int max_height = std::min(limits.max_height, max_pixels / 2);
  double scale = std::min({1.0, double(max_width) / source_width,
                           double(max_height) / source_height});
  const double pixels = double(source_width) * source_height;
  if (pixels > max_pixels)
    scale = std::min(scale, std::sqrt(max_pixels / pixels));
  return {std::max(2, int(source_width * scale) & ~1),
          std::max(2, int(source_height * scale) & ~1)};
}
}  // namespace minirtc
#endif
