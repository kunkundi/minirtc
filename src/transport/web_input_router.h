/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-16
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WEB_INPUT_ROUTER_H_
#define _WEB_INPUT_ROUTER_H_

#include <functional>
#include <mutex>
#include <string>

#include "nlohmann/json.hpp"

namespace minirtc {

// A reliable action advances an epoch. Motion from that epoch cannot overtake
// its button/key/display action even when the two SCTP streams are reordered.
class WebInputRouter {
 public:
  explicit WebInputRouter(std::function<void(const std::string&)> deliver)
      : deliver_(std::move(deliver)) {}

  void Deliver(const std::string& message, bool motion) {
    if (message.size() > 4096) return;
    Deliver(message, motion, nlohmann::json::parse(message, nullptr, false));
  }

  // The control channel already parses its handshake; reuse that JSON value.
  void Deliver(const std::string& message, bool motion, nlohmann::json action) {
    if (!action.is_object()) return;
    std::lock_guard lock(mutex_);
    try {
      if (!action.contains("_input_epoch") || !action.contains("_input_seq")) {
        if (!motion && !upgraded_) deliver_(message);
        return;
      }
      const auto epoch = action.at("_input_epoch").get<uint64_t>();
      const auto seq = action.at("_input_seq").get<uint64_t>();
      if (epoch > 9007199254740991ULL || !seq || seq > 9007199254740991ULL)
        return;
      if (motion &&
          (action.value("type", -1) != 0 || !action.contains("mouse") ||
           action.at("mouse").value("flag", -1) != 0))
        return;
      upgraded_ = true;
      if (motion) {
        if (epoch < epoch_ || seq <= sequence_) return;
        if (epoch > epoch_) {
          if (pending_.empty() ||
              seq > pending_.at("_input_seq").get<uint64_t>())
            pending_ = std::move(action);
          return;
        }
        sequence_ = seq;
        deliver_(message);
        return;
      }
      if (epoch <= epoch_ || seq <= control_sequence_) return;
      epoch_ = epoch;
      sequence_ = control_sequence_ = seq;
      deliver_(message);
      if (!pending_.empty() &&
          pending_.at("_input_epoch").get<uint64_t>() <= epoch_) {
        if (pending_.at("_input_epoch").get<uint64_t>() == epoch_ &&
            pending_.at("_input_seq").get<uint64_t>() > sequence_) {
          sequence_ = pending_.at("_input_seq").get<uint64_t>();
          deliver_(pending_.dump());
        }
        pending_ = nlohmann::json();
      }
    } catch (const nlohmann::json::exception&) {
      // Invalid or mixed-version input must never reach OS input injection.
    }
  }

 private:
  std::mutex mutex_;
  std::function<void(const std::string&)> deliver_;
  bool upgraded_ = false;
  uint64_t epoch_ = 0;
  uint64_t sequence_ = 0;
  uint64_t control_sequence_ = 0;
  nlohmann::json pending_;
};

}  // namespace minirtc

#endif
