/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "bwe_defines.h"

namespace minirtc {
namespace webrtc {

const char kBweTypeHistogram[] = "WebRTC.BWE.Types";

RateControlInput::RateControlInput(
    BandwidthUsage bw_state,
    const std::optional<DataRate>& estimated_throughput)
    : bw_state(bw_state), estimated_throughput(estimated_throughput) {}

RateControlInput::~RateControlInput() = default;

}  // namespace webrtc
}  // namespace minirtc