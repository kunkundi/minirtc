#include "resolution_adapter.h"

#include "libyuv.h"
#include "log.h"

namespace minirtc {
namespace {
constexpr size_t MAX_RESOLUTION_WIDTH = 3840 * 2160;
}

ResolutionAdapter::ResolutionAdapter(VideoQuality video_quality)
    : video_quality_(video_quality) {}

ResolutionAdapter::~ResolutionAdapter() {}

int ResolutionAdapter::GetResolution(int target_bitrate, int current_width,
                                     int current_height, int* target_width,
                                     int* target_height) {
  if (target_bitrate < GetBitrateLimits().front().min_start_bitrate_bps) {
    *target_width = GetBitrateLimits().front().width;
    *target_height = GetBitrateLimits().front().height;
    return 0;
  }

  for (auto& resolution : GetBitrateLimits()) {
    if (target_bitrate >= resolution.min_start_bitrate_bps &&
        target_bitrate < resolution.max_bitrate_bps) {
      float aspect_ratio = static_cast<float>(current_width) / current_height;
      int width, height;
      if (static_cast<float>(resolution.width) / resolution.height !=
          aspect_ratio) {
        if (aspect_ratio > 1.0f) {
          width = resolution.width;
          height = static_cast<int>(resolution.width / aspect_ratio);
        } else {
          height = resolution.height;
          width = static_cast<int>(resolution.height * aspect_ratio);
        }
      } else {
        width = resolution.width;
        height = resolution.height;
      }
      // width = (width / 16) * 16;
      // height = (height / 16) * 16;
      *target_width = width;
      *target_height = height;
      return 0;
    }
  }

  *target_width = -1;
  *target_height = -1;

  return -1;
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
      target_width, target_width, target_height, libyuv::kFilterLinear);

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
                    target_width, target_height, libyuv::kFilterLinear);

  scaled_frame.UpdateBuffer(tmp_buffer_.data(), scaled_resolution);
  scaled_frame.SetWidth(target_width);
  scaled_frame.SetHeight(target_height);
  scaled_frame.SetSize(scaled_resolution);

  return 0;
}

std::pair<int, int> ResolutionAdapter::GetNextLowerResolution(int current_w,
                                                              int current_h) {
  auto limits = GetBitrateLimits();
  if (limits.empty()) {
    return {-1, -1};
  }
  int current_area = current_w * current_h;
  int best_w = -1, best_h = -1;
  for (const auto& r : limits) {
    int area = r.width * r.height;
    if (area < current_area) {
      best_w = r.width;
      best_h = r.height;
    }
  }
  return {best_w, best_h};
}
}  // namespace minirtc