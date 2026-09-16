/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _PUNCH_RUNTIME_H_
#define _PUNCH_RUNTIME_H_

#include <deque>
#include <memory>

#include "nice/agent.h"
#include "punch_budget.h"
#include "punch_coordinator.h"

namespace minirtc {
// Owned by IceAgent; constructed/used/stopped only on its Nice main context.
// Holds opaque backend handles, never owns or reads a socket independently.
class PunchRuntime {
 public:
  PunchRuntime(NiceAgent*, guint stream, bool offer, uint64_t epoch,
               const PunchConfig&, const punch::Keys&, punch::Id,
               PunchMappingSnapshot snapshot, int64_t deadline_ms,
               bool retry_enabled = false);
  ~PunchRuntime();
  bool Begin(int64_t now_ms);
  bool Tick(int64_t now_ms);
  void Control(const uint8_t*, size_t, int64_t now_ms);
  void Direct(uint64_t epoch, uint64_t handle, const NiceCandidate*,
              const uint8_t*, size_t, int64_t now_ms);
  void Stop(const std::string& reason, bool notify = true);

 private:
  struct Endpoint {
    uint64_t id;
    NiceAddress bound;
    bool promoted = false;
  };
  struct Outstanding {
    uint64_t handle;
    NiceAddress destination;
  };
  struct Ack {
    uint64_t handle;
    NiceAddress to;
    punch::Bytes bytes;
  };
  bool Prepare(const punch::Json&);
  PunchNegotiation::Preparation PrepareBatch();
  void Start(const punch::Id&, int64_t now_ms);
  void Drain();
  bool StartRetry(int64_t now_ms);
  bool Emit(uint64_t, const NiceAddress&, const punch::Bytes&, int64_t,
            bool ack);
  bool Promote(uint64_t);
  bool DirectSelected();
  guint QueryBaseInterface();
  bool BaseAlive(bool refresh_interface = false);
  uint32_t prepare_socket_count_ = 0;
  int64_t last_interface_query_us_ = 0;
  bool base_interface_alive_ = false;
  NiceAgent* agent_;
  guint stream_;
  bool offer_, pool_ = false, stopped_ = false, draining_ = false;
  const bool retry_enabled_;
  uint32_t attempt_ = 0;
  uint64_t epoch_;
  const PunchConfig local_config_;
  PunchConfig agreed_;
  punch::Keys keys_;
  punch::Id generation_, round_{};
  PunchMappingSnapshot snapshot_;
  int64_t original_deadline_;
  int64_t remote_snapshot_deadline_ = 0;
  guint base_interface_ = 0;
  PunchDeadlines deadlines_;
  NiceAddress base_{}, anchor_{};
  std::string peer_ip_;
  std::vector<Endpoint> endpoints_;
  std::vector<uint16_t> ports_;
  // Includes signalled peer ports and every sampled port across both attempts.
  std::set<uint16_t> excluded_ports_;
  std::map<std::string, Outstanding> outstanding_;
  std::deque<Ack> acks_;
  std::map<uint32_t, Ack> ack_cache_;
  punch::ReplayWindow probe_replay_;
  uint32_t sequence_ = 1, promoted_ = 0;
  std::unique_ptr<PunchRoundLease> lease_;
  std::unique_ptr<PunchProbeBudget> budget_;
  std::unique_ptr<PunchRoundRobin> schedule_;
  std::unique_ptr<PunchNegotiation> negotiation_;
};
}  // namespace minirtc

#endif
