/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-04
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_EXTENSION_NEGOTIATION_H_
#define _RTP_EXTENSION_NEGOTIATION_H_

#include <cstdint>
#include <optional>
#include <string>

namespace minirtc {

enum class RtpExtensionDirection {
  kSendRecv,
  kSendOnly,
  kRecvOnly,
  kInactive,
};

struct AbsoluteSendTimeExtensionIds {
  std::optional<uint8_t> send_id;
  std::optional<uint8_t> recv_id;
  bool valid = true;
};

// Parses an SDP section produced by the remote endpoint. The returned
// directions are from the local endpoint's perspective.
AbsoluteSendTimeExtensionIds ParseRemoteAbsoluteSendTimeExtension(
    const std::string &sdp_section,
    RtpExtensionDirection default_direction = RtpExtensionDirection::kSendRecv);

// Builds an extmap line whose direction is from the local endpoint's
// perspective. Returns an empty string when the extension is disabled.
std::string BuildLocalAbsoluteSendTimeExtmap(
    const AbsoluteSendTimeExtensionIds &extension_ids);

} // namespace minirtc

#endif
