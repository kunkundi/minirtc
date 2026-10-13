/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-12
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_RESOLUTION_LIMIT_H_
#define _VIDEO_RESOLUTION_LIMIT_H_

#include <algorithm>
#include <utility>

namespace minirtc {
// NV12/encoder dimensions must be even. Apply the bound to the original
// aspect ratio, including portrait displays, without enlarging the source.
inline std::pair<int, int> FitVideoResolution(int source_width, int source_height,
                                             int width, int height) {
  if (source_width < 2 || source_height < 2) return {0, 0};
  const double scale = width > 0 && height > 0
                           ? std::min({1.0, double(width) / source_width,
                                       double(height) / source_height})
                           : 1.0;
  return {std::max(2, int(source_width * scale) & ~1),
          std::max(2, int(source_height * scale) & ~1)};
}
}  // namespace minirtc
#endif
