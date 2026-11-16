#include "ws_connection_diagnostics.h"

#ifndef NDEBUG

#include <string_view>

#include "log.h"

namespace minirtc {

WsConnectionDiagnostics::WsConnectionDiagnostics(uint64_t client_id,
                                                 uint64_t attempt)
    : client_id_(client_id), attempt_(attempt) {}

void WsConnectionDiagnostics::Event(const char* event,
                                    const std::error_code& error) noexcept {
  try {
    std::lock_guard lock(mutex_);
    const auto now = Clock::now();
    const std::string_view name(event);
    if (name == "tls_start") tls_started_ = now;
    if (name == "tls_complete") tls_finished_ = now;
    if (name == "tls_shutdown_start") shutdown_started_ = now;
    if (name == "tls_shutdown_complete") shutdown_finished_ = now;
    if (transport_finished_ == Clock::time_point{} &&
        (name == "websocket_failed" || name == "websocket_closed" ||
         name == "workers_stop_complete" || name == "client_shutdown_complete"))
      transport_finished_ = now;
    const auto duration = [now](Clock::time_point start,
                                Clock::time_point end) {
      return start == Clock::time_point{}
                 ? int64_t{-1}
                 : std::chrono::duration_cast<std::chrono::milliseconds>(
                       (end == Clock::time_point{} ? now : end) - start)
                       .count();
    };
    // A timeout can start shutdown before the canceled handshake callback runs.
    // Stop measuring the handshake at that boundary, not at its late callback.
    auto tls_end = tls_finished_;
    if (shutdown_started_ != Clock::time_point{} &&
        (tls_end == Clock::time_point{} || shutdown_started_ < tls_end))
      tls_end = shutdown_started_;
    // A timer can end the transport without a shutdown callback. Freeze phase
    // durations there so retry backoff and late cancellation aren't included.
    const auto phase_end = [this](Clock::time_point end) {
      if (transport_finished_ != Clock::time_point{} &&
          (end == Clock::time_point{} || transport_finished_ < end))
        return transport_finished_;
      return end;
    };
    const auto elapsed = duration(started_, now);
    const auto tls_ms = duration(tls_started_, phase_end(tls_end));
    const auto shutdown_ms =
        duration(shutdown_started_, phase_end(shutdown_finished_));
    // This scalar-only record is also allowed in controller diagnostics.
    LOG_INFO(
        "Signaling stage: client={} attempt={} event={} elapsed_ms={} "
        "tls_ms={} shutdown_ms={} tls_callback={} shutdown_callback={} "
        "category={} code={}",
        client_id_, attempt_, event, elapsed, tls_ms, shutdown_ms,
        tls_finished_ != Clock::time_point{},
        shutdown_finished_ != Clock::time_point{}, error.category().name(),
        error.value());
    if (error) {
      // Only native/library error descriptions; never a server message body.
      LOG_WARN(
          "Signaling error detail: client={} attempt={} event={} "
          "category={} code={} message=[{}]",
          client_id_, attempt_, event, error.category().name(), error.value(),
          error.message());
    }
  } catch (...) {
    // Instrumentation must not change transport callbacks or retry behavior.
  }
}

void WsConnectionDiagnostics::Target(const std::string& host,
                                     uint16_t port) noexcept {
  try {
    LOG_INFO("Signaling target: client={} attempt={} host={} port={}",
             client_id_, attempt_, host, port);
  } catch (...) {
  }
}

void WsConnectionDiagnostics::Endpoints(const std::string& local,
                                        const std::string& remote) noexcept {
  try {
    LOG_INFO("Signaling endpoints: client={} attempt={} local={} remote={}",
             client_id_, attempt_, local, remote);
  } catch (...) {
  }
}

}  // namespace minirtc

#endif  // !NDEBUG
