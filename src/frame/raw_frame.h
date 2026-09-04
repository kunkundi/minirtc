/*
 * @Author: DI JUNKUN
 * @Date: 2025-03-25
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RAW_FRAME_H_
#define _RAW_FRAME_H_

#include <utility>

#include "native_video_frame.h"
#include "video_frame.h"

namespace minirtc {

class RawFrame : public VideoFrame {
 public:
  RawFrame(const uint8_t *buffer, size_t size, uint32_t width, uint32_t height)
      : VideoFrame(buffer, size, width, height) {}
  RawFrame(size_t size, uint32_t width, uint32_t height)
      : VideoFrame(size, width, height) {}
  RawFrame(size_t size) : VideoFrame(size) {}
  RawFrame(const uint8_t *buffer, size_t size) : VideoFrame(buffer, size) {}
  explicit RawFrame(const MiniRtcNativeVideoFrame& native_frame)
      : native_frame_(&native_frame) {
    if (const auto* frame = native_frame_.Get()) {
      size_t size = 0;
      GetNv12FrameSize(frame->width, frame->height, &size);
      SetSize(size);
      SetWidth(frame->width);
      SetHeight(frame->height);
    }
  }
  RawFrame() = default;
  RawFrame(const RawFrame&) = default;
  RawFrame(RawFrame&&) noexcept = default;
  RawFrame& operator=(const RawFrame&) = default;
  RawFrame& operator=(RawFrame&&) noexcept = default;
  ~RawFrame() = default;

  int64_t CapturedTimestamp() const { return captured_timestamp_us_; }

  void SetCapturedTimestamp(int64_t captured_timestamp_us) {
    captured_timestamp_us_ = captured_timestamp_us;
  }

  const MiniRtcNativeVideoFrame* NativeFrame() const {
    return native_frame_.Get();
  }

  bool MaterializeNativeFrame() {
    const auto* native_frame = native_frame_.Get();
    if (!native_frame) {
      return Buffer() != nullptr;
    }

    size_t required_size = 0;
    if (!GetNv12FrameSize(native_frame->width, native_frame->height,
                          &required_size)) {
      return false;
    }
    VideoFrame cpu_frame(required_size, native_frame->width,
                         native_frame->height);
    if (!cpu_frame.MutableBuffer() ||
        native_frame->copy_to_nv12(native_frame->owner,
                                   cpu_frame.MutableBuffer(),
                                   required_size) != 0) {
      return false;
    }
    VideoFrame::operator=(std::move(cpu_frame));
    native_frame_.Reset();
    return true;
  }

 private:
  int64_t captured_timestamp_us_ = 0;
  NativeVideoFrameRef native_frame_;
};
}  // namespace minirtc

#endif
