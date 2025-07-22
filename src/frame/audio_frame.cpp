#include "audio_frame.h"

#include <cstring>
#include <utility>

namespace minirtc {

AudioFrame::AudioFrame() {}

AudioFrame::AudioFrame(size_t size) {
  buffer_ = new uint8_t[size];
  size_ = size;
  width_ = 0;
  height_ = 0;
}

AudioFrame::AudioFrame(const uint8_t *buffer, size_t size) {
  buffer_ = new uint8_t[size];
  memcpy(buffer_, buffer, size);
  size_ = size;
  width_ = 0;
  height_ = 0;
}

AudioFrame::AudioFrame(const uint8_t *buffer, size_t size, size_t width,
                       size_t height) {
  buffer_ = new uint8_t[size];
  memcpy(buffer_, buffer, size);
  size_ = size;
  width_ = width;
  height_ = height;
}

AudioFrame::AudioFrame(const AudioFrame &audio_frame) {
  if (audio_frame.size_ > 0) {
    buffer_ = new uint8_t[audio_frame.size_];
    memcpy(buffer_, audio_frame.buffer_, audio_frame.size_);
    size_ = audio_frame.size_;
    width_ = audio_frame.width_;
    height_ = audio_frame.height_;
  }
}

AudioFrame::AudioFrame(AudioFrame &&audio_frame)
    : buffer_((uint8_t *)std::move(audio_frame.buffer_)),
      size_(audio_frame.size_),
      width_(audio_frame.width_),
      height_(audio_frame.height_) {
  audio_frame.buffer_ = nullptr;
  audio_frame.size_ = 0;
  audio_frame.width_ = 0;
  audio_frame.height_ = 0;
}

AudioFrame &AudioFrame::operator=(const AudioFrame &audio_frame) {
  if (&audio_frame != this) {
    if (buffer_) {
      delete buffer_;
      buffer_ = nullptr;
    }
    buffer_ = new uint8_t[audio_frame.size_];
    memcpy(buffer_, audio_frame.buffer_, audio_frame.size_);
    size_ = audio_frame.size_;
    width_ = audio_frame.width_;
    height_ = audio_frame.height_;
  }
  return *this;
}

AudioFrame &AudioFrame::operator=(AudioFrame &&audio_frame) {
  if (&audio_frame != this) {
    buffer_ = std::move(audio_frame.buffer_);
    audio_frame.buffer_ = nullptr;
    size_ = audio_frame.size_;
    audio_frame.size_ = 0;
    width_ = audio_frame.width_;
    audio_frame.width_ = 0;
    height_ = audio_frame.height_;
    audio_frame.height_ = 0;
  }
  return *this;
}

AudioFrame::~AudioFrame() {
  if (buffer_) {
    delete buffer_;
    buffer_ = nullptr;
  }

  size_ = 0;
  width_ = 0;
  height_ = 0;
}
}  // namespace minirtc