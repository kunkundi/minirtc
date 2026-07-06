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
#include <mmsystem.h>
#endif

namespace minirtc {

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
  int64_t ticks;
#if defined(__APPLE__)
  static mach_timebase_info_data_t timebase;
  if (timebase.denom == 0) {
    // Get the timebase if this is the first time we run.
    // Recommended by Apple's QA1398.
    if (mach_timebase_info(&timebase) != KERN_SUCCESS) {
    }
  }
  // Use timebase to convert absolute time tick units into nanoseconds.
  const auto mul = [](uint64_t a, uint32_t b) -> int64_t {
    return static_cast<int64_t>(a * b);
  };
  ticks = mul(mach_absolute_time(), timebase.numer) / timebase.denom;
#elif defined(__linux__)
  constexpr int64_t kNumNanosecsPerSec = 1000000000;
  struct timespec ts;
  // TODO(deadbeef): Do we need to handle the case when CLOCK_MONOTONIC is not
  // supported?
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ticks = kNumNanosecsPerSec * static_cast<int64_t>(ts.tv_sec) +
          static_cast<int64_t>(ts.tv_nsec);
#elif defined(_WIN32)
  static volatile LONG last_timegettime = 0;
  static volatile int64_t num_wrap_timegettime = 0;
  volatile LONG* last_timegettime_ptr = &last_timegettime;
  DWORD now = timeGetTime();
  // Atomically update the last gotten time
  DWORD old = InterlockedExchange(last_timegettime_ptr, now);
  if (now < old) {
    // If now is earlier than old, there may have been a race between threads.
    // 0x0fffffff ~3.1 days, the code will not take that long to execute
    // so it must have been a wrap around.
    if (old > 0xf0000000 && now < 0x0fffffff) {
      num_wrap_timegettime++;
    }
  }
  ticks = now + (num_wrap_timegettime << 32);
  // TODO(deadbeef): Calculate with nanosecond precision. Otherwise, we're
  // just wasting a multiply and divide when doing Time() on Windows.
  ticks = ticks * 1000000LL;  // Convert milliseconds to nanoseconds
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
  FILETIME file_time;
  GetSystemTimeAsFileTime(&file_time);
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
}  // namespace minirtc
