/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-04
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_HEADER_EXTENSION_H_
#define _RTP_HEADER_EXTENSION_H_

#include <cstdint>

namespace minirtc {
namespace rtp {

inline constexpr char kAbsoluteSendTimeUri[] =
    "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time";
inline constexpr uint8_t kPreferredAbsoluteSendTimeExtensionId = 3;

inline bool IsValidOneByteExtensionId(uint32_t id) {
  // RFC 8285 reserves 0 for padding and 15 for future use.
  return id >= 1 && id <= 14;
}

} // namespace rtp
} // namespace minirtc

#endif
