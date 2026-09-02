/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-23
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_DEFINES_H_
#define _RTP_DEFINES_H_

#include <cstddef>
#include <cstdint>

#define DEFAULT_MTU 1500
#define MAX_NALU_LEN 1150

namespace minirtc {
namespace rtp {

typedef enum {
  UNDEFINED = 0,
  H264 = 96,
  H264_FEC_SOURCE = 97,
  H264_FEC_REPAIR = 98,
  AV1 = 99,
  OPUS = 111,
  RTX = 127,
  DATA = 120,
  KCP = 121
} PAYLOAD_TYPE;

typedef struct {
  uint8_t forbidden_bit : 1;
  uint8_t nal_reference_idc : 2;
  uint8_t nal_unit_type : 5;
} FU_INDICATOR;

typedef struct {
  uint8_t start : 1;
  uint8_t end : 1;
  uint8_t remain_bit : 1;
  uint8_t nal_unit_type : 5;
} FU_HEADER;

typedef enum { UNKNOWN = 0, NALU = 1, FU_A = 28, FU_B = 29 } NAL_UNIT_TYPE;

constexpr int64_t kVideoPayloadTypeFrequency = 90000;
constexpr int64_t kMsToRtpTimestamp = 90;
constexpr int64_t kMicrosecondsPerSecond = 1000000;

// RTP video timestamps always use the standard 90 kHz clock. Keep the
// conversion here so packetizers cannot accidentally put microseconds in the
// RTP header (which makes a 60 fps interval look like roughly 185 ms).
inline uint32_t VideoTimestampFromMicroseconds(int64_t timestamp_us) {
  if (timestamp_us <= 0) {
    return 0;
  }
  const uint64_t value = static_cast<uint64_t>(timestamp_us);
  const uint64_t seconds = value / kMicrosecondsPerSecond;
  const uint64_t remainder_us = value % kMicrosecondsPerSecond;
  const uint64_t timestamp =
      seconds * kVideoPayloadTypeFrequency +
      (remainder_us * kVideoPayloadTypeFrequency +
       kMicrosecondsPerSecond / 2) /
          kMicrosecondsPerSecond;
  return static_cast<uint32_t>(timestamp);
}

inline int64_t VideoTimestampToMicroseconds(int64_t timestamp) {
  return timestamp * kMicrosecondsPerSecond /
         kVideoPayloadTypeFrequency;
}
}  // namespace rtp
}  // namespace minirtc
#endif
