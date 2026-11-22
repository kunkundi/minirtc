/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-23
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _HDR_VIDEO_FRAME_H_
#define _HDR_VIDEO_FRAME_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "minirtc.h"

namespace minirtc {

// BT.2100 PQ maps [0, 1] to [0, 10000] cd/m².
inline float PqToNits(float pq) {
  float p = std::pow(pq, 32.f / 2523);
  return 10000.f * std::pow(std::max(p - 3424.f / 4096, 0.f) / (2413.f / 128 - 2392.f / 128 * p),
                            16384.f / 2610);
}

inline float NitsToPq(float nits) {
  float p = std::pow(nits / 10000.f, 2610.f / 16384);
  return std::pow((3424.f / 4096 + 2413.f / 128 * p) / (1.f + 2392.f / 128 * p), 2523.f / 32);
}

// Index positive IEEE floats by exponent and the top seven mantissa bits.
// This retains precision near black without evaluating powers per pixel.
template <class Fn>
inline std::array<float, 32768> MakeTransferTable(Fn fn) {
  std::array<float, 32768> table{};
  for (uint32_t i = 0; i < 255 * 128; ++i) {
    uint32_t bits = i << 16;
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    table[i] = fn(value);
  }
  return table;
}

inline float LookupPositive(const std::array<float, 32768>& table, float value) {
  if (!(value > 0)) {
    return 0;
  }
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t index = std::min(bits >> 16, 32638u);
  const float fraction = (bits & 65535u) / 65536.f;
  return table[index] + fraction * (table[index + 1] - table[index]);
}

inline float ScRgbToPq(float linear) {
  static const auto table =
      MakeTransferTable([](float x) { return NitsToPq(std::min(x, 125.f) * 80.f); });
  return LookupPositive(table, linear);
}

inline float LinearToSdr(float linear) {
  static const auto table = MakeTransferTable([](float x) {
    // Reserve a soft shoulder for specular highlights above diffuse white.
    if (x > .75f) {
      x = 1.f - .0625f / (x - .5f);
    }
    return x <= .0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - .055f;
  });
  return LookupPositive(table, linear);
}

struct HdrRgb {
  float r, g, b;
};

inline HdrRgb Pq2020ToSdr(HdrRgb rgb, float white_nits) {
  static const auto eotf = [] {
    std::array<float, 4097> table{};
    for (size_t i = 0; i < table.size(); ++i) {
      table[i] = PqToNits(i / 4096.f);
    }
    return table;
  }();
  auto decode = [&](float x) {
    return eotf[static_cast<size_t>(std::clamp(x, 0.f, 1.f) * 4096.f + .5f)] / white_nits;
  };
  float r = decode(rgb.r), g = decode(rgb.g), b = decode(rgb.b);
  return {LinearToSdr(1.660491f * r - .587641f * g - .072850f * b),
          LinearToSdr(-.124550f * r + 1.132900f * g - .008349f * b),
          LinearToSdr(-.018151f * r - .100579f * g + 1.118730f * b)};
}

// Owned CPU picture shared by the capture, encoder and decoder callbacks.
// copy_to_nv12 always returns a tone-mapped BT.601 limited-range SDR picture.
class HdrVideoFrame {
 public:
  static std::shared_ptr<HdrVideoFrame> Create(int w, int h, float white_nits = 203.f) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || ((w | h) & 1)) {
      return {};
    }
    return {new HdrVideoFrame(w, h, white_nits), [](HdrVideoFrame* p) { Release(p); }};
  }

  uint16_t* Y() { return pixels_.data(); }
  uint16_t* U() { return Y() + size_t(width_) * height_; }
  uint16_t* V() { return U() + size_t(width_) * height_ / 4; }
  const MiniRtcNativeVideoFrame* Descriptor() const { return &descriptor_; }

  static HdrRgb ReadPq(const uint16_t* pixels, int w, int h, int x, int y) {
    const size_t plane = size_t(w) * h;
    const size_t chroma = size_t(y / 2) * (w / 2) + x / 2;
    float l = (pixels[size_t(y) * w + x] - 64.f) / 876.f;
    float u = (pixels[plane + chroma] - 512.f) / 896.f;
    float v = (pixels[plane + plane / 4 + chroma] - 512.f) / 896.f;
    return {l + 1.4746f * v, l - .164553f * u - .571353f * v, l + 1.8814f * u};
  }

  template <class Read>
  void WritePqBlock(int x, int y, Read read) {
    float u = 0, v = 0;
    for (int j = 0; j < 2; ++j) {
      for (int i = 0; i < 2; ++i) {
        auto rgb = read(x + i, y + j);
        float l = .2627f * rgb.r + .6780f * rgb.g + .0593f * rgb.b;
        Y()[size_t(y + j) * width_ + x + i] = static_cast<uint16_t>(64.5f + 876.f * l);
        u += (rgb.b - l) / 1.8814f;
        v += (rgb.r - l) / 1.4746f;
      }
    }
    const size_t c = size_t(y / 2) * (width_ / 2) + x / 2;
    U()[c] = static_cast<uint16_t>(512.5f + 224.f * u);
    V()[c] = static_cast<uint16_t>(512.5f + 224.f * v);
  }

  int CopySdr(uint8_t* dst, size_t size) const {
    const size_t plane = size_t(width_) * height_;
    if (!dst || size < plane + plane / 2) {
      return -1;
    }
    for (int y = 0; y < height_; y += 2) {
      for (int x = 0; x < width_; x += 2) {
        float u = 0, v = 0;
        for (int j = 0; j < 2; ++j) {
          for (int i = 0; i < 2; ++i) {
            auto rgb =
                Pq2020ToSdr(ReadPq(pixels_.data(), width_, height_, x + i, y + j), white_nits_);
            float l = .299f * rgb.r + .587f * rgb.g + .114f * rgb.b;
            dst[size_t(y + j) * width_ + x + i] = static_cast<uint8_t>(16.5f + 219.f * l);
            u += (rgb.b - l) / 1.772f;
            v += (rgb.r - l) / 1.402f;
          }
        }
        const size_t c = plane + size_t(y / 2) * width_ + x;
        dst[c] = static_cast<uint8_t>(128.5f + 56.f * u);
        dst[c + 1] = static_cast<uint8_t>(128.5f + 56.f * v);
      }
    }
    return 0;
  }

 private:
  HdrVideoFrame(int w, int h, float white)
      : width_(w),
        height_(h),
        white_nits_(std::isfinite(white) && white > 0 ? white : 203.f),
        pixels_(size_t(w) * h * 3 / 2) {
    descriptor_.struct_size = sizeof(descriptor_);
    descriptor_.type = MiniRtcNativeVideoFrameHdr10;
    descriptor_.width = w;
    descriptor_.height = h;
    descriptor_.payload.hdr10.i010 = pixels_.data();
    descriptor_.payload.hdr10.sdr_white_nits = white_nits_;
    descriptor_.owner = this;
    descriptor_.retain = [](void* p) {
      static_cast<HdrVideoFrame*>(p)->refs_.fetch_add(1, std::memory_order_relaxed);
    };
    descriptor_.release = &Release;
    descriptor_.copy_to_nv12 = [](void* p, uint8_t* dst, size_t size) {
      return static_cast<HdrVideoFrame*>(p)->CopySdr(dst, size);
    };
  }
  static void Release(void* p) {
    auto* f = static_cast<HdrVideoFrame*>(p);
    if (f->refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete f;
    }
  }
  int width_, height_;
  float white_nits_;
  std::vector<uint16_t> pixels_;
  std::atomic<unsigned> refs_{1};
  MiniRtcNativeVideoFrame descriptor_{};
};

}  // namespace minirtc

#endif
