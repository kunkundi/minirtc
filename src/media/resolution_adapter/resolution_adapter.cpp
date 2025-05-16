#include "resolution_adapter.h"

#include "libyuv.h"
#include "log.h"

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
      width = (width / 16) * 16;
      height = (height / 16) * 16;
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
                                           XVideoFrame* new_frame) {
  if (target_width <= 0 || target_height <= 0) {
    return -1;
  }

  new_frame->width = target_width;
  new_frame->height = target_height;
  new_frame->size = target_width * target_height * 3 / 2;
  new_frame->data = new char[new_frame->size];

  libyuv::NV12Scale((const uint8_t*)(video_frame->data), video_frame->width,
                    (const uint8_t*)(video_frame->data +
                                     video_frame->width * video_frame->height),
                    video_frame->width, video_frame->width, video_frame->height,
                    (uint8_t*)(new_frame->data), target_width,
                    (uint8_t*)(new_frame->data + target_width * target_height),
                    target_width, target_width, target_height,
                    libyuv::kFilterLinear);

  return 0;
}