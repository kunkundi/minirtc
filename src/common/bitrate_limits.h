#ifndef MINIRTC_BITRATE_LIMITS_H_
#define MINIRTC_BITRATE_LIMITS_H_

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace minirtc {

inline constexpr int kDefaultMaxNetworkBitrateBps = 20'000'000;
inline constexpr int kDefaultMaxEncoderBitrateBps = 30'000'000;
inline constexpr int kMaxEncoderOvershootPercent = 50;

struct StartingBitrates {
  int direct_bps = 2'500'000;
  int relay_bps = 1'500'000;

  static bool IsValid(int bps) {
    return bps >= 300'000 && bps <= kDefaultMaxNetworkBitrateBps;
  }

  static StartingBitrates Normalize(int direct_bps, int relay_bps) {
    StartingBitrates rates;
    if (IsValid(direct_bps)) rates.direct_bps = direct_bps;
    if (IsValid(relay_bps)) rates.relay_bps = relay_bps;
    return rates;
  }

  static int Parse(std::string_view value, int fallback) {
    if (value.empty()) return fallback;
    int bps = 0;
    const auto result =
        std::from_chars(value.data(), value.data() + value.size(), bps);
    return result.ec == std::errc{} &&
                   result.ptr == value.data() + value.size() && IsValid(bps)
               ? bps : fallback;
  }
};

inline int ClampEncoderTargetBitrate(int bitrate, int max_bitrate) {
  return std::min(bitrate, max_bitrate);
}

inline int EncoderPeakBitrate(int target_bitrate, int max_bitrate) {
  const int64_t peak_bitrate =
      static_cast<int64_t>(target_bitrate) *
      (100 + kMaxEncoderOvershootPercent) / 100;
  return static_cast<int>(
      std::min<int64_t>(peak_bitrate, static_cast<int64_t>(max_bitrate)));
}

inline int EncoderOvershootPercent(int target_bitrate, int max_bitrate) {
  if (target_bitrate <= 0 || target_bitrate >= max_bitrate) {
    return 0;
  }
  const int64_t allowed_percent =
      static_cast<int64_t>(max_bitrate - target_bitrate) * 100 /
      target_bitrate;
  return static_cast<int>(std::min<int64_t>(
      allowed_percent, static_cast<int64_t>(kMaxEncoderOvershootPercent)));
}

}  // namespace minirtc

#endif  // MINIRTC_BITRATE_LIMITS_H_
