/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-07
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ICE_UTILS_H_
#define _ICE_UTILS_H_

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace minirtc {

inline constexpr char kRelayUpgradeAttribute[] = "a=x-minirtc-relay-upgrade:1";
inline constexpr char kP2pEnhancementAttribute[] =
    "a=x-minirtc-p2p-enhancement:1";
// Advertise rfc5764 support; legacy or an absent attribute uses the old layout.
inline constexpr char kSrtpKeyLayoutAttribute[] =
    "a=x-minirtc-srtp-key-layout:rfc5764";
inline constexpr char kUdpPunchAttribute[] = "a=x-minirtc-udp-punch:1";
// A separate fingerprint advertises DTLS authentication without requesting
// SRTP from peers that use the standard fingerprint as their media switch.
inline constexpr char kUdpPunchFingerprintAttribute[] =
    "a=x-minirtc-udp-punch-fingerprint:";

// The entire SDP is the authority. Duplicates and unknown versions disable
// this extension, including a conflicting declaration in a discarded section.
inline bool SupportsUdpPunch(const std::string& sdp) {
  std::istringstream lines(sdp);
  std::string line;
  unsigned count = 0;
  bool supported = false;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("a=x-minirtc-udp-punch:", 0) == 0) {
      ++count;
      supported = line == kUdpPunchAttribute;
    }
  }
  return count == 1 && supported;
}

inline std::string PreserveUdpPunchCapability(const std::string& full,
                                              const std::string& extracted) {
  if (full.find("a=x-minirtc-udp-punch:") == std::string::npos &&
      extracted.find("a=x-minirtc-udp-punch:") == std::string::npos)
    return extracted;
  std::istringstream lines(extracted);
  std::string line, result;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("a=x-minirtc-udp-punch:", 0) != 0) result += line + "\n";
  }
  if (SupportsUdpPunch(full)) result += std::string(kUdpPunchAttribute) + "\n";
  return result;
}

inline bool HasIceAttribute(const std::string& sdp,
                            const std::string& attribute) {
  std::istringstream lines(sdp);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == attribute) return true;
  }
  return false;
}

inline bool SupportsRelayUpgrade(const std::string& sdp) {
  return HasIceAttribute(sdp, kRelayUpgradeAttribute);
}

inline bool SupportsP2pEnhancement(const std::string& sdp) {
  return HasIceAttribute(sdp, kP2pEnhancementAttribute);
}

inline std::string TrimIceWhitespace(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return {};
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

inline std::string GetIceUsername(const std::string& sdp) {
  const std::string prefix = "a=ice-ufrag:";
  std::istringstream lines(sdp);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.compare(0, prefix.size(), prefix) == 0) {
      return TrimIceWhitespace(line.substr(prefix.size()));
    }
  }
  return {};
}

struct IceFingerprintSdp {
  std::string sdp, fingerprint, punch_fingerprint;
};
inline std::optional<IceFingerprintSdp> SplitIceFingerprint(
    const std::string& sdp) {
  IceFingerprintSdp result;
  std::istringstream lines(sdp);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const bool punch = line.rfind(kUdpPunchFingerprintAttribute, 0) == 0;
    if (!punch && line.rfind("a=fingerprint:", 0) != 0) {
      result.sdp += line + "\n";
      continue;
    }
    const std::string prefix =
        std::string(punch ? kUdpPunchFingerprintAttribute : "a=fingerprint:") +
        "sha-256 ";
    if (line.rfind(prefix, 0) != 0) return {};
    std::string value = line.substr(prefix.size());
    if (value.size() != 95) return {};
    for (size_t i = 0; i < value.size(); ++i) {
      if (i % 3 == 2) {
        if (value[i] != ':') return {};
      } else {
        const auto c = static_cast<unsigned char>(value[i]);
        if (!std::isxdigit(c)) return {};
        value[i] = static_cast<char>(std::toupper(c));
      }
    }
    auto& fingerprint = punch ? result.punch_fingerprint : result.fingerprint;
    if (!fingerprint.empty() && fingerprint != value) return {};
    fingerprint = std::move(value);
  }
  if (!result.fingerprint.empty() && !result.punch_fingerprint.empty() &&
      result.fingerprint != result.punch_fingerprint)
    return {};
  return result;
}

struct StunEndpoint {
  std::string host;
  uint16_t port = 0;
  std::string ToString() const {
    return (host.find(':') == std::string::npos ? host : "[" + host + "]") +
           ":" + std::to_string(port);
  }
};

struct IceCandidateSignal {
  std::string sdp;
  std::string ufrag;
  bool complete() const { return sdp.empty(); }
};

// Accept one candidate or one completion marker, never arbitrary SDP or a
// substring that merely happens to contain "end-of-candidates".
inline std::optional<IceCandidateSignal> ParseIceCandidateSignal(
    const std::string& candidate_sdp, const std::string& ufrag = {}) {
  if (candidate_sdp.size() > 4096 || ufrag.size() > 256 ||
      candidate_sdp.find('\0') != std::string::npos ||
      ufrag.find('\0') != std::string::npos ||
      ufrag.find_first_of(" \t\r\n") != std::string::npos)
    return std::nullopt;
  std::string sdp = TrimIceWhitespace(candidate_sdp);
  if (sdp.empty() || sdp == "a=end-of-candidates" || sdp == "end-of-candidates")
    return IceCandidateSignal{{}, ufrag};
  if (sdp.find_first_of("\r\n") != std::string::npos) return std::nullopt;
  if (sdp.compare(0, 2, "a=") == 0) sdp.erase(0, 2);
  if (sdp.compare(0, 10, "candidate:") != 0) return std::nullopt;

  IceCandidateSignal signal{"a=" + sdp, ufrag};
  std::istringstream fields(sdp);
  std::string token;
  // candidate foundation, component, transport, priority, address, port,
  // "typ", and candidate type precede all optional extension pairs.
  for (int i = 0; i < 8; ++i) {
    if (!(fields >> token)) return std::nullopt;
  }
  std::string value;
  while (fields >> token) {
    if (!(fields >> value)) return std::nullopt;
    if (token == "ufrag") {
      if (value.size() > 256 ||
          (!signal.ufrag.empty() && signal.ufrag != value))
        return std::nullopt;
      signal.ufrag = value;
    }
  }
  return signal;
}

}  // namespace minirtc

#endif
