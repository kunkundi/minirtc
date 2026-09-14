#include "punch_runtime.h"

#include <openssl/rand.h>

#include <algorithm>

#include "nice/interfaces.h"
#include "nice/punch.h"

namespace minirtc {
using namespace punch;
namespace {
NiceAddress Address(const std::string& ip, uint16_t port) {
  NiceAddress a;
  nice_address_init(&a);
  if (nice_address_set_from_string(&a, ip.c_str()))
    nice_address_set_port(&a, port);
  return a;
}
std::string Ip(const NiceAddress& a) {
  char ip[NICE_ADDRESS_STRING_LEN] = {};
  nice_address_to_string(&a, ip);
  return ip;
}
std::string Nonce(const Probe& p) {
  return std::string(reinterpret_cast<const char*>(p.nonce.data()),
                     p.nonce.size());
}
}  // namespace
PunchRuntime::PunchRuntime(NiceAgent* a, guint stream, bool offer,
                           uint64_t epoch, const PunchConfig& config,
                           const Keys& keys, Id generation,
                           PunchMappingSnapshot snapshot, int64_t deadline)
    : agent_(a),
      stream_(stream),
      offer_(offer),
      epoch_(epoch),
      local_config_(config),
      agreed_(config),
      keys_(keys),
      generation_(generation),
      snapshot_(std::move(snapshot)),
      original_deadline_(deadline) {
  base_ = Address(snapshot_.local_ip, snapshot_.local_port);
  base_interface_ = nice_interfaces_get_if_index_by_addr(&base_);
  PunchNegotiation::Hooks hooks;
  hooks.send = [this](const Bytes& b) {
    nice_agent_send(agent_, stream_, 1, static_cast<guint>(b.size()),
                    reinterpret_cast<const gchar*>(b.data()));
  };
  hooks.remote_hello = [this](const Json& hello, int64_t now) {
    int64_t age = 0;
    for (const auto& sample : hello["samples"])
      age = std::max(age, sample["age_ms"].get<int64_t>());
    remote_snapshot_deadline_ =
        now + local_config_.mapping_sample_max_age_ms - age;
  };
  hooks.plan = [this](const Json& local, const Json& remote) {
    return offer_ ? MakePunchPlan(local, remote) : MakePunchPlan(remote, local);
  };
  hooks.prepare = [this](const Json& p) { return Prepare(p); };
  hooks.start = [this](const Id& id, const Json&, int64_t t) { Start(id, t); };
  hooks.stop = [this](const std::string& reason) { Stop(reason, false); };
  hooks.random_round = [](Id& r) {
    return RAND_bytes(r.data(), static_cast<int>(r.size())) == 1;
  };
  negotiation_ = std::make_unique<PunchNegotiation>(offer, generation, keys,
                                                    std::move(hooks));
}
PunchRuntime::~PunchRuntime() { Stop("disconnect", false); }
bool PunchRuntime::Begin(int64_t now) {
  auto hello = PunchHello(snapshot_, local_config_, epoch_, now,
                          original_deadline_ - now);
  if (!hello) {
    Stop("ineligible", false);
    return false;
  }
  return negotiation_->Begin(*hello, now, original_deadline_);
}
bool PunchRuntime::BaseAlive() {
  if (!base_interface_ ||
      nice_interfaces_get_if_index_by_addr(&base_) != base_interface_)
    return false;
  bool found = false;
  GSList* candidates = nice_agent_get_local_candidates(agent_, stream_, 1);
  for (auto* i = candidates; i; i = i->next) {
    const auto* c = static_cast<NiceCandidate*>(i->data);
    if (c->type == NICE_CANDIDATE_TYPE_HOST &&
        c->transport == NICE_CANDIDATE_TRANSPORT_UDP &&
        nice_address_equal(&c->addr, &base_) &&
        c->foundation == snapshot_.foundation)
      found = true;
  }
  g_slist_free_full(candidates,
                    reinterpret_cast<GDestroyNotify>(nice_candidate_free));
  return found;
}
bool PunchRuntime::Prepare(const Json& p) {
  const auto now = g_get_monotonic_time() / 1000;
  const bool observation_valid =
      ClassifyPunchMapping(snapshot_, epoch_, now,
                           local_config_.mapping_sample_max_age_ms) !=
      PunchMapping::Unknown;
  if (stopped_ || !endpoints_.empty()) return false;
  if (!BaseAlive()) return false;
  if (!observation_valid || now >= remote_snapshot_deadline_) return false;
  pool_ = (p["anchor"] == "offer") != offer_;
  anchor_ = Address(p["anchor_endpoint"]["ip"], p["anchor_endpoint"]["port"]);
  peer_ip_ = pool_ ? Ip(anchor_) : p["peer_ip"].get<std::string>();
  // HELLO metadata cannot authorize a third party: require the agreed peer IP
  // and anchor endpoint to exist in the current generation's ICE candidates.
  bool known_ip = false, known_anchor = !pool_;
  GSList* remote = nice_agent_get_remote_candidates(agent_, stream_, 1);
  std::set<uint16_t> excluded;
  for (auto* i = remote; i; i = i->next) {
    const auto* c = static_cast<NiceCandidate*>(i->data);
    if (c->type == NICE_CANDIDATE_TYPE_RELAYED ||
        c->transport != NICE_CANDIDATE_TRANSPORT_UDP)
      continue;
    if (Ip(c->addr) == peer_ip_) {
      known_ip = true;
      excluded.insert(nice_address_get_port(&c->addr));
    }
    if (nice_address_equal(&c->addr, &anchor_)) known_anchor = true;
  }
  g_slist_free_full(remote,
                    reinterpret_cast<GDestroyNotify>(nice_candidate_free));
  if (!known_ip || !known_anchor) return false;
  round_ = negotiation_->round();
  const auto& b = p["budget"];
  agreed_.pool_size = b["pool_size"];
  agreed_.target_port_count = b["target_port_count"];
  agreed_.probe_pps = b["probe_pps"];
  agreed_.burst_packets = b["burst_packets"];
  agreed_.pool_probe_rounds = b["pool_probe_rounds"];
  agreed_.round_duration_ms = p["duration_ms"];
  agreed_.ice_finish_reserve_ms = b["ice_finish_reserve_ms"];
  agreed_.probe_packet_budget = b["probe_packet_budget"];
  agreed_.control_packet_budget = b["control_packet_budget"];
  agreed_.promoted_endpoint_limit = b["promoted_endpoint_limit"];
  if (!agreed_.Validate() ||
      !MakePunchDeadlines(now, original_deadline_, agreed_.round_duration_ms,
                          agreed_.ice_finish_reserve_ms)
           .probe)
    return false;
  lease_ = std::make_unique<PunchRoundLease>(PunchProcessBudget::Shared());
  if (!lease_->id()) return false;
  if (!pool_) {
    ports_ = SamplePunchPorts(agreed_.target_port_count, excluded,
                              SecurePunchRandom);
    if (ports_.size() != agreed_.target_port_count) return false;
  }
  const auto count = pool_ ? agreed_.sockets() : 1;
  for (uint32_t i = 0; i < count; ++i) {
    Endpoint e{};
    e.id = nice_agent_punch_open_v1(agent_, stream_, 1, epoch_, &base_, pool_,
                                    &e.bound);
    if (!e.id) return false;  // Stop closes partial allocations before READY.
    endpoints_.push_back(e);
  }
  return true;
}
void PunchRuntime::Start(const Id& round, int64_t now) {
  if (stopped_ || budget_) return;
  const bool observation_valid =
      ClassifyPunchMapping(snapshot_, epoch_, now,
                           local_config_.mapping_sample_max_age_ms) !=
      PunchMapping::Unknown;
  if (!observation_valid || now >= remote_snapshot_deadline_ || !BaseAlive()) {
    Stop("ineligible");
    return;
  }
  round_ = round;
  deadlines_ =
      MakePunchDeadlines(now, original_deadline_, agreed_.round_duration_ms,
                         agreed_.ice_finish_reserve_ms);
  if (!deadlines_.probe) {
    Stop("timeout");
    return;
  }
  budget_ = std::make_unique<PunchProbeBudget>(agreed_, now);
  schedule_ = std::make_unique<PunchRoundRobin>(
      pool_ ? endpoints_.size() : ports_.size(),
      pool_ ? agreed_.pool_probe_rounds : 1, now);
}
void PunchRuntime::Drain() {
  if (draining_ || stopped_) return;
  draining_ = true;
  acks_.clear();
  ack_cache_.clear();
  outstanding_.clear();
  schedule_.reset();
  nice_agent_punch_stop(agent_, stream_, 1, epoch_, FALSE);
  // A quiet discovery round no longer consumes process scheduling capacity.
  if (lease_) lease_->Reset();
}
void PunchRuntime::Stop(const std::string& reason, bool notify) {
  if (stopped_) return;
  stopped_ = true;
  if (negotiation_) negotiation_->Stop(reason, notify);
  nice_agent_punch_stop(agent_, stream_, 1, epoch_, TRUE);
  if (lease_) lease_->Reset();
  acks_.clear();
  ack_cache_.clear();
  outstanding_.clear();
  schedule_.reset();
  keys_.Clear();
}
bool PunchRuntime::DirectSelected() {
  NiceCandidate *local = nullptr, *remote = nullptr;
  if (!nice_agent_get_selected_pair(agent_, stream_, 1, &local, &remote) ||
      !local || !remote || local->type == NICE_CANDIDATE_TYPE_RELAYED ||
      remote->type == NICE_CANDIDATE_TYPE_RELAYED)
    return false;
  Stop("state-change", false);
  return true;
}
bool PunchRuntime::Tick(int64_t now) {
  if (stopped_) return false;
  if (DirectSelected()) return false;
  if (now >= original_deadline_) {
    Stop("timeout");
    return false;
  }
  if (!BaseAlive()) {
    Stop("interface-change");
    return false;
  }
  negotiation_->Tick(now);
  if (stopped_) return false;
  if (!budget_) return true;
  if (now >= deadlines_.finish) {
    Stop("timeout");
    return false;
  }
  if (!draining_ && (now >= deadlines_.probe || budget_->exhausted())) Drain();
  if (draining_) return true;
  // At most one bounded burst per event-loop dispatch, ACKs before new probes.
  for (uint32_t i = 0; i < agreed_.burst_packets; ++i) {
    if (!acks_.empty()) {
      auto& ack = acks_.front();
      if (!Emit(ack.handle, ack.to, ack.bytes, now, true)) break;
      if (pool_) Promote(ack.handle);
      acks_.pop_front();
      continue;
    }
    const auto next = schedule_->Peek(now);
    if (!next) break;
    auto& endpoint = endpoints_[pool_ ? *next : 0];
    if (endpoint.promoted) {
      schedule_->Commit(now);
      continue;
    }
    auto to = pool_ ? anchor_ : Address(peer_ip_, ports_[*next]);
    Probe probe;
    probe.socket = endpoint.id;
    if (RAND_bytes(probe.nonce.data(), static_cast<int>(probe.nonce.size())) !=
            1 ||
        !sequence_) {
      Stop("resources");
      break;
    }
    auto bytes = Encode(
        {Kind::Probe, generation_, round_, sequence_, ProbePayload(probe)},
        offer_ ? keys_.offer_probe : keys_.answer_probe);
    if (!Emit(endpoint.id, to, bytes, now, false)) break;
    ++sequence_;
    outstanding_.emplace(Nonce(probe), Outstanding{endpoint.id, to});
    schedule_->Commit(now);
  }
  return !stopped_;
}
bool PunchRuntime::Emit(uint64_t handle, const NiceAddress& to,
                        const Bytes& bytes, int64_t now, bool ack) {
  if (stopped_ || draining_ || !lease_ || !lease_->id() || !budget_ ||
      bytes.empty())
    return false;
  // Check local availability before process arbitration; deferred scheduling
  // is not a send attempt. Both token debits happen on this owning context.
  if (!budget_->Ready(now)) return false;
  if (!PunchProcessBudget::Shared().Take(lease_->id(), ack, now)) return false;
  if (!budget_->Take(now)) return false;
  nice_agent_punch_send(agent_, stream_, 1, epoch_, handle, &to, bytes.data(),
                        bytes.size());
  return true;  // Attempt committed even if the kernel rejects the send.
}
bool PunchRuntime::Promote(uint64_t id) {
  if (!pool_ || stopped_ || draining_) return false;
  auto it = std::find_if(endpoints_.begin(), endpoints_.end(),
                         [&](const Endpoint& e) { return e.id == id; });
  if (it == endpoints_.end() || it->promoted ||
      promoted_ >= agreed_.promoted_endpoint_limit)
    return false;
  if (!nice_agent_punch_promote(agent_, stream_, 1, epoch_, id, &anchor_))
    return false;
  it->promoted = true;
  ++promoted_;
  return true;
}
void PunchRuntime::Control(const uint8_t* data, size_t size, int64_t now) {
  if (!stopped_) negotiation_->Receive(data, size, now);
}
void PunchRuntime::Direct(uint64_t epoch, uint64_t handle,
                          const NiceCandidate* packet, const uint8_t* bytes,
                          size_t size, int64_t now) {
  if (stopped_ || draining_ || !budget_ || now >= deadlines_.probe ||
      epoch != epoch_ || !packet)
    return;
  auto it = std::find_if(endpoints_.begin(), endpoints_.end(),
                         [&](const Endpoint& e) { return e.id == handle; });
  if (it == endpoints_.end() ||
      !nice_address_equal(&it->bound, &packet->base_addr) ||
      Ip(packet->addr) != peer_ip_ ||
      (pool_ && !nice_address_equal(&packet->addr, &anchor_)))
    return;
  auto p = Decode(bytes, size, offer_ ? keys_.answer_probe : keys_.offer_probe,
                  Domain::Probe, generation_);
  if (!p || p->round != round_) return;
  const auto replay =
      probe_replay_.Accept(p->sequence, Bytes(bytes, bytes + size));
  if (replay == ReplayWindow::Result::TooOld ||
      replay == ReplayWindow::Result::Conflict)
    return;
  const auto probe = *ParseProbe(p->payload);
  if (p->kind == Kind::Ack) {
    auto found = outstanding_.find(Nonce(probe));
    if (found == outstanding_.end() || found->second.handle != handle ||
        probe.socket != handle ||
        !nice_address_equal(&found->second.destination, &packet->addr))
      return;
    outstanding_.erase(found);
    Promote(handle);
  } else if (acks_.size() < 8 && sequence_) {
    const auto cached = ack_cache_.find(p->sequence);
    if (cached != ack_cache_.end()) {
      acks_.push_back(cached->second);
      return;
    }
    auto ack = Encode({Kind::Ack, generation_, round_, sequence_++, p->payload},
                      offer_ ? keys_.offer_probe : keys_.answer_probe);
    Ack response{handle, packet->addr, std::move(ack)};
    if (ack_cache_.size() == 256) ack_cache_.erase(ack_cache_.begin());
    ack_cache_.emplace(p->sequence, response);
    acks_.push_back(std::move(response));
  }
}
}  // namespace minirtc
