/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-15
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WMF_ENCODER_BURST_CONTROL_H_
#define _WMF_ENCODER_BURST_CONTROL_H_

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace minirtc {

// HRD is an optional encoder buffer constraint, not a hard per-frame cap.
// Keep average bitrate, sample timestamps and frame admission independent.
class WmfEncoderBurstControl {
 public:
  static constexpr int kDefaultBufferMs = 300;
  static constexpr int kRecoveryBufferMs = 600;

  struct Range {
    uint32_t minimum;
    uint32_t maximum;
    uint32_t step;
  };
  struct Request {
    uint32_t bytes;
    int duration_ms;
    bool tighter;
  };

  static int ParseBufferMs(const char* value) {
    if (!value || !*value) return kDefaultBufferMs;
    int ms = 0;
    const auto parsed = std::from_chars(value, value + std::strlen(value), ms);
    if (parsed.ec != std::errc{} || *parsed.ptr != '\0' ||
        (ms != 0 && (ms < 100 || ms > 2000)))
      return kDefaultBufferMs;
    return ms;
  }

  // Set before process startup: 600 reproduces the previous buffer policy;
  // 0 leaves the driver's optional buffer property untouched.
  static int BufferMsFromEnvironment() {
    return ParseBufferMs(std::getenv("MINIRTC_WMF_BUFFER_MS"));
  }

  void Reset(int buffer_ms, bool supported, std::optional<Range> range) {
    buffer_ms_ = buffer_ms;
    enabled_ = supported && buffer_ms > 0;
    tighter_failed_ = false;
    range_ = range && range->minimum <= range->maximum && range->maximum > 0
                 ? range
                 : std::nullopt;
    requested_bytes_.reset();
    observed_bytes_.reset();
  }

  std::optional<Request> Next(int bitrate_bps, bool keyframe) const {
    if (!enabled_ || bitrate_bps <= 0) return std::nullopt;
    // A forced/periodic keyframe keeps at least the old recovery allowance.
    // An unsupported tighter setting also falls back to that same allowance.
    const int ms = keyframe || tighter_failed_
                       ? std::max(buffer_ms_, kRecoveryBufferMs)
                       : buffer_ms_;
    uint64_t bytes = std::clamp<int64_t>(
        int64_t(bitrate_bps) * ms / 8000, 4096, 128 * 1024);
    if (range_) {
      const auto& r = *range_;
      bytes = std::clamp<uint64_t>(bytes, r.minimum, r.maximum);
      // Respect the driver's reported lattice without overflowing at UINT_MAX.
      const uint64_t step = std::max<uint32_t>(r.step, 1);
      const uint64_t offset = bytes - r.minimum;
      const uint64_t rounded = r.minimum + (offset + step - 1) / step * step;
      bytes = rounded <= r.maximum
                  ? rounded
                  : r.minimum + (uint64_t(r.maximum) - r.minimum) / step * step;
    }
    if (bytes == 0 || requested_bytes_ == bytes) return std::nullopt;
    return Request{static_cast<uint32_t>(bytes), ms,
                   ms < kRecoveryBufferMs};
  }

  static bool ReadbackSatisfies(const Request& request, uint32_t observed) {
    // A tighter ordinary-frame ceiling may be rounded down. A recovery
    // allowance must be restored: retaining the preceding smaller buffer does
    // not confirm that the keyframe budget request took effect.
    return observed > 0 && (request.tighter ? observed <= request.bytes
                                           : observed == request.bytes);
  }

  // Return true only when a rejected/ignored tighter request needs one legacy
  // fallback attempt. A legacy failure disables optional writes for this MFT;
  // neither outcome asks the caller to rebuild the encoder or drop a frame.
  bool Complete(const Request& request, bool accepted,
                std::optional<uint32_t> observed) {
    observed_bytes_ = observed;
    const bool ignored = observed && !ReadbackSatisfies(request, *observed);
    if (accepted && !ignored) {
      requested_bytes_ = request.bytes;
      return false;
    }
    if (request.tighter) {
      tighter_failed_ = true;
      // A driver can partially apply a rejected SetValue. Do not assume the
      // previously requested recovery value is still in force.
      requested_bytes_.reset();
      return true;
    }
    enabled_ = false;
    return false;
  }

  bool enabled() const { return enabled_; }
  bool tighter_failed() const { return tighter_failed_; }
  std::optional<uint32_t> requested_bytes() const { return requested_bytes_; }
  std::optional<uint32_t> observed_bytes() const { return observed_bytes_; }

 private:
  int buffer_ms_ = kDefaultBufferMs;
  bool enabled_ = false;
  bool tighter_failed_ = false;
  std::optional<Range> range_;
  std::optional<uint32_t> requested_bytes_;
  std::optional<uint32_t> observed_bytes_;
};

}  // namespace minirtc

#endif
