/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-10
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RECONNECT_LOGIN_H_
#define _RECONNECT_LOGIN_H_

#include <algorithm>
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

namespace minirtc {

// Owned by one PeerConnection and used only on its signaling thread. The
// endpoint is fixed for that instance; no bearer secrets are written to disk.
class ReconnectLogin {
 public:
  nlohmann::json Request(const std::string& identity, int64_t now) {
    if (identity != identity_ || now >= expires_at_) Clear();
    pending_ = !token_.empty();
    nlohmann::json request = {{"type", "login"}, {"user_id", identity},
                              {"reconnect_version", 1}};
    if (pending_) {
      request["user_id"] = identity.substr(0, identity.find('@'));
      request["reconnect_token"] = token_;
    }
    return request;
  }

  void Accepted(const nlohmann::json& reply, const std::string& identity,
                int64_t now) {
    const bool was_reconnect = pending_;
    pending_ = false;
    const auto version = reply.find("reconnect_version");
    const auto expiry = reply.find("reconnect_expires_at");
    if (version == reply.end() || !version->is_number_integer() || *version != 1 ||
        expiry == reply.end() || !expiry->is_number_integer() ||
        expiry->get<int64_t>() <= now) {
      Clear();
      return;
    }
    const auto token = reply.find("reconnect_token");
    if (token != reply.end() && token->is_string() && ValidToken(token->get_ref<const std::string&>())) {
      identity_ = identity;
      token_ = token->get<std::string>();
      expires_at_ = expiry->get<int64_t>();
    } else if (!(was_reconnect && identity == identity_ &&
                 reply.contains("reconnected") && reply["reconnected"] == true)) {
      Clear();
    } else {
      // A reconnect never extends the absolute lifetime of its credential.
      expires_at_ = std::min(expires_at_, expiry->get<int64_t>());
    }
  }

  // Only a rejected token attempt can cause one password fallback. A failed
  // password login follows the application's existing error/retry behavior.
  bool RetryPassword() {
    const bool retry = pending_;
    Clear();
    return retry;
  }

  void Clear() {
    token_.clear();
    identity_.clear();
    expires_at_ = 0;
    pending_ = false;
  }

 private:
  static bool ValidToken(const std::string& token) {
    return token.size() == 64 && std::all_of(token.begin(), token.end(), [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
  }
  std::string identity_, token_;
  int64_t expires_at_ = 0;
  bool pending_ = false;
};

}  // namespace minirtc

#endif