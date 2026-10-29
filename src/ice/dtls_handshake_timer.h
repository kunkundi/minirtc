/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-12
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DTLS_HANDSHAKE_TIMER_H_
#define _DTLS_HANDSHAKE_TIMER_H_

// Only timeval is needed here. GIO's networking header exposes Windows
// resolver macros that can break ASIO when included between its headers.
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

#include <glib.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace minirtc {

// Runs on the same GMainContext as the SSL receive callbacks. Stop before
// freeing SSL, or after joining that context's thread during teardown.
class DtlsHandshakeTimer {
 public:
  enum class Failure { Deadline, Retransmission };
  using FailureCallback = std::function<void(Failure)>;

  DtlsHandshakeTimer() = default;
  DtlsHandshakeTimer(const DtlsHandshakeTimer&) = delete;
  DtlsHandshakeTimer& operator=(const DtlsHandshakeTimer&) = delete;
  ~DtlsHandshakeTimer() { Stop(); }

  void Start(SSL* ssl, GMainContext* context, std::chrono::milliseconds timeout,
             FailureCallback on_failure) {
    Stop();
    ssl_ = ssl;
    context_ = context;
    deadline_us_ = g_get_monotonic_time() + timeout.count() * 1000;
    on_failure_ = std::move(on_failure);
    Refresh();
  }

  void Refresh() {
    ClearSource();
    if (!ssl_ || !context_) return;
    const gint64 now = g_get_monotonic_time();
    if (now >= deadline_us_) {
      Fail(Failure::Deadline);
      return;
    }
    gint64 remaining_us = deadline_us_ - now;
    timeval timeout{};
    if (DTLSv1_get_timeout(ssl_, &timeout)) {
      const gint64 retry_us =
          static_cast<gint64>(timeout.tv_sec) * 1000000 + timeout.tv_usec;
      remaining_us = std::min(remaining_us, retry_us);
    }
    // Keep an already-expired retry ready, even under continuous incoming
    // packets; rounding to a fresh millisecond on each refresh can starve it.
    source_ = g_timeout_source_new(0);
    g_source_set_ready_time(source_, now + remaining_us);
    g_source_set_callback(source_, Tick, this, nullptr);
    g_source_attach(source_, context_);
  }

  void Stop() {
    ClearSource();
    ssl_ = nullptr;
    context_ = nullptr;
    on_failure_ = {};
  }

 private:
  void ClearSource() {
    if (!source_) return;
    g_source_destroy(source_);
    g_source_unref(source_);
    source_ = nullptr;
  }

  void Fail(Failure failure) {
    auto callback = std::move(on_failure_);
    Stop();
    if (callback) callback(failure);
  }

  static gboolean Tick(gpointer data) {
    auto* self = static_cast<DtlsHandshakeTimer*>(data);
    self->ClearSource();
    if (g_get_monotonic_time() >= self->deadline_us_) {
      self->Fail(Failure::Deadline);
    } else {
      ERR_clear_error();
      if (DTLSv1_handle_timeout(self->ssl_) < 0) {
        self->Fail(Failure::Retransmission);
      } else {
        self->Refresh();
      }
    }
    return G_SOURCE_REMOVE;
  }

  SSL* ssl_ = nullptr;
  GMainContext* context_ = nullptr;
  GSource* source_ = nullptr;
  gint64 deadline_us_ = 0;
  FailureCallback on_failure_;
};

}  // namespace minirtc

#endif
