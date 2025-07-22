/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-14
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CONSTRAIN_H_
#define _CONSTRAIN_H_

#include <optional>
#include <string>

namespace minirtc {
template <typename T>
class Constrained {
 public:
  Constrained(T default_value, std::optional<T> lower_limit,
              std::optional<T> upper_limit)
      : value_(default_value),
        lower_limit_(lower_limit),
        upper_limit_(upper_limit) {}
  T Get() const { return value_; }
  operator T() const { return Get(); }
  const T* operator->() const { return &value_; }

 private:
  T value_;
  std::optional<T> lower_limit_;
  std::optional<T> upper_limit_;
};
}  // namespace minirtc

#endif