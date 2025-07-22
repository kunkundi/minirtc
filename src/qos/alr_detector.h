/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_CONGESTION_CONTROLLER_GOOG_CC_ALR_DETECTOR_H_
#define MODULES_CONGESTION_CONTROLLER_GOOG_CC_ALR_DETECTOR_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>

#include "interval_budget.h"

namespace minirtc {
namespace webrtc {

class RtcEventLog;

struct AlrDetectorConfig {
  // Sent traffic ratio as a function of network capacity used to determine
  // application-limited region. ALR region start when bandwidth usage drops
  // below kAlrStartUsageRatio and ends when it raises above
  // kAlrEndUsageRatio. NOTE: This is intentionally conservative at the moment
  // until BW adjustments of application limited region is fine tuned.
  double bandwidth_usage_ratio = 0.65;
  double start_budget_level_ratio = 0.80;
  double stop_budget_level_ratio = 0.50;
};
// Application limited region detector is a class that utilizes signals of
// elapsed time and bytes sent to estimate whether network traffic is
// currently limited by the application's ability to generate traffic.
//
// AlrDetector provides a signal that can be utilized to adjust
// estimate bandwidth.
// Note: This class is not thread-safe.
class AlrDetector {
 public:
  AlrDetector(AlrDetectorConfig config);
  AlrDetector();
  ~AlrDetector();

  void OnBytesSent(size_t bytes_sent, int64_t send_time_ms);

  // Set current estimated bandwidth.
  void SetEstimatedBitrate(int bitrate_bps);

  // Returns time in milliseconds when the current application-limited region
  // started or empty result if the sender is currently not application-limited.
  std::optional<int64_t> GetApplicationLimitedRegionStartTime() const;

 private:
  friend class GoogCcStatePrinter;
  const AlrDetectorConfig conf_;

  std::optional<int64_t> last_send_time_ms_;

  IntervalBudget alr_budget_;
  std::optional<int64_t> alr_started_time_ms_;
};
}  // namespace webrtc
}  // namespace minirtc

#endif  // MODULES_CONGESTION_CONTROLLER_GOOG_CC_ALR_DETECTOR_H_