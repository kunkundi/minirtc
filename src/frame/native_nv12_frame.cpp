#include "native_nv12_frame.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace minirtc {
namespace {

bool Nv12Size(uint32_t width, uint32_t height, size_t* size) {
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

}  // namespace

std::shared_ptr<NativeNv12FramePool> NativeNv12FramePool::Create(
    size_t max_cached_buffers) {
  return std::shared_ptr<NativeNv12FramePool>(
      new NativeNv12FramePool(max_cached_buffers));
}

NativeNv12FramePool::NativeNv12FramePool(size_t max_cached_buffers)
    : max_cached_buffers_(max_cached_buffers) {}

NativeNv12Frame* NativeNv12FramePool::Acquire(uint32_t width,
                                               uint32_t height) {
  size_t required_size = 0;
  if (!Nv12Size(width, height, &required_size)) {
    return nullptr;
  }

  std::vector<uint8_t> storage;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto best = free_buffers_.end();
    for (auto it = free_buffers_.begin(); it != free_buffers_.end(); ++it) {
      if (it->capacity() < required_size) {
        continue;
      }
      if (best == free_buffers_.end() || it->capacity() < best->capacity()) {
        best = it;
      }
    }
    if (best == free_buffers_.end() && !free_buffers_.empty()) {
      best = std::max_element(
          free_buffers_.begin(), free_buffers_.end(),
          [](const auto& lhs, const auto& rhs) {
            return lhs.capacity() < rhs.capacity();
          });
    }
    if (best != free_buffers_.end()) {
      storage = std::move(*best);
      free_buffers_.erase(best);
    }
  }

  try {
    storage.resize(required_size);
    return new NativeNv12Frame(shared_from_this(), std::move(storage), width,
                               height);
  } catch (...) {
    return nullptr;
  }
}

void NativeNv12FramePool::Recycle(std::vector<uint8_t> storage) {
  if (max_cached_buffers_ == 0 || storage.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (free_buffers_.size() < max_cached_buffers_) {
    free_buffers_.push_back(std::move(storage));
    return;
  }
  auto smallest = std::min_element(
      free_buffers_.begin(), free_buffers_.end(),
      [](const auto& lhs, const auto& rhs) {
        return lhs.capacity() < rhs.capacity();
      });
  if (smallest != free_buffers_.end() &&
      smallest->capacity() < storage.capacity()) {
    *smallest = std::move(storage);
  }
}

NativeNv12Frame::NativeNv12Frame(std::shared_ptr<NativeNv12FramePool> pool,
                                 std::vector<uint8_t> storage, uint32_t width,
                                 uint32_t height)
    : pool_(std::move(pool)),
      storage_(std::move(storage)),
      width_(width),
      height_(height) {
  descriptor_.struct_size = sizeof(descriptor_);
  descriptor_.type = MiniRtcNativeVideoFrameCpuNv12;
  descriptor_.width = width_;
  descriptor_.height = height_;
  descriptor_.payload.cpu_nv12 = {YPlane(), UvPlane(), width_, width_};
  descriptor_.owner = this;
  descriptor_.retain = &RetainOwner;
  descriptor_.release = &ReleaseOwner;
  descriptor_.copy_to_nv12 = &CopyToNv12;
}

NativeNv12Frame::~NativeNv12Frame() {
  if (pool_) {
    pool_->Recycle(std::move(storage_));
  }
}

void NativeNv12Frame::AddRef() {
  references_.fetch_add(1, std::memory_order_relaxed);
}

void NativeNv12Frame::Release() {
  if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    delete this;
  }
}

void NativeNv12Frame::RetainOwner(void* owner) {
  static_cast<NativeNv12Frame*>(owner)->AddRef();
}

void NativeNv12Frame::ReleaseOwner(void* owner) {
  static_cast<NativeNv12Frame*>(owner)->Release();
}

int NativeNv12Frame::CopyToNv12(void* owner, uint8_t* destination,
                                size_t destination_size) {
  auto* frame = static_cast<NativeNv12Frame*>(owner);
  if (!frame || !destination || destination_size < frame->Size()) {
    return -1;
  }
  std::memcpy(destination, frame->Data(), frame->Size());
  return 0;
}

}  // namespace minirtc
