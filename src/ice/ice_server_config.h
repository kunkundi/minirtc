/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-08
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ICE_SERVER_CONFIG_H_
#define _ICE_SERVER_CONFIG_H_

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>

#include "ice_utils.h"
#include "minirtc.h"

namespace minirtc {

// libnice reserves three preference bits for TURN endpoints.
inline constexpr size_t kMaxTurnEndpoints = 8;

struct IceServerEndpoint {
  std::string host;
  uint16_t port = 3478;
  bool turn = false;
  bool tcp = false;
  std::string username;
  std::string password;
  int64_t expires_at = 0;
};

inline int64_t IceUnixTime() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline bool ParseIceServerUrl(const std::string& url,
                              IceServerEndpoint& result) {
  static const std::regex uri(
      R"(^(stun|turn):(\[[0-9A-Za-z:.%_-]+\]|[A-Za-z0-9._-]+)(:([0-9]{1,5}))?(\?transport=(udp|tcp))?$)");
  std::smatch match;
  if (url.size() > 512 || !std::regex_match(url, match, uri)) return false;
  IceServerEndpoint endpoint;
  endpoint.turn = match[1] != "stun";
  endpoint.tcp = match[6] == "tcp";
  if (!endpoint.turn && match[5].matched) return false;
  const auto port = match[4].matched ? std::stoi(match[4]) : 3478;
  endpoint.host = match[2];
  if (port < 1 || port > 65535 ||
      endpoint.host.size() + 1 + std::to_string(port).size() > 255)
    return false;
  if (endpoint.host.front() == '[') {
    endpoint.host = endpoint.host.substr(1, endpoint.host.size() - 2);
    if (endpoint.host.find(':') == std::string::npos) return false;
  }
  const auto zone = endpoint.host.find('%');
  std::transform(endpoint.host.begin(),
                 zone == std::string::npos ? endpoint.host.end()
                                           : endpoint.host.begin() + zone,
                 endpoint.host.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  endpoint.port = static_cast<uint16_t>(port);
  result = std::move(endpoint);
  return true;
}

struct IceServerConfiguration {
  std::string id;
  int64_t expires_at = 0;
  std::vector<IceServerEndpoint> servers;

  bool Fresh(int64_t now = IceUnixTime()) const {
    if (expires_at <= now + 5) return false;
    for (const auto& server : servers)
      if (server.turn && server.expires_at <= now + 5) return false;
    return true;
  }
};

inline bool ReadIceExpiry(const nlohmann::json& object, int64_t& expiry) {
  auto it = object.find("expires_at");
  if (it == object.end() || !it->is_number_integer()) return false;
  if (it->is_number_unsigned() && it->get<uint64_t>() > INT64_MAX) return false;
  expiry = it->get<int64_t>();
  return expiry > 0;
}

inline bool ParseIceServerConfiguration(const nlohmann::json& ice,
                                        IceServerConfiguration& result) {
  if (!ice.is_object() || !ice.contains("version") ||
      !ice["version"].is_number_integer() || ice["version"] != 1 ||
      !ice.contains("id") || !ice["id"].is_string() ||
      !ice.contains("servers") || !ice["servers"].is_array() ||
      ice["servers"].size() > 16)
    return false;
  IceServerConfiguration config;
  config.id = ice["id"].get<std::string>();
  if (config.id.empty() || config.id.size() > 64 ||
      config.id.find_first_not_of("0123456789abcdefABCDEF-") !=
          std::string::npos ||
      !ReadIceExpiry(ice, config.expires_at))
    return false;
  std::map<std::string, size_t> seen;
  size_t stun_count = 0, turn_count = 0, url_count = 0;
  for (const auto& server : ice["servers"]) {
    if (!server.is_object() || !server.contains("urls")) return false;
    auto urls = server["urls"];
    if (urls.is_string()) urls = nlohmann::json::array({urls});
    if (!urls.is_array() || urls.empty()) return false;
    for (const auto& url : urls) {
      if (++url_count > 32 || !url.is_string()) return false;
      IceServerEndpoint endpoint;
      if (!ParseIceServerUrl(url.get<std::string>(), endpoint)) return false;
      const auto key = std::string(endpoint.turn ? "turn/" : "stun/") +
                       StunEndpoint{endpoint.host, endpoint.port}.ToString() +
                       (endpoint.tcp ? "/tcp" : "/udp");
      if (endpoint.turn) {
        if (!server.contains("username") || !server["username"].is_string() ||
            !server.contains("credential") ||
            !server["credential"].is_string() ||
            !ReadIceExpiry(server, endpoint.expires_at))
          return false;
        endpoint.username = server["username"].get<std::string>();
        endpoint.password = server["credential"].get<std::string>();
        if (endpoint.username.empty() || endpoint.username.size() > 512 ||
            endpoint.password.empty() || endpoint.password.size() > 512 ||
            endpoint.username.find('\0') != std::string::npos ||
            endpoint.password.find('\0') != std::string::npos ||
            endpoint.expires_at <= IceUnixTime() + 5)
          return false;
      }
      const auto [existing, inserted] =
          seen.emplace(key, config.servers.size());
      if (!inserted) {
        const auto& previous = config.servers[existing->second];
        if (endpoint.username != previous.username ||
            endpoint.password != previous.password ||
            endpoint.expires_at != previous.expires_at)
          return false;
        continue;
      }
      if (endpoint.turn ? ++turn_count > kMaxTurnEndpoints : ++stun_count > 8)
        return false;
      config.servers.push_back(std::move(endpoint));
    }
  }
  if (!config.Fresh()) return false;
  result = std::move(config);
  return true;
}

inline bool IceTurnTransportAllowed(const IceServerEndpoint& server,
                                    MiniRtcTurnMode mode) {
  return server.turn && mode != MiniRtcTurnMode::MiniRtcTurnDisabled &&
         (mode != MiniRtcTurnMode::MiniRtcTurnForceUdp || !server.tcp) &&
         (mode != MiniRtcTurnMode::MiniRtcTurnForceTcp || server.tcp);
}

}  // namespace minirtc

#endif
