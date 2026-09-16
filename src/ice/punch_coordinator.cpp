#include "punch_coordinator.h"

#include <openssl/crypto.h>

#include <utility>

namespace minirtc {
using namespace punch;
PunchNegotiation::PunchNegotiation(bool offer, Id generation, const Keys& keys,
                                   Hooks hooks)
    : offer_(offer),
      generation_(generation),
      tx_(offer ? keys.offer_control : keys.answer_control),
      rx_(offer ? keys.answer_control : keys.offer_control),
      hooks_(std::move(hooks)) {}
PunchNegotiation::~PunchNegotiation() {
  OPENSSL_cleanse(tx_.data(), tx_.size());
  OPENSSL_cleanse(rx_.data(), rx_.size());
}
bool PunchNegotiation::Emit(const Bytes& bytes) {
  if (bytes.empty() || state_ == State::Stopped) return false;
  if (attempts_ >= limit_) {
    Stop("budget", false);
    return false;
  }
  ++attempts_;  // Count failed sends too; network success is not a budget
                // refund.
  if (hooks_.send) hooks_.send(bytes);
  return true;
}
Bytes PunchNegotiation::Make(Kind kind, const Json& value) {
  if (!next_sequence_ || state_ == State::Stopped) return {};
  return Encode({kind, generation_, kind == Kind::Hello ? Id{} : round_,
                 next_sequence_++, ControlPayload(kind, value)},
                tx_);
}
void PunchNegotiation::Pending(Kind kind, const Bytes& bytes, int64_t now) {
  pending_ = bytes;
  pending_kind_ = kind;
  retries_ = 0;
  retry_at_ = now + 200;
  Emit(bytes);
}
bool PunchNegotiation::Begin(const Json& hello, int64_t now, int64_t deadline) {
  if (state_ != State::Idle || deadline <= now || deadline - now > 30000 ||
      !hooks_.plan || !hooks_.prepare || !hooks_.random_round ||
      ControlPayload(Kind::Hello, hello).empty())
    return false;
  state_ = State::Hello;
  deadline_ = deadline;
  local_hello_ = hello;
  limit_ = hello["budget"]["control_packet_budget"].get<uint32_t>();
  hello_ = Make(Kind::Hello, hello);
  Pending(Kind::Hello, hello_, now);
  return state_ != State::Stopped;
}
void PunchNegotiation::Stop(const std::string& reason, bool notify) {
  if (state_ == State::Stopped) return;
  if (notify && !IsZero(round_) && attempts_ < limit_)
    Emit(Make(Kind::Cancel, {{"reason", reason}}));
  state_ = State::Stopped;
  pending_.clear();
  OPENSSL_cleanse(tx_.data(), tx_.size());
  OPENSSL_cleanse(rx_.data(), rx_.size());
  if (hooks_.stop) hooks_.stop(reason);
}
void PunchNegotiation::Tick(int64_t now) {
  if (state_ == State::Idle || state_ == State::Stopped) return;
  if (now >= deadline_) {
    Stop("timeout");
    return;
  }
  if (state_ == State::Allocating) {
    const auto result = hooks_.poll_prepare();
    if (state_ == State::Stopped) return;
    if (result == Preparation::Failed)
      Stop("resources");
    else if (result == Preparation::Ready)
      FinishPreparation(now);
    return;
  }
  if (pending_.empty() || now < retry_at_) return;
  if (retries_ == 3) {
    // Exhausting HELLO sends is not the ICE upgrade deadline. A peer may
    // still be finishing normal checks before it can begin negotiation.
    // Keep receiving authenticated messages, with the same snapshot, replay
    // window and original deadline; never allocate or send more on this timer.
    if (pending_kind_ == Kind::Hello) {
      pending_.clear();
    } else
      Stop("timeout");
    return;
  }
  // Retransmit the original HELLO with PREPARE: otherwise a lost first HELLO
  // can strand the responder without the snapshot whose digest PREPARE names.
  if (pending_kind_ == Kind::Prepare && !Emit(hello_)) return;
  if (!Emit(pending_)) return;
  constexpr int waits[] = {400, 800, 800};
  retry_at_ = now + waits[retries_++];
}
void PunchNegotiation::Start(int64_t now) {
  if (state_ == State::Probing || state_ == State::Stopped) return;
  state_ = State::Probing;
  if (hooks_.start) hooks_.start(round_, plan_, now);
}
void PunchNegotiation::BeginPreparation(int64_t now) {
  state_ = State::Allocating;
  pending_.clear();
  if (!hooks_.prepare(plan_)) {
    Stop("resources");
    return;
  }
  if (!hooks_.poll_prepare) FinishPreparation(now);
}
void PunchNegotiation::FinishPreparation(int64_t now) {
  if (state_ != State::Allocating) return;
  digest_ = Digest(ControlPayload(Kind::Prepare, plan_));
  if (offer_) {
    prepare_ = Make(Kind::Prepare, plan_);
    state_ = State::Preparing;
    Emit(hello_);
    Pending(Kind::Prepare, prepare_, now);
  } else {
    ready_ =
        Make(Kind::Ready, {{"digest", digest_}, {"budget", plan_["budget"]}});
    state_ = State::Ready;
    Pending(Kind::Ready, ready_, now);
  }
}
void PunchNegotiation::Receive(const uint8_t* data, size_t size, int64_t now) {
  if (state_ == State::Idle || state_ == State::Stopped) return;
  if (now >= deadline_) {
    Stop("timeout");
    return;
  }
  auto packet = Decode(data, size, rx_, Domain::Control, generation_);
  if (!packet) return;
  const auto kind = packet->kind;
  // Only the original offer may propose the one accepted round. Never let an
  // old/unrelated round consume sequence space or cancel this one.
  if (kind != Kind::Hello && packet->round != round_ &&
      !(kind == Kind::Prepare && !offer_ && IsZero(round_) &&
        state_ == State::Hello))
    return;
  const auto replay =
      replay_.Accept(packet->sequence, Bytes(data, data + size));
  if (replay == ReplayWindow::Result::TooOld) return;
  if (replay == ReplayWindow::Result::Conflict) {
    Stop("conflict");
    return;
  }
  const auto value = *ParseControl(kind, packet->payload);
  const bool duplicate = replay == ReplayWindow::Result::Duplicate;
  if (kind == Kind::Hello) {
    if (!remote_hello_.is_null()) {
      if (remote_hello_ != value || remote_hello_payload_ != packet->payload) {
        Stop("conflict");
        return;
      }
      // A duplicate HELLO can solicit the cached PREPARE; the responder never
      // echoes duplicate HELLOs, avoiding a response loop.
      if (offer_ && !prepare_.empty() && state_ == State::Preparing) {
        Emit(hello_);
        Emit(prepare_);
      }
      return;
    }
    remote_hello_ = value;
    remote_hello_payload_ = packet->payload;
    if (hooks_.remote_hello) hooks_.remote_hello(value, now);
    if (!offer_) {
      Emit(hello_);
      return;
    }
    auto proposal = hooks_.plan(local_hello_, remote_hello_);
    if (proposal) {
      (*proposal)["offer_hello"] =
          Digest(ControlPayload(Kind::Hello, local_hello_));
      (*proposal)["answer_hello"] = Digest(remote_hello_payload_);
    }
    if (!proposal || ControlPayload(Kind::Prepare, *proposal).empty()) {
      Stop("ineligible", false);
      return;
    }
    plan_ = *proposal;
    if (!hooks_.random_round(round_) || IsZero(round_)) {
      Stop("resources", false);
      return;
    }
    BeginPreparation(now);
    return;
  }
  if (kind == Kind::Prepare && !offer_) {
    if (!plan_.is_null()) {
      if (plan_ != value) {
        Stop("conflict");
        return;
      }
      if (!ready_.empty()) Emit(ready_);
      return;
    }
    if (state_ != State::Hello || remote_hello_.is_null()) return;
    auto expected = hooks_.plan(local_hello_, remote_hello_);
    if (expected) {
      (*expected)["offer_hello"] = Digest(remote_hello_payload_);
      (*expected)["answer_hello"] =
          Digest(ControlPayload(Kind::Hello, local_hello_));
    }
    if (!expected || *expected != value)
      return;  // No peer-authorized arbitrary IP/budget.
    round_ = packet->round;
    plan_ = value;
    BeginPreparation(now);
    return;
  }
  if (kind == Kind::Ready && offer_) {
    if (value["digest"] != digest_ || value["budget"] != plan_["budget"])
      return;
    if (state_ == State::Preparing) {
      start_ = Make(Kind::Start, {{"digest", digest_}});
      Start(now);
      Pending(Kind::Start, start_, now);
    } else if (state_ == State::Probing && !start_.empty())
      Emit(start_);
    return;
  }
  if (kind == Kind::Start && !offer_) {
    if (value["digest"] != digest_) return;
    if (state_ == State::Ready) {
      pending_.clear();
      started_ = Make(Kind::Started, {{"digest", digest_}});
      Start(now);
      Emit(started_);
    } else if (state_ == State::Probing)
      Emit(started_);
    return;
  }
  if (kind == Kind::Started && offer_ && state_ == State::Probing &&
      value["digest"] == digest_) {
    pending_.clear();
    return;
  }
  if (kind == Kind::Cancel && !duplicate) {
    Stop(value["reason"].get<std::string>(), false);
    return;
  }
}
}  // namespace minirtc
