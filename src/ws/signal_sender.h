/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-24
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SIGNAL_SENDER_H_
#define _SIGNAL_SENDER_H_

#include <functional>
#include <string>
#include <utility>

namespace minirtc {
class SignalSender {
 public:
  virtual ~SignalSender() = default;
  virtual int Send(const std::string& message) = 0;
};

class CallbackSignalSender final : public SignalSender {
 public:
  explicit CallbackSignalSender(std::function<int(const std::string&)> send)
      : send_(std::move(send)) {}
  int Send(const std::string& message) override { return send_(message); }

 private:
  std::function<int(const std::string&)> send_;
};
}  // namespace minirtc

#endif
