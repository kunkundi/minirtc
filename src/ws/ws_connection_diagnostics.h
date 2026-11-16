/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-17
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WS_CONNECTION_DIAGNOSTICS_H_
#define _WS_CONNECTION_DIAGNOSTICS_H_

#ifndef NDEBUG

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <system_error>

namespace minirtc {

// Owned by one connection attempt, including callbacks that finish after a
// cancellation. Never attach request bodies, URI paths or credentials here.
class WsConnectionDiagnostics {
 public:
  WsConnectionDiagnostics(uint64_t client_id, uint64_t attempt);
  void Event(const char* event, const std::error_code& error = {}) noexcept;
  void Target(const std::string& host, uint16_t port) noexcept;
  void Endpoints(const std::string& local, const std::string& remote) noexcept;

 private:
  using Clock = std::chrono::steady_clock;
  const uint64_t client_id_, attempt_;
  const Clock::time_point started_ = Clock::now();
  std::mutex mutex_;
  Clock::time_point tls_started_{}, tls_finished_{}, shutdown_started_{},
      shutdown_finished_{}, transport_finished_{};
};

}  // namespace minirtc

#endif  // !NDEBUG

#endif
