/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-04
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _NATIVE_VIDEO_FRAME_H_
#define _NATIVE_VIDEO_FRAME_H_

#include <limits>
#include <utility>

#include "minirtc.h"

namespace minirtc {

inline bool GetNv12FrameSize(uint32_t width, uint32_t height, size_t* size) {
  if (!size || width == 0 || height == 0 || (width & 1U) != 0 ||
      (height & 1U) != 0 ||
      static_cast<size_t>(width) >
          std::numeric_limits<size_t>::max() / height) {
    return false;
  }
  const size_t pixels = static_cast<size_t>(width) * height;
  if (pixels > std::numeric_limits<size_t>::max() / 3U * 2U) {
    return false;
  }
  *size = pixels * 3U / 2U;
  return true;
}

inline const XNativeVideoFrame* GetNativeVideoFrame(
    const XNativeVideoFrame* frame) {
  size_t frame_size = 0;
  if (!frame ||
      frame->struct_size < static_cast<uint32_t>(sizeof(*frame)) ||
      !frame->owner || !frame->retain || !frame->release ||
      !frame->copy_to_nv12 ||
      frame->width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
      frame->height > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
      !GetNv12FrameSize(frame->width, frame->height, &frame_size)) {
    return nullptr;
  }

  switch (frame->type) {
    case XNativeVideoFrameCpuNv12:
      return frame->payload.cpu_nv12.y_plane &&
                     frame->payload.cpu_nv12.uv_plane &&
                     frame->payload.cpu_nv12.y_stride >= frame->width &&
                     frame->payload.cpu_nv12.uv_stride >= frame->width
                 ? frame
                 : nullptr;
    case XNativeVideoFrameCudaNv12:
      return frame->payload.cuda_nv12.y_device_pointer != 0 &&
                     frame->payload.cuda_nv12.uv_device_pointer != 0 &&
                     frame->payload.cuda_nv12.y_stride >= frame->width &&
                     frame->payload.cuda_nv12.uv_stride >= frame->width &&
                     frame->payload.cuda_nv12.context
                 ? frame
                 : nullptr;
    case XNativeVideoFrameCVPixelBuffer:
      return frame->payload.cv_pixel_buffer ? frame : nullptr;
    case XNativeVideoFrameNone:
    default:
      return nullptr;
  }
}

inline const XNativeVideoFrame* GetNativeVideoFrameInput(
    const XVideoFrame* frame) {
  const XNativeVideoFrame* native_frame =
      GetNativeVideoFrame(frame ? frame->native_frame : nullptr);
  return native_frame && frame->width == native_frame->width &&
                 frame->height == native_frame->height
             ? native_frame
             : nullptr;
}

inline bool IsValidVideoFrameInput(const XVideoFrame* frame) {
  if (GetNativeVideoFrameInput(frame)) {
    return true;
  }
  size_t required_size = 0;
  return frame && frame->data &&
         GetNv12FrameSize(frame->width, frame->height, &required_size) &&
         frame->size >= required_size;
}

class NativeVideoFrameRef {
 public:
  NativeVideoFrameRef() = default;

  explicit NativeVideoFrameRef(const XNativeVideoFrame* frame) {
    frame = GetNativeVideoFrame(frame);
    if (frame) {
      frame_ = *frame;
      frame_.struct_size = sizeof(frame_);
      frame_.retain(frame_.owner);
      valid_ = true;
    }
  }

  NativeVideoFrameRef(const NativeVideoFrameRef& other)
      : NativeVideoFrameRef(other.Get()) {}

  NativeVideoFrameRef(NativeVideoFrameRef&& other) noexcept
      : frame_(other.frame_), valid_(std::exchange(other.valid_, false)) {
    other.frame_ = {};
  }

  NativeVideoFrameRef& operator=(NativeVideoFrameRef other) noexcept {
    Swap(other);
    return *this;
  }

  ~NativeVideoFrameRef() { Reset(); }

  void Reset() {
    if (valid_) {
      frame_.release(frame_.owner);
    }
    frame_ = {};
    valid_ = false;
  }

  void Swap(NativeVideoFrameRef& other) noexcept {
    std::swap(frame_, other.frame_);
    std::swap(valid_, other.valid_);
  }

  const XNativeVideoFrame* Get() const {
    return valid_ ? &frame_ : nullptr;
  }

  explicit operator bool() const { return valid_; }

 private:
  XNativeVideoFrame frame_{};
  bool valid_ = false;
};

}  // namespace minirtc


#endif