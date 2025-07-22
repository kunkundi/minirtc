/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-24
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _AUDIO_FRAME_H_
#define _AUDIO_FRAME_H_

#include <cstddef>
#include <cstdint>

namespace minirtc {

class AudioFrame {
 public:
  AudioFrame();
  AudioFrame(size_t size);
  AudioFrame(const uint8_t *buffer, size_t size);
  AudioFrame(const uint8_t *buffer, size_t size, size_t width, size_t height);
  AudioFrame(const AudioFrame &audio_frame);
  AudioFrame(AudioFrame &&audio_frame);
  AudioFrame &operator=(const AudioFrame &audio_frame);
  AudioFrame &operator=(AudioFrame &&audio_frame);

  ~AudioFrame();

 public:
  const uint8_t *Buffer() { return buffer_; }
  size_t Size() { return size_; }

  uint8_t *GetBuffer() { return buffer_; }

 private:
  size_t width_ = 0;
  size_t height_ = 0;
  uint8_t *buffer_ = nullptr;
  size_t size_ = 0;
};
}  // namespace minirtc

#endif