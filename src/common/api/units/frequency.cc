/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "frequency.h"

#include <cstdint>
#include <string>

namespace minirtc {
namespace webrtc {
std::string ToString(Frequency value) {
  if (value.IsPlusInfinity()) {
    return "+inf Hz";
  } else if (value.IsMinusInfinity()) {
    return "-inf Hz";
  } else if (value.millihertz<int64_t>() % 1000 != 0) {
    return std::to_string(value.hertz<double>()) + " Hz";
  } else {
    return std::to_string(value.hertz<int64_t>()) + " Hz";
  }
}
}  // namespace webrtc
}  // namespace minirtc