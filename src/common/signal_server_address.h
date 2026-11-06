/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-09
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SIGNAL_SERVER_ADDRESS_H_
#define _SIGNAL_SERVER_ADDRESS_H_

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace minirtc {

struct SignalServerAddress {
  std::string host;
  std::string target;
  std::optional<int> port;

  // Keep the separately configured port out of the persisted address.
  std::string Address() const { return host + target; }
  std::string Url(int fallback_port) const {
    return "wss://" + host + ":" + std::to_string(port.value_or(fallback_port)) +
           target;
  }
};

// Accept a host, host/path, or secure URL. Never downgrade signaling TLS.
inline std::optional<SignalServerAddress> ParseSignalServerAddress(
    std::string_view input) {
  const auto first = input.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return std::nullopt;
  input = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
  if (std::any_of(input.begin(), input.end(), [](unsigned char c) {
        return c <= 32 || c == 127;
      }) || input.find_first_of("\\#") != std::string_view::npos) {
    return std::nullopt;
  }
  const auto scheme_end = input.find("://");
  if (scheme_end != std::string_view::npos &&
      scheme_end < input.find_first_of("/?")) {
    std::string scheme(input.substr(0, scheme_end));
    for (char& c : scheme) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (scheme != "wss" && scheme != "https") return std::nullopt;
    input.remove_prefix(scheme_end + 3);
  }

  const auto target_start = input.find_first_of("/?");
  auto authority = input.substr(0, target_start);
  if (authority.empty() || authority.find('@') != std::string_view::npos)
    return std::nullopt;
  SignalServerAddress result;
  if (target_start != std::string_view::npos) {
    result.target = std::string(input.substr(target_start));
    if (result.target.front() == '?') result.target.insert(0, "/");
    if (result.target == "/") result.target.clear();
  }

  std::string_view port_text;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close == 1) return std::nullopt;
    const auto ipv6 = authority.substr(1, close - 1);
    if (ipv6.find(':') == std::string_view::npos ||
        ipv6.find_first_not_of("0123456789abcdefABCDEF:.") !=
            std::string_view::npos) return std::nullopt;
    result.host = std::string(authority.substr(0, close + 1));
    authority.remove_prefix(close + 1);
    if (!authority.empty()) {
      if (authority.front() != ':') return std::nullopt;
      port_text = authority.substr(1);
      if (port_text.empty()) return std::nullopt;
    }
  } else {
    const auto colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    if (host.empty() || host.size() > 253 ||
        host.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") !=
            std::string_view::npos) return std::nullopt;
    result.host = std::string(host);
    if (colon != std::string_view::npos) {
      port_text = authority.substr(colon + 1);
      if (port_text.empty()) return std::nullopt;
    }
  }
  if (!port_text.empty()) {
    int port = 0;
    const auto parsed = std::from_chars(port_text.data(),
                                       port_text.data() + port_text.size(), port);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != port_text.data() + port_text.size() ||
        port < 1 || port > 65535) return std::nullopt;
    result.port = port;
  }
  // Params::signal_server_ip is a 256-byte, null-terminated buffer.
  if (result.Address().size() >= 256) return std::nullopt;
  return result;
}

}  // namespace minirtc

#endif