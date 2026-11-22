/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-23
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SDR_COLOR_H_
#define _SDR_COLOR_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace minirtc {

// The untagged NV12 API uses the same BT.601 limited-range matrix as libyuv.
// Convert tagged SDR at the decoder boundary, including snapshots and native
// CPU uploads. Chroma is shared by each 2x2 block, so no RGB buffer is needed.
struct SdrColor {
  bool full_range = false;
  bool bt709 = false;

  bool NeedsConversion() const { return full_range || bt709; }

  void Normalize(uint8_t* nv12, int width, int height) const {
    if (!NeedsConversion() || !nv12 || width <= 0 || height <= 0 || ((width | height) & 1)) {
      return;
    }
    auto* uv = nv12 + size_t(width) * height;
    const float y_scale = full_range ? 219.f / 255 : 1.f;
    const float c_scale = full_range ? 1.f / 255 : 1.f / 224;
    const float y_offset = full_range ? 16.f : 0.f;
    auto code = [](float x, float hi) {
      return static_cast<uint8_t>(std::clamp(x, 16.f, hi) + .5f);
    };
    for (int row = 0; row < height; row += 2) {
      for (int x = 0; x < width; x += 2) {
        auto* c = uv + size_t(row / 2) * width + x;
        float cb = (c[0] - 128) * c_scale, cr = (c[1] - 128) * c_scale;
        // Substitute BT.709 R',G',B' into the BT.601 luma equation.
        float dy = bt709 ? .101579f * cb + .196076f * cr : 0.f;
        c[0] = code(128 + 224 * (bt709 ? (1.8556f * cb - dy) / 1.772f : cb), 240);
        c[1] = code(128 + 224 * (bt709 ? (1.5748f * cr - dy) / 1.402f : cr), 240);
        for (int j = 0; j < 2; ++j) {
          auto* y = nv12 + size_t(row + j) * width + x;
          y[0] = code(y[0] * y_scale + y_offset + 219 * dy, 235);
          y[1] = code(y[1] * y_scale + y_offset + 219 * dy, 235);
        }
      }
    }
  }
};

}  // namespace minirtc

#endif
