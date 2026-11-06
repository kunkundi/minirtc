/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-29
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DIRECT_ADDRESS_H_
#define _DIRECT_ADDRESS_H_

#include <algorithm>
#include <asio/ip/address.hpp>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace minirtc {
// Numeric unicast addresses only. A link-local endpoint needs a local zone;
// ICE candidate text omits it because interface indices are host-specific.
inline std::optional<asio::ip::address> ParseDirectAddress(
    std::string_view text, bool require_scope = true) {
  if (text.empty() || text.size() > 128 ||
      text.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLM"
                             "NOPQRSTUVWXYZ:._-%") != std::string_view::npos)
    return std::nullopt;
  const auto percent = text.find('%');
  const auto host = text.substr(0, percent);
  asio::error_code error;
  auto address = asio::ip::make_address(std::string(host), error);
  if (error) return std::nullopt;
  if (address.is_v4()) {
    const auto first = address.to_v4().to_bytes()[0];
    if (percent != std::string_view::npos || address.to_string() != host ||
        !first || first == 127 || first >= 224)
      return std::nullopt;
    return address;
  }
  auto v6 = address.to_v6();
  const auto bytes = v6.to_bytes();
  if (v6.is_unspecified() || v6.is_loopback() || v6.is_multicast() ||
      v6.is_v4_mapped() ||
      std::all_of(bytes.begin(), bytes.begin() + 12,
                  [](auto b) { return b == 0; }))
    return std::nullopt;
  if (percent != std::string_view::npos) {
    const auto zone = text.substr(percent + 1);
    if (!v6.is_link_local() || zone.empty() ||
        zone.find('%') != std::string_view::npos)
      return std::nullopt;
    uint32_t index = 0;
    if (zone.find_first_not_of("0123456789") == std::string_view::npos) {
      const auto result =
          std::from_chars(zone.data(), zone.data() + zone.size(), index);
      if (result.ec != std::errc{} || result.ptr != zone.data() + zone.size() ||
          !index)
        return std::nullopt;
    } else {
      // Reject partial numeric zones accepted by some platform parsers.
      if (zone.front() >= '0' && zone.front() <= '9') return std::nullopt;
      const auto scoped = asio::ip::make_address_v6(std::string(text), error);
      if (error || !scoped.scope_id() || scoped.scope_id() > UINT32_MAX)
        return std::nullopt;
      index = static_cast<uint32_t>(scoped.scope_id());
    }
    v6.scope_id(index);
  }
  if (require_scope && v6.is_link_local() && !v6.scope_id())
    return std::nullopt;
  return asio::ip::address(v6);
}

inline std::string DirectAddressString(asio::ip::address address) {
  if (address.is_v4()) return address.to_string();
  auto v6 = address.to_v6();
  const auto scope = v6.scope_id();
  v6.scope_id(0);
  return v6.to_string() + (scope ? "%" + std::to_string(scope) : "");
}

inline bool IsDirectAddressPair(std::string_view local,
                                std::string_view remote) {
  const auto a = ParseDirectAddress(local), b = ParseDirectAddress(remote);
  if (!a || !b || a->is_v4() != b->is_v4()) return false;
  if (a->is_v4()) return true;
  return a->to_v6().is_link_local() == b->to_v6().is_link_local() &&
         a->to_v6().scope_id() == b->to_v6().scope_id();
}
}  // namespace minirtc

#endif
