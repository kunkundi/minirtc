#ifndef _JOIN_FAILURE_H_
#define _JOIN_FAILURE_H_

#include <limits>
#include <nlohmann/json.hpp>
#include <string_view>

#include "minirtc.h"

namespace minirtc {

enum class JoinFailureKind {
  PasswordRequired,
  AuthenticationFailed,
  NoSuchTransmission,
  RemoteUnavailable,
  Throttled,
  Busy,
  NotAuthenticated,
  Unknown
};

inline JoinFailureKind ClassifyJoinFailure(std::string_view reason,
                                           std::string_view code = {}) {
  if (!code.empty()) {
    if (code == "PASSWORD_REQUIRED") return JoinFailureKind::PasswordRequired;
    if (code == "AUTHENTICATION_FAILED")
      return JoinFailureKind::AuthenticationFailed;
    if (code == "REMOTE_UNAVAILABLE") return JoinFailureKind::RemoteUnavailable;
    if (code == "AUTHENTICATION_THROTTLED") return JoinFailureKind::Throttled;
    if (code == "CREDENTIAL_SERVICE_BUSY" || code == "CREDENTIAL_RATE_LIMITED")
      return JoinFailureKind::Busy;
    if (code == "NOT_AUTHENTICATED") return JoinFailureKind::NotAuthenticated;
    // A legacy reason must not override a new error code we don't understand.
    return JoinFailureKind::Unknown;
  }
  if (reason == "Incorrect password" || reason == "Authentication failed")
    return JoinFailureKind::AuthenticationFailed;
  if (reason == "No such transmission id")
    return JoinFailureKind::NoSuchTransmission;
  if (reason == "Remote unavailable") return JoinFailureKind::RemoteUnavailable;
  if (reason == "Too many authentication attempts")
    return JoinFailureKind::Throttled;
  if (reason == "Credential service busy" ||
      reason == "Too many credential requests")
    return JoinFailureKind::Busy;
  if (reason == "Not authenticated") return JoinFailureKind::NotAuthenticated;
  return JoinFailureKind::Unknown;
}

inline ConnectionStatus JoinFailureStatus(std::string_view reason,
                                          std::string_view code = {}) {
  switch (ClassifyJoinFailure(reason, code)) {
    case JoinFailureKind::PasswordRequired:
    case JoinFailureKind::AuthenticationFailed:
      return ConnectionStatus::IncorrectPassword;
    case JoinFailureKind::NoSuchTransmission:
      return ConnectionStatus::NoSuchTransmissionId;
    case JoinFailureKind::RemoteUnavailable:
      return ConnectionStatus::RemoteUnavailable;
    default:
      return ConnectionStatus::Failed;
  }
}

struct JoinFailure {
  JoinFailureKind kind = JoinFailureKind::Unknown;
  ConnectionStatus status = ConnectionStatus::Failed;
  int retry_after = 0;
};

inline JoinFailure ParseJoinFailure(const nlohmann::json& message) {
  auto field = [&](const char* key) -> std::string_view {
    const auto it = message.find(key);
    return it != message.end() && it->is_string()
               ? std::string_view(it->get_ref<const std::string&>())
               : std::string_view{};
  };
  JoinFailure failure;
  failure.kind = ClassifyJoinFailure(field("reason"), field("error_code"));
  failure.status = JoinFailureStatus(field("reason"), field("error_code"));
  const auto retry = message.find("retry_after");
  if ((failure.kind == JoinFailureKind::Throttled ||
       failure.kind == JoinFailureKind::Busy) &&
      retry != message.end() && retry->is_number_integer() && *retry > 0 &&
      *retry <= std::numeric_limits<int>::max()) {
    failure.retry_after = retry->get<int>();
  }
  return failure;
}

}  // namespace minirtc

#endif
