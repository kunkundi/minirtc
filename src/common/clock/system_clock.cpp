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

namespace minirtc {
namespace {

constexpr int64_t kMicrosecondsPerSecond = 1'000'000;
constexpr int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr int64_t kNtpEpochOffsetSeconds = 2'208'988'800LL;
constexpr uint64_t kNtpFractionalUnit = uint64_t{1} << 32;

}  // namespace

SystemClock::SystemClock()
    : monotonic_to_utc_offset_us_(CurrentUtcTimeUs() - CurrentTimeUs()) {}

int64_t SystemClock::CurrentTimeNs() const {
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

int64_t SystemClock::CurrentTime() const { return CurrentTimeUs(); }

int64_t SystemClock::CurrentTimeUs() const { return CurrentTimeNs() / 1000LL; }

int64_t SystemClock::CurrentTimeMs() const {
  return CurrentTimeNs() / 1000000LL;
}

uint64_t SystemClock::CurrentNtpTime() const {
  return MonotonicTimeUsToNtp(CurrentTimeUs());
}

uint64_t SystemClock::MonotonicTimeUsToNtp(
    int64_t monotonic_time_us) const {
  return UtcTimeUsToNtp(monotonic_time_us + monotonic_to_utc_offset_us_);
}

uint64_t SystemClock::UtcTimeUsToNtp(int64_t utc_time_us) const {
  int64_t unix_seconds = utc_time_us / kMicrosecondsPerSecond;
  int64_t microseconds = utc_time_us % kMicrosecondsPerSecond;
  if (microseconds < 0) {
    microseconds += kMicrosecondsPerSecond;
    --unix_seconds;
  }

  const uint32_t ntp_seconds = static_cast<uint32_t>(
      unix_seconds + kNtpEpochOffsetSeconds);
  const uint32_t ntp_fractions = static_cast<uint32_t>(
      static_cast<uint64_t>(microseconds) * kNtpFractionalUnit /
      kMicrosecondsPerSecond);
  return (static_cast<uint64_t>(ntp_seconds) << 32) | ntp_fractions;
}

int64_t SystemClock::NtpToUtcTimeUs(uint64_t ntp_time) const {
  const uint32_t ntp_seconds = static_cast<uint32_t>(ntp_time >> 32);
  const uint32_t ntp_fractions = static_cast<uint32_t>(ntp_time);

  // Select the NTP era closest to the current wall clock. This preserves the
  // 32-bit wire format across the 2036 seconds-field wraparound.
  const int64_t current_ntp_seconds =
      CurrentUtcTimeUs() / kMicrosecondsPerSecond + kNtpEpochOffsetSeconds;
  const int64_t current_era = current_ntp_seconds >> 32;
  int64_t expanded_ntp_seconds =
      (current_era << 32) | static_cast<int64_t>(ntp_seconds);
  constexpr int64_t kNtpEraSeconds = int64_t{1} << 32;
  if (expanded_ntp_seconds - current_ntp_seconds > kNtpEraSeconds / 2) {
    expanded_ntp_seconds -= kNtpEraSeconds;
  } else if (current_ntp_seconds - expanded_ntp_seconds >
             kNtpEraSeconds / 2) {
    expanded_ntp_seconds += kNtpEraSeconds;
  }

  const int64_t fractional_us = static_cast<int64_t>(
      (static_cast<uint64_t>(ntp_fractions) * kMicrosecondsPerSecond +
       kNtpFractionalUnit / 2) /
      kNtpFractionalUnit);
  return (expanded_ntp_seconds - kNtpEpochOffsetSeconds) *
             kMicrosecondsPerSecond +
         fractional_us;
}

int64_t SystemClock::NtpToMonotonicTimeUs(uint64_t ntp_time) const {
  return NtpToUtcTimeUs(ntp_time) - monotonic_to_utc_offset_us_;
}

uint32_t SystemClock::CompactNtp(uint64_t ntp_time) {
  return (static_cast<uint32_t>(ntp_time >> 32) << 16) |
         (static_cast<uint32_t>(ntp_time) >> 16);
}

int64_t SystemClock::CompactNtpIntervalToMilliseconds(uint32_t interval) {
  constexpr uint64_t kCompactNtpUnitsPerSecond = uint64_t{1} << 16;
  return static_cast<int64_t>(
      (static_cast<uint64_t>(interval) * 1000 +
       kCompactNtpUnitsPerSecond / 2) /
      kCompactNtpUnitsPerSecond);
}

int64_t SystemClock::CurrentUtcTimeNs() const {
#if defined(__linux__)
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<int64_t>(ts.tv_sec) * kNanosecondsPerSecond + ts.tv_nsec;
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
  return static_cast<int64_t>(ts.tv_sec) * kNanosecondsPerSecond + ts.tv_nsec;
#endif
  return 0;
}

int64_t SystemClock::CurrentUtcTimeUs() const {
  return CurrentUtcTimeNs() / 1000LL;
}

int64_t SystemClock::CurrentUtcTimeMs() const {
  return CurrentUtcTimeNs() / 1000000LL;
}

int64_t SystemClock::CurrentUtcTime() const {
  return CurrentUtcTimeNs() / kNanosecondsPerSecond;
}
}  // namespace minirtc
