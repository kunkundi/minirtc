/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-12
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_RESOLUTION_LIMIT_H_
#define _VIDEO_RESOLUTION_LIMIT_H_

#include <algorithm>
#include <chrono>
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
enum class EncoderSizeSupport { Supported, Unsupported, Unavailable };

// Some encoders only reveal their limits by negotiating a concrete format.
// Run on their encode queue, once per requested size. A successful call leaves
// the accepted format configured, including after an unsuccessful larger probe.
class EncoderResolutionSelector {
 public:
  template <typename Configure>
  std::pair<int, int> Select(int width, int height, VideoResolutionLimits limits,
                             Configure configure) {
    const auto request = FitVideoResolution(width, height, 0, 0, limits);
    if (request.first == 0) return {};
    if (request == requested_ &&
        (selected_.first || std::chrono::steady_clock::now() < retry_after_))
      return selected_;
    requested_ = request;
    selected_ = {};
    auto status = configure(request.first, request.second);
    if (status == EncoderSizeSupport::Supported) return selected_ = request;

    // Search along the original aspect ratio in even-pixel steps. Drivers
    // often cap an axis at a power of two. Bracket that edge first to avoid
    // opening a dozen hardware sessions just to discover a 4096-pixel limit.
    int high = std::max(request.first, request.second) / 2;
    int low = 0;
    std::pair<int, int> best{}, last{};
    auto probe = [&](int mid) {
      last = FitVideoResolution(request.first, request.second, mid * 2, mid * 2);
      status = configure(last.first, last.second);
      if (status == EncoderSizeSupport::Supported) {
        best = last;
        low = mid;
      } else {
        high = mid;
      }
    };
    int edge = 1;
    while (edge * 2 < high) edge *= 2;
    if (status == EncoderSizeSupport::Unsupported && edge + 1 < high) probe(edge + 1);
    if (status == EncoderSizeSupport::Unsupported && edge < high) probe(edge);
    while (status != EncoderSizeSupport::Unavailable && high - low > 1) {
      probe(low + (high - low) / 2);
    }
    if (best.first && ((last == best && status == EncoderSizeSupport::Supported) ||
        configure(best.first, best.second) == EncoderSizeSupport::Supported))
      return selected_ = best;
    retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    return {};
  }

 private:
  std::pair<int, int> requested_{}, selected_{};
  std::chrono::steady_clock::time_point retry_after_{};
};
}  // namespace minirtc
#endif
