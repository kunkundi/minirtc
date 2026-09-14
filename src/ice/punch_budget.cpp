#include "punch_budget.h"

#include <algorithm>

namespace minirtc {
uint64_t PunchProcessBudget::Acquire() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_.size() >= 2 || next_ == UINT64_MAX) return 0;
  const auto id = ++next_;
  active_.insert(id);
  return id;
}
void PunchProcessBudget::Release(uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  active_.erase(id);
  waiting_.erase(std::remove_if(waiting_.begin(), waiting_.end(),
                                [&](const Waiter& w) { return w.id == id; }),
                 waiting_.end());
}
bool PunchProcessBudget::Take(uint64_t id, bool ack, int64_t now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_.count(id)) return false;
  auto own = std::find_if(waiting_.begin(), waiting_.end(),
                          [&](const Waiter& w) { return w.id == id; });
  if (own == waiting_.end())
    waiting_.push_back({id, ack});
  else
    own->ack = ack;
  auto next = std::find_if(waiting_.begin(), waiting_.end(),
                           [](const Waiter& w) { return w.ack; });
  if (next == waiting_.end()) next = waiting_.begin();
  if (next->id != id || !bucket_.Take(now)) return false;
  waiting_.erase(next);
  return true;
}
PunchProcessBudget& PunchProcessBudget::Shared() {
  // GLib's monotonic clock and steady_clock need not share an epoch. Start at
  // zero; the first request saturates the bounded burst instead of assuming
  // one.
  static PunchProcessBudget instance(0);
  return instance;
}
}  // namespace minirtc
