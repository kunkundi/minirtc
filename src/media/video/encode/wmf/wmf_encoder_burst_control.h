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
  enum class DynamicState { Disabled, Unknown, Verified, Unavailable };

  static int ParseBufferMs(const char* value) {
    if (!value || !*value) return kDefaultBufferMs;
    int ms = 0;
    const auto parsed = std::from_chars(value, value + std::strlen(value), ms);
    if (parsed.ec != std::errc{} || *parsed.ptr != '\0' ||
        (ms != 0 && (ms < 100 || ms > 2000)))
      return kDefaultBufferMs;
    return ms;
  }

  // Set before process startup. Both 300 and 600 initialize with at least
  // 600 ms; 0 leaves the driver's optional buffer property untouched.
  static int BufferMsFromEnvironment() {
    return ParseBufferMs(std::getenv("MINIRTC_WMF_BUFFER_MS"));
  }

  // Explicit experiment override, including the first IDR. This is independent
  // of the ordinary-frame duration; it does not imply runtime support. Missing,
  // zero or invalid values retain the recovery allowance at initialization.
  static int InitialBufferMsFromEnvironment(int buffer_ms) {
    const char* value = std::getenv("MINIRTC_WMF_INITIAL_BUFFER_MS");
    int ms = 0;
    if (value && *value) {
      const auto parsed = std::from_chars(value, value + std::strlen(value), ms);
      if (parsed.ec == std::errc{} && *parsed.ptr == '\0' &&
          ms >= 100 && ms <= 2000)
        return ms;
    }
    return std::max(buffer_ms, kRecoveryBufferMs);
  }

  void Reset(int buffer_ms) {
    buffer_ms_ = buffer_ms;
    writes_enabled_ = false;
    dynamic_state_ = DynamicState::Disabled;
    initial_confirmed_ = false;
    tighter_failed_ = false;
    range_.reset();
    requested_bytes_.reset();
    applied_request_bytes_.reset();
    observed_bytes_.reset();
    initial_bytes_.reset();
  }

  // Only called during type negotiation. Preserve observations across phases;
  // IsSupported and an accepted initialization write prove no runtime ability.
  void Configure(bool supported, std::optional<Range> range) {
    writes_enabled_ = supported && buffer_ms_ > 0;
    dynamic_state_ = writes_enabled_ ? DynamicState::Unknown
                                     : DynamicState::Disabled;
    range_ = range && range->minimum <= range->maximum && range->maximum > 0
                 ? range
                 : std::nullopt;
  }

  std::optional<Request> InitialRequest(int bitrate_bps, int ms) const {
    // Read-only verification of an early setting remains valid even if the
    // transform no longer advertises support after type negotiation.
    if (buffer_ms_ <= 0 || bitrate_bps <= 0) return std::nullopt;
    return MakeRequest(bitrate_bps, ms);
  }

  std::optional<Request> Next(int bitrate_bps, bool keyframe) const {
    if (!writes_enabled_ || bitrate_bps <= 0) return std::nullopt;
    // A forced/periodic keyframe keeps at least the old recovery allowance.
    // An unsupported tighter setting also falls back to that same allowance.
    const int ms = keyframe || tighter_failed_
                       ? std::max(buffer_ms_, kRecoveryBufferMs)
                       : buffer_ms_;
    auto request = MakeRequest(bitrate_bps, ms);
    if (request && applied_request_bytes_ == request->bytes)
      return std::nullopt;
    return request;
  }

  static bool ReadbackSatisfies(const Request& request, uint32_t observed) {
    // A tighter ordinary-frame ceiling may be rounded down. A recovery
    // allowance must be restored: retaining the preceding smaller buffer does
    // not confirm that the keyframe budget request took effect.
    return observed > 0 && (request.tighter ? observed <= request.bytes
                                           : observed == request.bytes);
  }

  bool VerifyInitialization(const Request& request,
                            std::optional<uint32_t> observed) {
    Observe(observed);
    initial_confirmed_ = observed && ReadbackSatisfies(request, *observed);
    initial_bytes_ = initial_confirmed_ ? observed : std::nullopt;
    applied_request_bytes_ = initial_confirmed_
        ? std::optional<uint32_t>(request.bytes) : std::nullopt;
    return initial_confirmed_;
  }

  void CompleteInitialization(const Request& request, bool accepted,
                              std::optional<uint32_t> observed) {
    requested_bytes_ = request.bytes;
    VerifyInitialization(request, observed);
    if (!accepted) applied_request_bytes_.reset();
  }

  void Observe(std::optional<uint32_t> observed) {
    observed_bytes_ = observed;
  }

  void ObserveRuntime(std::optional<uint32_t> observed) {
    // A later failed read or unsolicited value change invalidates the current
    // dynamic confirmation. Do not restart optional writes on this instance.
    if (dynamic_state_ == DynamicState::Verified &&
        (!observed || *observed == 0 || observed != observed_bytes_)) {
      dynamic_state_ = DynamicState::Unavailable;
      writes_enabled_ = false;
    }
    Observe(observed);
  }

  // Return true only when a rejected/ignored tighter request needs one legacy
  // fallback attempt. A legacy failure disables optional writes for this MFT;
  // neither outcome asks the caller to rebuild the encoder or drop a frame.
  bool Complete(const Request& request, bool accepted,
                std::optional<uint32_t> observed) {
    const auto previous = observed_bytes_;
    requested_bytes_ = request.bytes;
    Observe(observed);
    if (accepted && observed && ReadbackSatisfies(request, *observed)) {
      applied_request_bytes_ = request.bytes;
      // An unchanged value (including a driver-rounded tighter value) is not
      // evidence that the MFT can modify the property while streaming.
      if (previous && *previous != *observed)
        dynamic_state_ = DynamicState::Verified;
      else if (dynamic_state_ == DynamicState::Unavailable)
        writes_enabled_ = false;
      return false;
    }
    dynamic_state_ = DynamicState::Unavailable;
    if (request.tighter && !tighter_failed_) {
      tighter_failed_ = true;
      // A driver can partially apply a rejected SetValue. Do not assume the
      // previously requested recovery value is still in force.
      applied_request_bytes_.reset();
      return true;
    }
    writes_enabled_ = false;
    return false;
  }

  bool writes_enabled() const { return writes_enabled_; }
  bool initial_confirmed() const { return initial_confirmed_; }
  bool tighter_failed() const { return tighter_failed_; }
  DynamicState dynamic_state() const { return dynamic_state_; }
  const char* dynamic_state_name() const {
    switch (dynamic_state_) {
      case DynamicState::Disabled: return "disabled";
      case DynamicState::Unknown: return "unknown";
      case DynamicState::Verified: return "readback_verified";
      case DynamicState::Unavailable: return "unavailable";
    }
    return "unknown";
  }
  const char* policy_name() const {
    if (buffer_ms_ == 0) return "driver_default";
    if (dynamic_state_ == DynamicState::Verified)
      return tighter_failed_ ? "dynamic_recovery" : "dynamic";
    if (observed_bytes_ && *observed_bytes_ > 0)
      return "fixed_readback";
    return "unverified";
  }
  std::optional<uint32_t> initial_bytes() const { return initial_bytes_; }
  std::optional<uint32_t> requested_bytes() const { return requested_bytes_; }
  std::optional<uint32_t> observed_bytes() const { return observed_bytes_; }

 private:
  std::optional<Request> MakeRequest(int bitrate_bps, int ms) const {
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
    if (bytes == 0) return std::nullopt;
    return Request{static_cast<uint32_t>(bytes), ms, ms < kRecoveryBufferMs};
  }

  int buffer_ms_ = kDefaultBufferMs;
  bool writes_enabled_ = false;
  bool initial_confirmed_ = false;
  bool tighter_failed_ = false;
  DynamicState dynamic_state_ = DynamicState::Disabled;
  std::optional<Range> range_;
  std::optional<uint32_t> requested_bytes_;
  std::optional<uint32_t> applied_request_bytes_;
  std::optional<uint32_t> observed_bytes_;
  std::optional<uint32_t> initial_bytes_;
};

}  // namespace minirtc

#endif
