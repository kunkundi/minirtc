/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "api/clock/clock.h"

namespace minirtc {
namespace webrtc {

class WebrtcClock : public Clock {
 public:
  WebrtcClock(std::shared_ptr<SystemClock> system_clock)
      : system_clock_(system_clock) {}
  WebrtcClock() = delete;

  Timestamp CurrentTime() override {
    return Timestamp::Micros(system_clock_->CurrentTimeUs());
  }

  NtpTime ConvertTimestampToNtpTime(Timestamp timestamp) override {
    const uint64_t ntp_time =
        system_clock_->MonotonicTimeUsToNtp(timestamp.us());
    return NtpTime(static_cast<uint32_t>(ntp_time >> 32),
                   static_cast<uint32_t>(ntp_time));
  }

 private:
  std::shared_ptr<SystemClock> system_clock_;
};

Clock* Clock::GetWebrtcClock(std::shared_ptr<SystemClock> system_clock) {
  static Clock* const clock = new WebrtcClock(system_clock);
  return clock;
}

std::shared_ptr<Clock> Clock::GetWebrtcClockShared(
    std::shared_ptr<SystemClock> system_clock) {
  return std::make_shared<WebrtcClock>(system_clock);
}
}  // namespace webrtc
}  // namespace minirtc
