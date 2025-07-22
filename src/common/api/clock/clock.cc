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

#include "rtc_base/time_utils.h"

namespace minirtc {
namespace webrtc {
namespace {

int64_t NtpOffsetUsCalledOnce() {
  constexpr int64_t kNtpJan1970Sec = 2208988800;
  int64_t clock_time = rtc::TimeMicros();
  int64_t utc_time = rtc::TimeUTCMicros();
  return utc_time - clock_time + kNtpJan1970Sec * rtc::kNumMicrosecsPerSec;
}

NtpTime TimeMicrosToNtp(int64_t time_us) {
  static int64_t ntp_offset_us = NtpOffsetUsCalledOnce();

  int64_t time_ntp_us = time_us + ntp_offset_us;

  // Convert seconds to uint32 through uint64 for a well-defined cast.
  // A wrap around, which will happen in 2036, is expected for NTP time.
  uint32_t ntp_seconds =
      static_cast<uint64_t>(time_ntp_us / rtc::kNumMicrosecsPerSec);

  // Scale fractions of the second to NTP resolution.
  constexpr int64_t kNtpFractionsInSecond = 1LL << 32;
  int64_t us_fractions = time_ntp_us % rtc::kNumMicrosecsPerSec;
  uint32_t ntp_fractions =
      us_fractions * kNtpFractionsInSecond / rtc::kNumMicrosecsPerSec;

  return NtpTime(ntp_seconds, ntp_fractions);
}

}  // namespace

class WebrtcClock : public Clock {
 public:
  WebrtcClock(std::shared_ptr<SystemClock> system_clock)
      : system_clock_(system_clock) {}
  WebrtcClock() = delete;

  Timestamp CurrentTime() override {
    return Timestamp::Micros(system_clock_->CurrentTimeUs());
  }

  NtpTime ConvertTimestampToNtpTime(Timestamp timestamp) override {
    int64_t time_us = timestamp.us();
    constexpr int64_t kNtpJan1970Sec = 2208988800;
    int64_t clock_time = system_clock_->CurrentTimeUs();
    int64_t utc_time = system_clock_->CurrentUtcTimeUs();
    static int64_t ntp_offset_us =
        utc_time - clock_time + kNtpJan1970Sec * rtc::kNumMicrosecsPerSec;

    int64_t time_ntp_us = time_us + ntp_offset_us;

    // Convert seconds to uint32 through uint64 for a well-defined cast.
    // A wrap around, which will happen in 2036, is expected for NTP time.
    uint32_t ntp_seconds =
        static_cast<uint64_t>(time_ntp_us / rtc::kNumMicrosecsPerSec);

    // Scale fractions of the second to NTP resolution.
    constexpr int64_t kNtpFractionsInSecond = 1LL << 32;
    int64_t us_fractions = time_ntp_us % rtc::kNumMicrosecsPerSec;
    uint32_t ntp_fractions =
        us_fractions * kNtpFractionsInSecond / rtc::kNumMicrosecsPerSec;

    return NtpTime(ntp_seconds, ntp_fractions);
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