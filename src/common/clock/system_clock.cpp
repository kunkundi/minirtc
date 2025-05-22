#include "system_clock.h"

#include <time.h>

#include <cstdint>
#include <limits>

#if defined(__linux__)
#include <sys/time.h>
#endif
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

int64_t SystemClock::ConvertToNtpTime(int64_t time_us) {
  constexpr int64_t kMicrosecondsPerSecond = 1000000;
  constexpr uint64_t kNtpFractionalUnit = 0x100000000;  // 2^32
  uint32_t seconds = static_cast<uint32_t>(time_us / kMicrosecondsPerSecond);
  uint32_t fractions =
      static_cast<uint32_t>((time_us % kMicrosecondsPerSecond) *
                            kNtpFractionalUnit / kMicrosecondsPerSecond);

  return seconds * kNtpFractionalUnit + fractions;
}

int64_t SystemClock::CurrentTimeNs() {
  int64_t ticks = -1;  // Default to error case

#if defined(__APPLE__)
  static mach_timebase_info_data_t timebase;
  if (timebase.denom == 0 && mach_timebase_info(&timebase) != KERN_SUCCESS) {
    return -1;  // Error case for macOS timebase info retrieval
  }
  uint64_t abs_time = mach_absolute_time();
  ticks = static_cast<int64_t>((abs_time * timebase.numer) / timebase.denom);

#elif defined(__linux__)
  constexpr int64_t kNumNanosecsPerSec = 1000000000;
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return -1;  // Error case for POSIX clock retrieval
  }
  ticks = static_cast<int64_t>(ts.tv_sec) * kNumNanosecsPerSec +
          static_cast<int64_t>(ts.tv_nsec);

#elif defined(_WIN32)
  static LARGE_INTEGER freq;
  static BOOL initialized = QueryPerformanceFrequency(&freq);
  if (!initialized) return -1;
  LARGE_INTEGER counter;
  if (!QueryPerformanceCounter(&counter)) return -1;
  return static_cast<int64_t>(counter.QuadPart) * 1000000000LL / freq.QuadPart;

#endif

  return ticks;
}

int64_t SystemClock::CurrentTime() { return CurrentTimeNs() / 1000LL; }

int64_t SystemClock::CurrentTimeUs() { return CurrentTimeNs() / 1000LL; }

int64_t SystemClock::CurrentTimeMs() { return CurrentTimeNs() / 1000000LL; }

int64_t SystemClock::CurrentNtpTime() {
  return ConvertToNtpTime(CurrentTimeNs());
}

int64_t SystemClock::CurrentNtpTimeMs() {
  int64_t ntp_ts = ConvertToNtpTime(CurrentTimeNs());
  uint32_t seconds = static_cast<uint32_t>(ntp_ts / 1000000000);
  uint32_t fractions = static_cast<uint32_t>(ntp_ts % 1000000000);

  static constexpr double kNtpFracPerMs = 4.294967296E6;  // 2^32 / 1000.
  const double frac_ms = static_cast<double>(fractions) / kNtpFracPerMs;
  return 1000 * static_cast<int64_t>(seconds) +
         static_cast<int64_t>(frac_ms + 0.5);
}

int64_t SystemClock::CurrentUtcTimeNs() {
#if defined(__linux__)
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#elif defined(_WIN32)
  uint64_t file_time_100ns =
      (static_cast<uint64_t>(file_time.dwHighDateTime) << 32) |
      file_time.dwLowDateTime;
  constexpr uint64_t kUnixEpochFileTimeOffsetIn100ns = 116444736000000000ULL;
  return static_cast<int64_t>(file_time_100ns -
                              kUnixEpochFileTimeOffsetIn100ns) *
         100;
#elif defined(__APPLE__)
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return -1;  // Error case for macOS clock retrieval
  }
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
  return 0;
}

int64_t SystemClock::CurrentUtcTimeUs() { return CurrentUtcTimeNs() / 1000LL; }

int64_t SystemClock::CurrentUtcTimeMs() {
  return CurrentUtcTimeNs() / 1000000LL;
}

int64_t SystemClock::CurrentUtcTime() {
  return CurrentUtcTimeNs() / 1000000000LL;
}

int64_t SystemClock::NtpToUtc(int64_t ntp_time) {
  constexpr int64_t kNtpEpochOffset =
      2208988800LL;  // NTP epoch starts at 1900-01-01, Unix epoch starts at
                     // 1970-01-01
  constexpr int64_t kMicrosecondsPerSecond = 1000000;
  constexpr uint64_t kNtpFractionalUnit = 0x100000000;  // 2^32

  uint32_t seconds = static_cast<uint32_t>(ntp_time / kNtpFractionalUnit);
  uint32_t fractions = static_cast<uint32_t>(ntp_time % kNtpFractionalUnit);

  int64_t unix_seconds = static_cast<int64_t>(seconds) - kNtpEpochOffset;
  int64_t microseconds =
      (static_cast<int64_t>(fractions) * kMicrosecondsPerSecond) /
      kNtpFractionalUnit;

  return unix_seconds * kMicrosecondsPerSecond + microseconds;
}