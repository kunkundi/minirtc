#include "windows_native_video_frame.h"

#if defined(_WIN32)

#include <cstring>
#include <limits>
#include <new>

namespace minirtc {

WindowsNativeNv12Frame* WindowsNativeNv12Frame::Create(uint32_t width,
                                                        uint32_t height) {
  if (width == 0 || height == 0 || (width & 1U) != 0 ||
      (height & 1U) != 0 || static_cast<size_t>(width) >
                                  std::numeric_limits<size_t>::max() / height) {
    return nullptr;
  }
  const size_t pixel_count = static_cast<size_t>(width) * height;
  if (pixel_count > std::numeric_limits<size_t>::max() / 3U * 2U) {
    return nullptr;
  }
  try {
    return new WindowsNativeNv12Frame(width, height);
  } catch (...) {
    return nullptr;
  }
}

WindowsNativeNv12Frame::WindowsNativeNv12Frame(uint32_t width,
                                                uint32_t height)
    : width_(width),
      height_(height),
      storage_(static_cast<size_t>(width) * height * 3U / 2U) {
  handle_.struct_size = sizeof(handle_);
  handle_.memory_type = XWindowsVideoFrameMemoryCpu;
  handle_.y_plane = YPlane();
  handle_.uv_plane = UvPlane();
  handle_.size = storage_.size();
  handle_.width = width_;
  handle_.height = height_;
  handle_.y_stride = width_;
  handle_.uv_stride = width_;
  handle_.owner = this;
  handle_.retain = &RetainOwner;
  handle_.release = &ReleaseOwner;
  handle_.copy_to_cpu = &CopyToCpu;
}

void WindowsNativeNv12Frame::AddRef() {
  references_.fetch_add(1, std::memory_order_relaxed);
}

void WindowsNativeNv12Frame::Release() {
  if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete this;
  }
}

void WindowsNativeNv12Frame::RetainOwner(void* owner) {
  static_cast<WindowsNativeNv12Frame*>(owner)->AddRef();
}

void WindowsNativeNv12Frame::ReleaseOwner(void* owner) {
  static_cast<WindowsNativeNv12Frame*>(owner)->Release();
}

int WindowsNativeNv12Frame::CopyToCpu(void* owner, uint8_t* destination,
                                      size_t destination_size) {
  auto* frame = static_cast<WindowsNativeNv12Frame*>(owner);
  if (!frame || !destination || destination_size < frame->Size()) {
    return -1;
  }
  std::memcpy(destination, frame->Data(), frame->Size());
  return 0;
}

}  // namespace minirtc

#endif  // defined(_WIN32)
