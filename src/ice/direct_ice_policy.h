/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-24
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DIRECT_ICE_POLICY_H_
#define _DIRECT_ICE_POLICY_H_

#include <charconv>
#include <cstdint>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "direct_address.h"

namespace minirtc {
enum class IcePolicy { ManagedServers, HostOnly };
struct DirectIcePolicy {
  IcePolicy mode = IcePolicy::ManagedServers;
  std::string local_address;
  std::string remote_address;
  bool enabled() const { return mode == IcePolicy::HostOnly; }
};

enum class DirectCandidateOrigin { Local, Remote };

inline std::optional<std::string> NormalizeDirectCandidate(
    std::string candidate, const std::string& address,
    DirectCandidateOrigin origin) {
  if (candidate.empty() || candidate.size() > 2048) return std::nullopt;
  if (candidate.size() >= 2 &&
      candidate.compare(candidate.size() - 2, 2, "\r\n") == 0)
    candidate.resize(candidate.size() - 2);
  if (candidate.find_first_of("\r\n") != std::string::npos ||
      candidate.find('\0') != std::string::npos)
    return std::nullopt;
  std::istringstream input(candidate);
  std::string foundation, transport, ip, typ, kind, extra;
  uint64_t component = 0, priority = 0, port = 0;
  if (!(input >> foundation >> component >> transport >> priority >> ip >>
        port >> typ >> kind))
    return std::nullopt;
  if (foundation.rfind("a=", 0) == 0) foundation.erase(0, 2);
  if (foundation.rfind("candidate:", 0) != 0 || foundation.size() <= 10 ||
      foundation.size() > 42 || component != 1 ||
      (transport != "UDP" && transport != "udp") || priority > UINT32_MAX ||
      !port || port > 65535 || typ != "typ" || kind != "host")
    return std::nullopt;
  const auto candidate_address = ParseDirectAddress(ip, false);
  const auto pinned_address = ParseDirectAddress(address);
  if (!candidate_address || !pinned_address ||
      ip.find('%') != std::string::npos ||
      candidate_address->is_v4() != pinned_address->is_v4())
    return std::nullopt;
  auto expected = *pinned_address;
  if (expected.is_v6()) {
    auto v6 = expected.to_v6();
    v6.scope_id(0);
    expected = v6;
  }
  // A VPN can translate the advertised host address. For remote candidates
  // use the authenticated signaling peer's address and the advertised port.
  if (origin == DirectCandidateOrigin::Local && *candidate_address != expected)
    return std::nullopt;
  ip = DirectAddressString(expected);
  // This native-only protocol does not negotiate candidate extensions.
  if (input >> extra) return std::nullopt;
  return "a=" + foundation + " 1 UDP " + std::to_string(priority) + " " + ip +
         " " + std::to_string(port) + " typ host";
}

inline bool PrepareDirectSdp(std::string& sdp, const std::string& address,
                             DirectCandidateOrigin origin,
                             std::string* ufrag = nullptr,
                             std::set<std::string>* all_candidates = nullptr) {
  if (sdp.empty() || sdp.size() > 128 * 1024 ||
      sdp.find('\0') != std::string::npos)
    return false;
  std::istringstream input(sdp);
  std::string line, ice_ufrag, remote_sdp;
  std::set<std::string> candidates =
      all_candidates ? *all_candidates : std::set<std::string>{};
  while (std::getline(input, line)) {
    const bool has_cr = !line.empty() && line.back() == '\r';
    if (has_cr) line.pop_back();
    if (line.rfind("a=candidate:", 0) == 0) {
      auto candidate = NormalizeDirectCandidate(line, address, origin);
      if (!candidate) return false;
      candidates.insert(*candidate);
      if (candidates.size() > 128) return false;
      line = std::move(*candidate);
    } else if (line.rfind("a=ice-ufrag:", 0) == 0) {
      const auto value = line.substr(12);
      if (value.empty() || value.size() > 256 ||
          value.find_first_of(" \t\r\n") != std::string::npos ||
          (!ice_ufrag.empty() && value != ice_ufrag))
        return false;
      ice_ufrag = value;
    }
    if (origin == DirectCandidateOrigin::Remote) {
      remote_sdp += line;
      if (has_cr) remote_sdp += '\r';
      if (!input.eof()) remote_sdp += '\n';
    }
  }
  if (ice_ufrag.empty()) return false;
  if (origin == DirectCandidateOrigin::Remote) sdp = std::move(remote_sdp);
  if (all_candidates) *all_candidates = std::move(candidates);
  if (ufrag) *ufrag = std::move(ice_ufrag);
  return true;
}
}  // namespace minirtc

#endif
