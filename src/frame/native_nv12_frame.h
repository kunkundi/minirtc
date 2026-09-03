/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-03
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _NATIVE_NV12_FRAME_H_
#define _NATIVE_NV12_FRAME_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "minirtc.h"

namespace minirtc {

class NativeNv12Frame;

// Reuses the large pixel allocations owned by decoded native frames. Frame
// wrappers are short-lived, while a bounded number of pixel buffers survive
// across frames and resolution changes.
class NativeNv12FramePool final
    : public std::enable_shared_from_this<NativeNv12FramePool> {
 public:
  static std::shared_ptr<NativeNv12FramePool> Create(
      size_t max_cached_buffers = 4);

  NativeNv12Frame* Acquire(uint32_t width, uint32_t height);

 private:
  explicit NativeNv12FramePool(size_t max_cached_buffers);
  void Recycle(std::vector<uint8_t> storage);

  friend class NativeNv12Frame;

  const size_t max_cached_buffers_;
  std::mutex mutex_;
  std::vector<std::vector<uint8_t>> free_buffers_;
};

class NativeNv12Frame final {
 public:
  XNativeVideoFrame* Descriptor() { return &descriptor_; }
  uint8_t* YPlane() { return storage_.data(); }
  uint8_t* UvPlane() {
    return storage_.data() + static_cast<size_t>(width_) * height_;
  }
  const uint8_t* Data() const { return storage_.data(); }
  size_t Size() const { return storage_.size(); }

  void AddRef();
  void Release();

 private:
  friend class NativeNv12FramePool;

  NativeNv12Frame(std::shared_ptr<NativeNv12FramePool> pool,
                  std::vector<uint8_t> storage, uint32_t width,
                  uint32_t height);
  ~NativeNv12Frame();

  static void RetainOwner(void* owner);
  static void ReleaseOwner(void* owner);
  static int CopyToNv12(void* owner, uint8_t* destination,
                        size_t destination_size);

  std::atomic<uint32_t> references_{1};
  std::shared_ptr<NativeNv12FramePool> pool_;
  std::vector<uint8_t> storage_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  XNativeVideoFrame descriptor_{};
};

}  // namespace minirtc

#endif