#include "rtp_extension_negotiation.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <sstream>
#include <string_view>

#include "rtp_header_extension.h"

namespace minirtc {
namespace {

std::string Trim(std::string value) {
  const auto is_space = [](unsigned char c) { return std::isspace(c); };
  value.erase(value.begin(),
              std::find_if_not(value.begin(), value.end(), is_space));
  value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(),
              value.end());
  return value;
}

std::optional<uint32_t> ParseUint32(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  uint32_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return parsed;
}

AbsoluteSendTimeExtensionIds InvalidExtensionIds() {
  AbsoluteSendTimeExtensionIds ids;
  ids.valid = false;
  return ids;
}

std::optional<RtpExtensionDirection>
ParseDirection(std::string_view direction) {
  if (direction == "sendrecv") {
    return RtpExtensionDirection::kSendRecv;
  }
  if (direction == "sendonly") {
    return RtpExtensionDirection::kSendOnly;
  }
  if (direction == "recvonly") {
    return RtpExtensionDirection::kRecvOnly;
  }
  if (direction == "inactive") {
    return RtpExtensionDirection::kInactive;
  }
  return std::nullopt;
}

} // namespace

AbsoluteSendTimeExtensionIds
ParseRemoteAbsoluteSendTimeExtension(const std::string &sdp_section,
                                     RtpExtensionDirection default_direction) {
  constexpr std::string_view kExtmapPrefix = "a=extmap:";
  std::map<uint8_t, std::string> extensions;
  AbsoluteSendTimeExtensionIds result;
  bool absolute_send_time_seen = false;

  std::istringstream lines(sdp_section);
  std::string line;
  while (std::getline(lines, line)) {
    line = Trim(line);
    if (line.rfind(kExtmapPrefix.data(), 0) != 0) {
      continue;
    }

    const size_t separator = line.find(' ', kExtmapPrefix.size());
    if (separator == std::string::npos) {
      continue;
    }

    std::string_view id_token(line.data() + kExtmapPrefix.size(),
                              separator - kExtmapPrefix.size());
    std::optional<RtpExtensionDirection> direction = default_direction;
    const size_t direction_separator = id_token.find('/');
    if (direction_separator != std::string_view::npos) {
      direction = ParseDirection(id_token.substr(direction_separator + 1));
      id_token = id_token.substr(0, direction_separator);
    }

    std::string uri = Trim(line.substr(separator + 1));
    const size_t uri_end = uri.find_first_of(" \t");
    if (uri_end != std::string::npos) {
      uri.resize(uri_end);
    }

    const std::optional<uint32_t> parsed_id = ParseUint32(id_token);
    if (!parsed_id.has_value() || !rtp::IsValidOneByteExtensionId(*parsed_id)) {
      continue;
    }

    const uint8_t extension_id = static_cast<uint8_t>(*parsed_id);
    if (!extensions.emplace(extension_id, uri).second) {
      return InvalidExtensionIds();
    }
    if (uri != rtp::kAbsoluteSendTimeUri) {
      continue;
    }
    if (absolute_send_time_seen) {
      return InvalidExtensionIds();
    }
    absolute_send_time_seen = true;

    if (!direction.has_value()) {
      return InvalidExtensionIds();
    }
    if (*direction == RtpExtensionDirection::kSendRecv) {
      result.send_id = extension_id;
      result.recv_id = extension_id;
    } else if (*direction == RtpExtensionDirection::kSendOnly) {
      result.recv_id = extension_id;
    } else if (*direction == RtpExtensionDirection::kRecvOnly) {
      result.send_id = extension_id;
    }
  }
  return result;
}

std::string BuildLocalAbsoluteSendTimeExtmap(
    const AbsoluteSendTimeExtensionIds &extension_ids) {
  if (!extension_ids.valid) {
    return {};
  }

  std::optional<uint8_t> extension_id = extension_ids.send_id;
  if (!extension_id.has_value()) {
    extension_id = extension_ids.recv_id;
  }
  if (!extension_id.has_value() ||
      !rtp::IsValidOneByteExtensionId(*extension_id)) {
    return {};
  }
  if (extension_ids.send_id.has_value() && extension_ids.recv_id.has_value() &&
      extension_ids.send_id != extension_ids.recv_id) {
    return {};
  }

  std::string direction;
  if (extension_ids.send_id.has_value() && !extension_ids.recv_id.has_value()) {
    direction = "/sendonly";
  } else if (!extension_ids.send_id.has_value() &&
             extension_ids.recv_id.has_value()) {
    direction = "/recvonly";
  }

  return "a=extmap:" + std::to_string(*extension_id) + direction + " " +
         rtp::kAbsoluteSendTimeUri + "\n";
}

} // namespace minirtc
