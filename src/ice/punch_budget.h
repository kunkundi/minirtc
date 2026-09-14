/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _PUNCH_BUDGET_H_
#define _PUNCH_BUDGET_H_

#include <deque>
#include <mutex>
#include <set>

#include "punch_scheduler.h"

namespace minirtc {
// Process-wide discovery rounds and PROBE/ACK rate. Actual extra socket quota
// remains owned by libnice SocketSource, including after a selected promotion.
class PunchProcessBudget {
 public:
  explicit PunchProcessBudget(int64_t now_ms) : bucket_(200, 8, now_ms) {}
  uint64_t Acquire();
  void Release(uint64_t round);
  bool Take(uint64_t round, bool ack, int64_t now_ms);
  static PunchProcessBudget& Shared();

 private:
  struct Waiter {
    uint64_t id;
    bool ack;
  };
  std::mutex mutex_;
  uint64_t next_ = 0;
  std::set<uint64_t> active_;
  std::deque<Waiter> waiting_;
  PunchTokenBucket bucket_;
};
class PunchRoundLease {
 public:
  explicit PunchRoundLease(PunchProcessBudget& budget)
      : budget_(&budget), id_(budget.Acquire()) {}
  ~PunchRoundLease() { Reset(); }
  PunchRoundLease(const PunchRoundLease&) = delete;
  PunchRoundLease& operator=(const PunchRoundLease&) = delete;
  uint64_t id() const { return id_; }
  void Reset() {
    if (id_) {
      budget_->Release(id_);
      id_ = 0;
    }
  }

 private:
  PunchProcessBudget* budget_;
  uint64_t id_;
};
}  // namespace minirtc

#endif
