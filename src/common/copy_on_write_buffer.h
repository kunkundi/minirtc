/*
 * @Author: DI JUNKUN
 * @Date: 2025-05-22
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _COPY_ON_WRITE_BUFFER_H_
#define _COPY_ON_WRITE_BUFFER_H_

#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace minirtc {

class CopyOnWriteBuffer {
 public:
  CopyOnWriteBuffer() = default;

  explicit CopyOnWriteBuffer(size_t size) {
    Allocate(size);
    block_->size = size;
  }

  CopyOnWriteBuffer(const uint8_t* data, size_t size) {
    Allocate(size);
    std::memcpy(block_->data, data, size);
    block_->size = size;
  }

  CopyOnWriteBuffer(const CopyOnWriteBuffer&) = default;
  CopyOnWriteBuffer(CopyOnWriteBuffer&&) noexcept = default;
  CopyOnWriteBuffer& operator=(const CopyOnWriteBuffer&) = default;
  CopyOnWriteBuffer& operator=(CopyOnWriteBuffer&&) noexcept = default;

  void SetData(const uint8_t* data, size_t size) {
    Allocate(size);
    std::memcpy(block_->data, data, size);
    block_->size = size;
  }

  const uint8_t* data() const { return block_ ? block_->data : nullptr; }

  uint8_t* MutableData() {
    EnsureUnique();
    return block_ ? block_->data : nullptr;
  }

  size_t size() const { return block_ ? block_->size : 0; }

  size_t capacity() const { return block_ ? block_->capacity : 0; }

  uint8_t& operator[](size_t index) {
    if (!block_ || index >= block_->size) {
      throw std::out_of_range("index out of range");
    }
    EnsureUnique();
    return block_->data[index];
  }

  const uint8_t& operator[](size_t index) const {
    if (!block_ || index >= block_->size) {
      throw std::out_of_range("index out of range");
    }
    return block_->data[index];
  }

 private:
  struct BufferBlock {
    size_t size = 0;
    size_t capacity = 0;
    uint8_t* data = nullptr;

    ~BufferBlock() { std::free(data); }
  };

  void Allocate(size_t capacity) {
    if (capacity == 0) return;

    auto block = std::make_shared<BufferBlock>();
    block->capacity = capacity;
    block->data = static_cast<uint8_t*>(std::malloc(capacity));
    if (!block->data) throw std::bad_alloc();
    block->size = 0;
    block_ = std::move(block);
  }

  void EnsureUnique() {
    if (!block_ || block_.use_count() == 1) return;
    auto new_block = std::make_shared<BufferBlock>();
    new_block->capacity = block_->capacity;
    new_block->data = static_cast<uint8_t*>(std::malloc(new_block->capacity));
    if (!new_block->data) throw std::bad_alloc();
    std::memcpy(new_block->data, block_->data, block_->size);
    new_block->size = block_->size;
    block_ = std::move(new_block);
  }

  std::shared_ptr<BufferBlock> block_;
};
}  // namespace minirtc

#endif
