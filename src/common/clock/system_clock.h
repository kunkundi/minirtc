/*
 * @Author: DI JUNKUN
 * @Date: 2025-02-19
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SYSTEM_CLOCK_H_
#define _SYSTEM_CLOCK_H_

#include <cstdint>

namespace minirtc {

class SystemClock {
 public:
  SystemClock();
  ~SystemClock() = default;

  int64_t CurrentTime() const;
  int64_t CurrentTimeUs() const;
  int64_t CurrentTimeMs() const;
  int64_t CurrentTimeNs() const;

  // NTP timestamps use the RFC 3550 32.32 fixed-point representation: the
  // upper 32 bits are seconds since 1900-01-01 and the lower 32 bits are the
  // fractional part of a second.
  uint64_t CurrentNtpTime() const;
  uint64_t MonotonicTimeUsToNtp(int64_t monotonic_time_us) const;
  int64_t NtpToUtcTimeUs(uint64_t ntp_time) const;
  int64_t NtpToMonotonicTimeUs(uint64_t ntp_time) const;

  static uint32_t CompactNtp(uint64_t ntp_time);
  static int64_t CompactNtpIntervalToMilliseconds(uint32_t interval);

  int64_t CurrentUtcTime() const;
  int64_t CurrentUtcTimeMs() const;
  int64_t CurrentUtcTimeUs() const;
  int64_t CurrentUtcTimeNs() const;

 private:
  uint64_t UtcTimeUsToNtp(int64_t utc_time_us) const;

  const int64_t monotonic_to_utc_offset_us_;
};
}  // namespace minirtc

#endif
