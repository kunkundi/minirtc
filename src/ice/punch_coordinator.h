/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _PUNCH_COORDINATOR_H_
#define _PUNCH_COORDINATOR_H_

#include <functional>

#include "punch_protocol.h"

namespace minirtc {
// Pure negotiation state machine. All timestamps are local monotonic
// milliseconds; callbacks run synchronously on the owning ICE context.
class PunchNegotiation {
 public:
  enum class State { Idle, Hello, Preparing, Ready, Probing, Stopped };
  struct Hooks {
    std::function<void(const punch::Bytes&)> send;
    // Called once after authenticating the first remote HELLO, before prepare.
    std::function<void(const punch::Json&, int64_t)> remote_hello;
    std::function<std::optional<punch::Json>(const punch::Json&,
                                             const punch::Json&)>
        plan;
    std::function<bool(const punch::Json&)> prepare;
    std::function<void(const punch::Id&, const punch::Json&, int64_t)> start;
    std::function<void(const std::string&)> stop;
    std::function<bool(punch::Id&)> random_round;
  };
  PunchNegotiation(bool offer, punch::Id generation, const punch::Keys& keys,
                   Hooks hooks);
  ~PunchNegotiation();
  PunchNegotiation(const PunchNegotiation&) = delete;
  PunchNegotiation& operator=(const PunchNegotiation&) = delete;
  bool Begin(const punch::Json& hello, int64_t now_ms, int64_t deadline_ms);
  void Receive(const uint8_t* data, size_t size, int64_t now_ms);
  void Tick(int64_t now_ms);
  void Stop(const std::string& reason, bool notify = true);
  const punch::Id& round() const { return round_; }

 private:
  bool Emit(const punch::Bytes& bytes);
  punch::Bytes Make(punch::Kind kind, const punch::Json& value);
  void Pending(punch::Kind kind, const punch::Bytes& bytes, int64_t now_ms);
  void Start(int64_t now_ms);
  bool offer_;
  punch::Id generation_, round_{};
  punch::Key tx_{}, rx_{};
  Hooks hooks_;
  State state_ = State::Idle;
  punch::ReplayWindow replay_;
  punch::Json local_hello_, remote_hello_, plan_;
  punch::Bytes hello_, prepare_, ready_, start_, started_, pending_;
  punch::Bytes remote_hello_payload_;
  punch::Kind pending_kind_ = punch::Kind::Hello;
  std::string digest_;
  uint32_t next_sequence_ = 1, attempts_ = 0, limit_ = 48, retries_ = 0;
  int64_t deadline_ = 0, retry_at_ = 0;
};
}  // namespace minirtc

#endif
