/*
 * @Author: DI JUNKUN
 * @Date: 2025-03-25
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RAW_FRAME_H_
#define _RAW_FRAME_H_

#include "video_frame.h"

class RawFrame : public VideoFrame {
 public:
  RawFrame(const uint8_t *buffer, size_t size, uint32_t width, uint32_t height)
      : VideoFrame(buffer, size, width, height) {}
  RawFrame(size_t size, uint32_t width, uint32_t height)
      : VideoFrame(size, width, height) {}
  RawFrame(size_t size) : VideoFrame(size) {}
  RawFrame(const uint8_t *buffer, size_t size) : VideoFrame(buffer, size) {}
  RawFrame() = default;
  ~RawFrame() = default;

  int64_t CapturedTimestamp() const { return captured_timestamp_us_; }

  void SetCapturedTimestamp(int64_t captured_timestamp_us) {
    captured_timestamp_us_ = captured_timestamp_us;
  }

 private:
  int64_t captured_timestamp_us_ = 0;
};

#endif