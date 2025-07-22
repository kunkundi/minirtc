/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-14
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _LIMITS_BASE_H_
#define _LIMITS_BASE_H_

#include <limits>

namespace minirtc {

template <typename T>
bool IsInfinite(const T& value) {
  return value == std::numeric_limits<T>::min() ||
         value == std::numeric_limits<T>::max();
}

template <typename T>
bool IsFinite(const T& value) {
  return !IsInfinite(value);
}

template <typename T>
bool IsPlusFinite(const T& value) {
  return value == std::numeric_limits<T>::max();
}

template <typename T>
bool IsMinusFinite(const T& value) {
  return value == std::numeric_limits<T>::min();
}
}  // namespace minirtc

#define INT64_T_MAX std::numeric_limits<int64_t>::max()
#define INT64_T_MIN std::numeric_limits<int64_t>::min()

#endif