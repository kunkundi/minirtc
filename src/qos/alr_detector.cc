/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "alr_detector.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>

#include "rtc_base/time_utils.h"

namespace minirtc {
namespace webrtc {

AlrDetector::AlrDetector(AlrDetectorConfig config)
    : conf_(config), alr_budget_(0, true) {}

AlrDetector::AlrDetector() : alr_budget_(0, true) {}
AlrDetector::~AlrDetector() {}

void AlrDetector::OnBytesSent(size_t bytes_sent, int64_t send_time_ms) {
  if (!last_send_time_ms_.has_value()) {
    last_send_time_ms_ = send_time_ms;
    // Since the duration for sending the bytes is unknwon, return without
    // updating alr state.
    return;
  }
  int64_t delta_time_ms = send_time_ms - *last_send_time_ms_;
  last_send_time_ms_ = send_time_ms;

  alr_budget_.UseBudget(bytes_sent);
  alr_budget_.IncreaseBudget(delta_time_ms);
  bool state_changed = false;
  if (alr_budget_.budget_ratio() > conf_.start_budget_level_ratio &&
      !alr_started_time_ms_) {
    alr_started_time_ms_.emplace(rtc::TimeMillis());
    state_changed = true;
  } else if (alr_budget_.budget_ratio() < conf_.stop_budget_level_ratio &&
             alr_started_time_ms_) {
    state_changed = true;
    alr_started_time_ms_.reset();
  }
}

void AlrDetector::SetEstimatedBitrate(int bitrate_bps) {
  int target_rate_kbps =
      static_cast<double>(bitrate_bps) * conf_.bandwidth_usage_ratio / 1000;
  alr_budget_.set_target_rate_kbps(target_rate_kbps);
}

std::optional<int64_t> AlrDetector::GetApplicationLimitedRegionStartTime()
    const {
  return alr_started_time_ms_;
}

}  // namespace webrtc
}  // namespace minirtc