#include "minirtc.h"
#include "../pc/join_failure.h"

MiniRtcJoinFailure ParseMiniRtcJoinFailure(const char* message, size_t size) {
  const MiniRtcJoinFailure unknown{MiniRtcJoinFailureKind::Unknown, Failed, 0};
  if (!message) return unknown;
  try {
    const auto value = nlohmann::json::parse(message, message + size, nullptr, false);
    if (value.is_discarded()) return unknown;
    const auto result = minirtc::ParseJoinFailure(value);
    return {result.kind, result.status, result.retry_after};
  } catch (...) { return unknown; }
}
