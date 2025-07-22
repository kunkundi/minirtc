/*
 * @Author: DI JUNKUN
 * @Date: 2025-03-12
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _INLINED_VECTOR_H_
#define _INLINED_VECTOR_H_

#include <array>
#include <initializer_list>
#include <iostream>
#include <vector>

namespace minirtc {

template <typename T, size_t N>
class InlinedVector {
 public:
  InlinedVector() : size_(0), use_heap_(false) {}

  void push_back(const T& value) {
    if (!use_heap_ && size_ < N) {
      stack_data_[size_] = value;
    } else {
      if (!use_heap_) {
        heap_data_.reserve(N * 2);
        for (size_t i = 0; i < size_; ++i) {
          heap_data_.push_back(stack_data_[i]);
        }
        use_heap_ = true;
      }
      heap_data_.push_back(value);
    }
    ++size_;
  }

  void assign(size_t n, const T& value) {
    clear();
    for (size_t i = 0; i < n; ++i) {
      push_back(value);
    }
  }

  size_t size() const { return size_; }
  T& operator[](size_t index) {
    return use_heap_ ? heap_data_[index] : stack_data_[index];
  }

  const T& operator[](size_t index) const {
    return use_heap_ ? heap_data_[index] : stack_data_[index];
  }

 private:
  void clear() {
    size_ = 0;
    use_heap_ = false;
    heap_data_.clear();
  }

  size_t size_;
  bool use_heap_;
  std::array<T, N> stack_data_;
  std::vector<T> heap_data_;
};
}  // namespace minirtc

#endif  // _INLINED_VECTOR_H_