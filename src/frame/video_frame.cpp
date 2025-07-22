#include "video_frame.h"

#include <cstring>
#include <utility>

namespace minirtc {

VideoFrame::VideoFrame() {}

VideoFrame::VideoFrame(size_t size) : buffer_(size) {
  size_ = size;
  width_ = 0;
  height_ = 0;
}

VideoFrame::VideoFrame(size_t size, uint32_t width, uint32_t height)
    : buffer_(size) {
  size_ = size;
  width_ = width;
  height_ = height;
}

VideoFrame::VideoFrame(const uint8_t *buffer, size_t size)
    : buffer_(buffer, size) {
  size_ = size;
  width_ = 0;
  height_ = 0;
}

VideoFrame::VideoFrame(const uint8_t *buffer, size_t size, uint32_t width,
                       uint32_t height)
    : buffer_(buffer, size) {
  size_ = size;
  width_ = width;
  height_ = height;
}

VideoFrame::VideoFrame(const VideoFrame &video_frame) = default;

VideoFrame::VideoFrame(VideoFrame &&video_frame) = default;

VideoFrame &VideoFrame::operator=(const VideoFrame &video_frame) = default;

VideoFrame &VideoFrame::operator=(VideoFrame &&video_frame) = default;

VideoFrame::~VideoFrame() = default;

void VideoFrame::UpdateBuffer(const uint8_t *new_buffer, size_t new_size) {
  if (new_size > size_) {
    buffer_ = CopyOnWriteBuffer(new_buffer, new_size);
    size_ = new_size;
  } else {
    std::memcpy((void *)buffer_.data(), new_buffer, new_size);
    size_ = new_size;
  }
}
}  // namespace minirtc