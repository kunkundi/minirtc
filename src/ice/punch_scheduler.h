/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _PUNCH_SCHEDULER_H_
#define _PUNCH_SCHEDULER_H_

#include <algorithm>
#include <set>

#include "punch_config.h"
#include "punch_protocol.h"

namespace minirtc {
struct PunchMappingSample {
  uint64_t socket = 0, generation = 0;
  std::string server_ip, mapped_ip;
  uint16_t server_port = 0, mapped_port = 0;
  int64_t received_ms = 0;
  uint32_t sequence = 0;
};
struct PunchMappingSnapshot {
  uint64_t socket = 0, generation = 0;
  std::string local_ip, foundation;
  uint16_t local_port = 0;
  uint32_t component = 1;
  std::vector<PunchMappingSample> samples;
};
enum class PunchMapping { Unknown, StableObserved, DependentObserved };
PunchMapping ClassifyPunchMapping(const PunchMappingSnapshot&,
                                  uint64_t generation, int64_t now_ms,
                                  uint32_t max_age_ms = 15000);
// Prefer the selected ICE base, then another UDP base on the same local IP.
// With no usable observation on that path, prefer a stable mapping. Expired
// samples are removed from the returned copy; source observations stay intact.
std::optional<PunchMappingSnapshot> SelectPunchMapping(
    const std::vector<PunchMappingSnapshot>& snapshots,
    const std::string& selected_local_ip, uint16_t selected_local_port,
    uint64_t generation, int64_t now_ms, uint32_t max_age_ms = 15000);
punch::Json PunchBudgetJson(const PunchConfig&);
std::optional<punch::Json> PunchHello(const PunchMappingSnapshot&,
                                      const PunchConfig&, uint64_t generation,
                                      int64_t now_ms, int64_t remaining_ms,
                                      bool retry = false);
// Inputs are already schema/authentication checked. Agreement is deterministic
// from both HELLOs; each endpoint still enforces its own actual deadline.
std::optional<punch::Json> MakePunchPlan(const punch::Json& offer,
                                         const punch::Json& answer);
using PunchRandom = std::function<bool(uint32_t&)>;
// Uniform sampling without replacement from unexcluded ports in 1024..65535;
// return the randomly chosen subset in ascending order for probe scheduling.
std::vector<uint16_t> SamplePunchPorts(size_t count,
                                       const std::set<uint16_t>& exclude,
                                       const PunchRandom& random);
bool SecurePunchRandom(uint32_t& value);
struct PunchDeadlines {
  int64_t probe = 0, finish = 0;
};
PunchDeadlines MakePunchDeadlines(int64_t now_ms, int64_t original_deadline_ms,
                                  uint32_t duration_ms, uint32_t reserve_ms);
class PunchTokenBucket {
 public:
  PunchTokenBucket(uint32_t rate, uint32_t burst, int64_t now_ms);
  bool Available(int64_t now_ms);
  bool Take(int64_t now_ms);

 private:
  uint32_t rate_, burst_;
  int64_t tokens_, last_;
};
class PunchProbeBudget {
 public:
  static uint32_t ProbeLimit(const PunchConfig& c, uint32_t ack_reserve) {
    return c.probe_packet_budget -
           std::min(ack_reserve, c.probe_packet_budget / 8);
  }
  // Reserve ACK capacity inside each attempt's existing packet limit. The
  // retry mode permits at most two such limits; rate tokens never reset.
  PunchProbeBudget(const PunchConfig& c, int64_t now_ms, bool retry = false,
                   uint32_t ack_reserve = 0)
      : bucket_(c.probe_pps, c.burst_packets, now_ms),
        limit_(c.probe_packet_budget),
        probe_limit_(ProbeLimit(c, ack_reserve)),
        max_rounds_(retry ? 2 : 1) {}
  bool Take(int64_t now_ms, bool ack = false) {
    if (!Ready(now_ms, ack) || !bucket_.Take(now_ms)) return false;
    ++attempts_;
    if (!ack) ++probes_;
    return true;
  }
  bool Ready(int64_t now_ms, bool ack = false) {
    return attempts_ < limit_ && (ack || probes_ < probe_limit_) &&
           bucket_.Available(now_ms);
  }
  bool exhausted() const {
    return attempts_ >= limit_ || probes_ >= probe_limit_;
  }
  bool NextAttempt() {
    if (round_ >= max_rounds_) return false;
    ++round_;
    attempts_ = probes_ = 0;
    return true;
  }
  uint32_t probe_limit() const { return probe_limit_; }

 private:
  PunchTokenBucket bucket_;
  uint32_t attempts_ = 0, probes_ = 0, limit_, probe_limit_;
  uint32_t round_ = 1, max_rounds_;
};
// Peek/Commit permits ACKs to consume the shared budget before scheduled
// probes.
class PunchRoundRobin {
 public:
  PunchRoundRobin(size_t count, uint32_t rounds, int64_t start_ms)
      : count_(count), rounds_(rounds), round_start_(start_ms) {}
  std::optional<size_t> Peek(int64_t now_ms);
  void Commit(int64_t now_ms);
  bool done() const { return !count_ || round_ >= rounds_; }

 private:
  size_t count_, index_ = 0;
  uint32_t rounds_, round_ = 0;
  int64_t round_start_;
};
}  // namespace minirtc

#endif
