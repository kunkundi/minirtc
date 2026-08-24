#include "resolution_adapter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "libyuv.h"
#include "log.h"

namespace minirtc {
namespace {
// Standard resolution tiers, ordered from lowest to highest pixel count.
// These define the discrete "steps" the adaptive algorithm moves between;
// actual encoded dimensions are aspect-ratio-corrected in GetResolution().
constexpr std::pair<int, int> kResolutionSteps[] = {
    {320, 180},    // 180p
    {480, 270},    // 270p
    {640, 360},    // 360p
    {960, 540},    // 540p
    {1280, 720},   // 720p
    {1920, 1080},  // 1080p
    {2560, 1440},  // 1440p
    {3840, 2160},  // 4K
};

// Screen-content model: bitrate = coefficient * pixels.
//
// At 30 fps the coefficient is calibrated for sharp H.264 desktop text:
//   720p  -> ~2.5 Mbps | 1080p -> ~5.6 Mbps
//   1440p -> ~10 Mbps  | 4K    -> ~22 Mbps
// Frame-rate scaling uses sqrt(fps / 30): temporal prediction means 60 fps
// does not require exactly twice the bitrate, while still giving it enough
// headroom to avoid spending the extra frames on coarse quantization.
constexpr float kBitrateAlpha = 1.0f;
constexpr float kBitrateCoeff30Fps = 2.70f;
constexpr float kReferenceFrameRate = 30.0f;
// Reserve media-pipeline headroom when every captured frame matters. Without
// this margin, normal throughput variance keeps the pipeline on the edge of
// its budget and the input queue has to discard frames to stay bounded.
constexpr float kMaintainFrameRateHeadroom = 1.25f;
constexpr float kLegacyBitrateAlpha = 0.822f;
constexpr float kLegacyBitrateCoeff = 10.025f;

// Fraction of min_bitrate used as the minimum-start threshold.
// Lower values allow the encoder to attempt a resolution sooner.
constexpr float kStartRatio = 0.60f;

// Fraction of min_bitrate used as the upgrade ceiling.
// When available bitrate exceeds this level the next tier becomes a candidate.
constexpr float kMaxRatio = 1.60f;

constexpr float kDownscaleStep = 0.88f;
constexpr float kUpscaleStep = 1.10f;

int64_t ClampI64(int64_t value, int64_t min_v, int64_t max_v) {
  if (value < min_v) {
    return min_v;
  }
  if (value > max_v) {
    return max_v;
  }
  return value;
}

int SnapDownToMultiple(int value, int step) {
  if (step <= 1) {
    return value;
  }
  return value - (value % step);
}

int SnapUpToMultiple(int value, int step) {
  if (step <= 1) {
    return value;
  }
  return ((value + step - 1) / step) * step;
}

int ClampInt(int value, int min_v, int max_v) {
  if (value < min_v) {
    return min_v;
  }
  if (value > max_v) {
    return max_v;
  }
  return value;
}

bool BuildStrictAspectResolution(int src_w, int src_h, int64_t target_area,
                                 int min_area, int max_area, int* out_w,
                                 int* out_h) {
  if (!out_w || !out_h || src_w <= 0 || src_h <= 0 || min_area <= 0 ||
      max_area <= 0 || min_area > max_area) {
    return false;
  }

  const int gcd_wh = std::gcd(src_w, src_h);
  const int unit_w = src_w / gcd_wh;
  const int unit_h = src_h / gcd_wh;
  const int64_t unit_area = static_cast<int64_t>(unit_w) * unit_h;
  if (unit_area <= 0) {
    return false;
  }

  // NV12 requires even dimensions. Keep strict aspect ratio by forcing
  // the scaling factor k to be a multiple that makes both w/h even.
  const int k_step = (unit_w % 2 != 0 || unit_h % 2 != 0) ? 2 : 1;

  const int64_t clamped_target_area =
      ClampI64(target_area, static_cast<int64_t>(min_area),
               static_cast<int64_t>(max_area));

  int min_k = static_cast<int>(std::ceil(std::sqrt(
      static_cast<double>(min_area) / static_cast<double>(unit_area))));
  int max_k = static_cast<int>(std::floor(std::sqrt(
      static_cast<double>(max_area) / static_cast<double>(unit_area))));

  min_k = std::max(1, SnapUpToMultiple(min_k, k_step));
  max_k = SnapDownToMultiple(max_k, k_step);
  if (max_k < min_k) {
    return false;
  }

  int ideal_k = static_cast<int>(
      std::round(std::sqrt(static_cast<double>(clamped_target_area) /
                           static_cast<double>(unit_area))));
  ideal_k = ClampInt(ideal_k, min_k, max_k);

  int k_down = SnapDownToMultiple(ideal_k, k_step);
  int k_up = SnapUpToMultiple(ideal_k, k_step);
  if (k_down < min_k) {
    k_down = min_k;
  }
  if (k_up > max_k) {
    k_up = max_k;
  }

  auto area_from_k = [&](int k) -> int64_t {
    return static_cast<int64_t>(unit_area) * k * k;
  };

  int chosen_k = k_down;
  if (k_up >= min_k && k_up <= max_k) {
    int64_t down_diff = std::llabs(area_from_k(k_down) -
                                   static_cast<int64_t>(clamped_target_area));
    int64_t up_diff = std::llabs(area_from_k(k_up) -
                                 static_cast<int64_t>(clamped_target_area));
    if (up_diff < down_diff) {
      chosen_k = k_up;
    }
  }

  *out_w = unit_w * chosen_k;
  *out_h = unit_h * chosen_k;
  return (*out_w > 0 && *out_h > 0);
}
}  // namespace

ResolutionAdapter::ResolutionAdapter(VideoQuality video_quality,
                                     int video_frame_rate,
                                     VideoContentType video_content_type,
                                     VideoDegradationPreference
                                         video_degradation_preference)
    : video_quality_(video_quality),
      video_frame_rate_(std::clamp(video_frame_rate, 15, 60)),
      video_content_type_(video_content_type),
      video_degradation_preference_(video_degradation_preference) {}

ResolutionAdapter::~ResolutionAdapter() {}

int ResolutionAdapter::GetMaxPixelsForQuality() const {
  switch (video_quality_) {
    case QualityHigh:
      return 3840 * 2160;  // up to 4K
    case QualityMedium:
      return 1920 * 1080;  // up to 1080p
    default:
      return 1280 * 720;  // up to 720p
  }
}

float ResolutionAdapter::GetBitrateCoefficient() const {
  if (video_content_type_ != VideoContentType::ScreenContent) {
    return kLegacyBitrateCoeff;
  }
  const float frame_rate_scale =
      std::sqrt(static_cast<float>(video_frame_rate_) / kReferenceFrameRate);
  const float headroom =
      video_degradation_preference_ ==
              VideoDegradationPreference::MaintainFrameRate
          ? kMaintainFrameRateHeadroom
          : 1.0f;
  return kBitrateCoeff30Fps * frame_rate_scale * headroom;
}

float ResolutionAdapter::GetBitrateAlpha() const {
  return video_content_type_ == VideoContentType::ScreenContent
             ? kBitrateAlpha
             : kLegacyBitrateAlpha;
}

ResolutionBitrateLimits ResolutionAdapter::ComputeBitrateLimitsForResolution(
    int w, int h, bool is_highest) const {
  const float pixels = static_cast<float>(w * h);
  const float pow_pixels = std::pow(pixels, GetBitrateAlpha());
  const float bitrate_coefficient = GetBitrateCoefficient();

  const int min_bps = static_cast<int>(bitrate_coefficient * pow_pixels);
  const int start_bps = static_cast<int>(min_bps * kStartRatio);
  // The top tier has no upper cap so the encoder is never forced above it.
  const int max_bps = is_highest ? std::numeric_limits<int>::max()
                                 : static_cast<int>(min_bps * kMaxRatio);
  return {w, h, start_bps, min_bps, max_bps};
}

std::vector<ResolutionBitrateLimits> ResolutionAdapter::GetBitrateLimits()
    const {
  const int max_pixels = GetMaxPixelsForQuality();

  std::vector<ResolutionBitrateLimits> limits;
  limits.reserve(std::size(kResolutionSteps));

  int last_valid_idx = -1;
  for (int i = 0; i < static_cast<int>(std::size(kResolutionSteps)); ++i) {
    if (kResolutionSteps[i].first * kResolutionSteps[i].second <= max_pixels) {
      last_valid_idx = i;
    }
  }

  for (int i = 0; i <= last_valid_idx; ++i) {
    const auto [w, h] = kResolutionSteps[i];
    limits.push_back(
        ComputeBitrateLimitsForResolution(w, h, i == last_valid_idx));
  }

  return limits;
}

int ResolutionAdapter::GetResolution(int target_bitrate, int current_width,
                                     int current_height, int* target_width,
                                     int* target_height) {
  if (!target_width || !target_height || current_width <= 0 ||
      current_height <= 0 || target_bitrate <= 0) {
    return -1;
  }

  auto limits = GetBitrateLimits();
  if (limits.empty()) {
    *target_width = -1;
    *target_height = -1;
    return -1;
  }

  const int min_pixels = limits.front().width * limits.front().height;
  const int max_pixels = GetMaxPixelsForQuality();

  const float estimated_pixels_f =
      video_degradation_preference_ ==
              VideoDegradationPreference::MaintainResolution
          ? static_cast<float>(max_pixels)
          : std::pow(
                static_cast<float>(target_bitrate) / GetBitrateCoefficient(),
                1.0f / GetBitrateAlpha());
  const int64_t estimated_pixels = static_cast<int64_t>(std::llround(
      std::max(static_cast<float>(min_pixels),
               std::min(estimated_pixels_f, static_cast<float>(max_pixels)))));

  if (!BuildStrictAspectResolution(current_width, current_height,
                                   estimated_pixels, min_pixels, max_pixels,
                                   target_width, target_height)) {
    *target_width = -1;
    *target_height = -1;
    return -1;
  }

  return 0;
}

int ResolutionAdapter::ResolutionDowngrade(const XVideoFrame* video_frame,
                                           int target_width, int target_height,
                                           XVideoFrame* scaled_frame) {
  if (target_width <= 0 || target_height <= 0) {
    return -1;
  }

  scaled_frame->width = target_width;
  scaled_frame->height = target_height;
  scaled_frame->size = target_width * target_height * 3 / 2;
  scaled_frame->data = new char[scaled_frame->size];

  libyuv::NV12Scale(
      (const uint8_t*)(video_frame->data), video_frame->width,
      (const uint8_t*)(video_frame->data +
                       video_frame->width * video_frame->height),
      video_frame->width, video_frame->width, video_frame->height,
      (uint8_t*)(scaled_frame->data), target_width,
      (uint8_t*)(scaled_frame->data + target_width * target_height),
      target_width, target_width, target_height,
      video_content_type_ == VideoContentType::ScreenContent
          ? libyuv::kFilterBox
          : libyuv::kFilterLinear);

  return 0;
}

int ResolutionAdapter::ResolutionDowngrade(const RawFrame& video_frame,
                                           int target_width, int target_height,
                                           RawFrame& scaled_frame) {
  if (target_width <= 0 || target_height <= 0) {
    return -1;
  }

  int scaled_resolution = target_width * target_height * 3 / 2;
  if (scaled_resolution > tmp_buffer_.size()) {
    tmp_buffer_.resize(scaled_resolution);
  }

  const int src_width = video_frame.Width();
  const int src_height = video_frame.Height();
  const uint8_t* y_plane = video_frame.Buffer();
  const uint8_t* uv_plane = y_plane + src_width * src_height;

  uint8_t* dst_y = tmp_buffer_.data();
  uint8_t* dst_uv = dst_y + target_width * target_height;

  libyuv::NV12Scale(y_plane, src_width, uv_plane, src_width, src_width,
                    src_height, dst_y, target_width, dst_uv, target_width,
                    target_width, target_height,
                    video_content_type_ == VideoContentType::ScreenContent
                        ? libyuv::kFilterBox
                        : libyuv::kFilterLinear);

  scaled_frame.UpdateBuffer(tmp_buffer_.data(), scaled_resolution);
  scaled_frame.SetWidth(target_width);
  scaled_frame.SetHeight(target_height);
  scaled_frame.SetSize(scaled_resolution);

  return 0;
}

std::pair<int, int> ResolutionAdapter::GetNextLowerResolution(int current_w,
                                                              int current_h,
                                                              int source_w,
                                                              int source_h) {
  auto limits = GetBitrateLimits();
  if (limits.empty()) {
    return {-1, -1};
  }
  if (current_w <= 0 || current_h <= 0 || source_w <= 0 || source_h <= 0) {
    return {-1, -1};
  }

  const int min_area = limits.front().width * limits.front().height;
  const int64_t current_area = static_cast<int64_t>(current_w) * current_h;
  if (current_area <= min_area) {
    return {-1, -1};
  }

  const int64_t target_area = static_cast<int64_t>(
      std::llround(current_area * kDownscaleStep * kDownscaleStep));
  int next_w = -1;
  int next_h = -1;
  // Always use the original source resolution for aspect ratio.
  if (!BuildStrictAspectResolution(source_w, source_h, target_area, min_area,
                                   static_cast<int>(current_area), &next_w,
                                   &next_h)) {
    return {-1, -1};
  }
  if (next_w >= current_w || next_h >= current_h) {
    return {-1, -1};
  }
  return {next_w, next_h};
}

std::pair<int, int> ResolutionAdapter::GetNextHigherResolution(int current_w,
                                                               int current_h,
                                                               int source_w,
                                                               int source_h) {
  auto limits = GetBitrateLimits();
  if (limits.empty()) {
    return {-1, -1};
  }
  if (current_w <= 0 || current_h <= 0 || source_w <= 0 || source_h <= 0) {
    return {-1, -1};
  }

  const int max_area = limits.back().width * limits.back().height;
  const int64_t current_area = static_cast<int64_t>(current_w) * current_h;
  if (current_area >= max_area) {
    return {-1, -1};
  }

  const int64_t target_area = static_cast<int64_t>(
      std::llround(current_area * kUpscaleStep * kUpscaleStep));
  int next_w = -1;
  int next_h = -1;
  // Always use the original source resolution for aspect ratio.
  if (!BuildStrictAspectResolution(source_w, source_h, target_area,
                                   static_cast<int>(current_area), max_area,
                                   &next_w, &next_h)) {
    return {-1, -1};
  }
  if (next_w <= current_w || next_h <= current_h) {
    return {-1, -1};
  }
  return {next_w, next_h};
}
}  // namespace minirtc
