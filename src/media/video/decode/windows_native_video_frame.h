#ifndef MINIRTC_WINDOWS_NATIVE_VIDEO_FRAME_H_
#define MINIRTC_WINDOWS_NATIVE_VIDEO_FRAME_H_

#if defined(_WIN32)

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "minirtc.h"

namespace minirtc {

// Ref-counted tightly packed NV12 storage used by Windows software decoders.
// The initial reference belongs to the decoder callback.
class WindowsNativeNv12Frame final {
 public:
  static WindowsNativeNv12Frame* Create(uint32_t width, uint32_t height);

  XWindowsVideoFrame* Handle() { return &handle_; }
  uint8_t* YPlane() { return storage_.data(); }
  uint8_t* UvPlane() { return storage_.data() + width_ * height_; }
  const uint8_t* Data() const { return storage_.data(); }
  size_t Size() const { return storage_.size(); }

  void AddRef();
  void Release();

 private:
  WindowsNativeNv12Frame(uint32_t width, uint32_t height);
  ~WindowsNativeNv12Frame() = default;

  static void RetainOwner(void* owner);
  static void ReleaseOwner(void* owner);
  static int CopyToCpu(void* owner, uint8_t* destination,
                       size_t destination_size);

  std::atomic<uint32_t> references_{1};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<uint8_t> storage_;
  XWindowsVideoFrame handle_{};
};

}  // namespace minirtc

#endif  // defined(_WIN32)

#endif  // MINIRTC_WINDOWS_NATIVE_VIDEO_FRAME_H_
