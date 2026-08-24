/*
 * @Author: DI JUNKUN
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ICE_COMPONENT_STATE_H_
#define _ICE_COMPONENT_STATE_H_

#include <atomic>

#include <nice/agent.h>

namespace minirtc {

enum class IcePathAvailabilityChange {
  Unchanged,
  BecameAvailable,
  BecameUnavailable,
};

// libnice requires the first nice_agent_send() to wait for READY. After a
// component has reached READY, libnice permits the previously selected pair to
// remain usable while an ICE restart temporarily moves the component through
// GATHERING, CONNECTING, or CONNECTED again. Keep that distinction in one
// thread-safe state tracker so media and congestion control use the same
// availability semantics.
class IceComponentStateTracker {
 public:
  IcePathAvailabilityChange Update(NiceComponentState state) noexcept {
    switch (state) {
      case NICE_COMPONENT_STATE_READY:
        return usable_.exchange(true)
                   ? IcePathAvailabilityChange::Unchanged
                   : IcePathAvailabilityChange::BecameAvailable;

      case NICE_COMPONENT_STATE_DISCONNECTED:
      case NICE_COMPONENT_STATE_FAILED:
        return usable_.exchange(false)
                   ? IcePathAvailabilityChange::BecameUnavailable
                   : IcePathAvailabilityChange::Unchanged;

      case NICE_COMPONENT_STATE_GATHERING:
      case NICE_COMPONENT_STATE_CONNECTING:
      case NICE_COMPONENT_STATE_CONNECTED:
      case NICE_COMPONENT_STATE_LAST:
        return IcePathAvailabilityChange::Unchanged;
    }

    return IcePathAvailabilityChange::Unchanged;
  }

  bool IsUsable() const noexcept { return usable_.load(); }

 private:
  std::atomic<bool> usable_{false};
};

}  // namespace minirtc

#endif
