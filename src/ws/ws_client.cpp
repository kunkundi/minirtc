#include "ws_client.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>
#if defined(__ANDROID__)
#include <asio/ssl/host_name_verification.hpp>
#endif

#include "log.h"

namespace minirtc {

WsClient::WsClient(std::function<void(const std::string&)> on_receive_msg_cb,
                   std::function<void(WsStatus)> on_ws_status_cb)
    : on_receive_msg_(on_receive_msg_cb), on_ws_status_(on_ws_status_cb) {}

namespace {
thread_local const WsClient* active_callback_client = nullptr;

void JoinThread(std::thread& thread) {
  if (!thread.joinable()) {
    return;
  }

  // A worker can temporarily own the last shared_ptr to WsClient. Never try to
  // join that worker from its own destructor path.
  if (thread.get_id() == std::this_thread::get_id()) {
    thread.detach();
    return;
  }

  thread.join();
}

}  // namespace

WsClient::~WsClient() { Shutdown(); }

void WsClient::Shutdown() {
  // A callback may request shutdown while another thread is already draining
  // it.
  if (active_callback_client == this && shutdown_) return;
  std::lock_guard<std::mutex> shutdown_lock(shutdown_mtx_);
  if (shutdown_.load() && !m_endpoint_ && !m_thread_.joinable() &&
      !ping_thread_.joinable() && !reconnect_thread_.joinable() &&
      !callback_thread_.joinable()) {
    return;
  }
  LOG_INFO("WebSocket shutdown started");

  // Shutdown is terminal. In particular, wake and invalidate a delayed
  // reconnect before waiting for any worker so an obsolete client cannot be
  // revived after its PeerConnection has been destroyed.
  {
    std::lock_guard<std::mutex> lock(reconnect_mtx_);
    shutdown_ = true;
    reconnect_generation_.fetch_add(1);
  }
  running_ = false;
  cond_var_.notify_all();
  reconnect_cv_.notify_all();
  callbacks_->ready.notify_all();

  std::vector<std::thread> reconnect_threads;
  {
    std::lock_guard<std::mutex> lock(pending_threads_mtx_);
    if (reconnect_thread_.joinable()) {
      reconnect_threads.push_back(std::move(reconnect_thread_));
    }
    for (auto& thread : pending_threads_) {
      reconnect_threads.push_back(std::move(thread));
    }
    pending_threads_.clear();
  }

  for (auto& thread : reconnect_threads) {
    JoinThread(thread);
  }
  LOG_DEBUG("WebSocket reconnect workers stopped");

  // A reconnect that was already running may have restarted the heartbeat.
  running_ = false;
  cond_var_.notify_all();
  JoinThread(ping_thread_);
  LOG_DEBUG("WebSocket heartbeat worker stopped");

  if (m_endpoint_) {
    m_endpoint_->stop_perpetual();
    m_endpoint_->stop();
  }
  // stop() ends asynchronous network waits. An executing callback must still
  // finish before Close returns: PeerConnection callbacks capture their owner
  // by raw pointer, and DestroyPeer deletes that owner immediately afterwards.
  JoinThread(m_thread_);
  LOG_DEBUG("WebSocket I/O worker stopped");

  {
    std::lock_guard lock(callbacks_->mutex);
    callbacks_->stopping = true;
    callbacks_->jobs.clear();
    callbacks_->bytes = 0;
  }
  callbacks_->ready.notify_all();
  JoinThread(callback_thread_);
  {
    std::lock_guard lock(transport_mutex_);
    m_endpoint_.reset();
    connection_handle_.reset();
  }
  heartbeat_started_ = false;
  LOG_INFO("WebSocket shutdown completed");
}

void WsClient::StopThreads() {
  running_ = false;
  cond_var_.notify_all();

  JoinThread(ping_thread_);

  CleanupPendingThreads();

  if (m_endpoint_) {
    // stop_perpetual alone does not end run() while a handshake or timer is
    // still outstanding, which is what used to hang ReConnect here forever.
    m_endpoint_->stop_perpetual();
    m_endpoint_->stop();
  }
  JoinThread(m_thread_);
  InvalidateCallbacks();

  {
    std::lock_guard lock(transport_mutex_);
    m_endpoint_.reset();
    connection_handle_.reset();
  }
  heartbeat_started_ = false;
}

void WsClient::StartCallbacks() {
  if (callback_thread_.joinable()) return;
  auto queue = callbacks_;
  std::weak_ptr<WsClient> weak_self = shared_from_this();
  callback_thread_ = std::thread([queue, weak_self] {
    std::unique_lock lock(queue->mutex);
    while (!queue->stopping) {
      queue->ready.wait(
          lock, [&] { return queue->stopping || !queue->jobs.empty(); });
      if (queue->stopping) break;
      auto job = std::move(queue->jobs.front());
      queue->jobs.pop_front();
      queue->bytes -= job.bytes;
      if (job.generation != queue->generation) continue;
      queue->executing = true;
      lock.unlock();
      if (auto self = weak_self.lock()) {
        if (!self->shutdown_) {
          active_callback_client = self.get();
          const auto started = Clock::now();
          self->callback_queue_ms_ =
              std::chrono::duration_cast<std::chrono::milliseconds>(started -
                                                                    job.queued)
                  .count();
          try {
            job.run(*self);
          } catch (const std::exception&) {
            // Message bodies and parser errors can contain credentials.
            LOG_ERROR("WebSocket application callback failed");
            self->AsyncReConnect();
          } catch (...) {
            LOG_ERROR(
                "WebSocket application callback failed with unknown exception");
            self->AsyncReConnect();
          }
          self->callback_execution_ms_ =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  Clock::now() - started)
                  .count();
          if (self->callback_execution_ms_ >= 1000 ||
              self->callback_queue_ms_ >= 1000)
            LOG_WARN("WebSocket callback delayed: queue_ms={} execution_ms={}",
                     self->callback_queue_ms_.load(),
                     self->callback_execution_ms_.load());
          active_callback_client = nullptr;
        }
      }
      lock.lock();
      queue->executing = false;
      queue->ready.notify_all();
    }
  });
}

void WsClient::InvalidateCallbacks() {
  std::unique_lock lock(callbacks_->mutex);
  ++callbacks_->generation;
  callbacks_->jobs.clear();
  callbacks_->bytes = 0;
  if (active_callback_client != this)
    callbacks_->ready.wait(lock, [&] {
      // A callback can initiate Shutdown while a reconnect is draining it.
      // Shutdown owns the final join, and Connect rejects terminal shutdown.
      return !callbacks_->executing || shutdown_.load();
    });
}

bool WsClient::QueueCallback(std::function<void(WsClient&)> callback,
                             size_t bytes) {
  std::lock_guard lock(callbacks_->mutex);
  if (shutdown_ || callbacks_->stopping) return false;
  // Reserve a few slots for connection status transitions during overload.
  const size_t limit = bytes ? 128 : 144;
  if (callbacks_->jobs.size() >= limit ||
      bytes > 1024 * 1024 - callbacks_->bytes)
    return false;
  callbacks_->jobs.push_back(
      {callbacks_->generation, Clock::now(), bytes, std::move(callback)});
  callbacks_->bytes += bytes;
  callbacks_->ready.notify_one();
  return true;
}

void WsClient::CleanupPendingThreads() {
  // Increment generation to signal all pending threads to exit.
  {
    std::lock_guard<std::mutex> lock(reconnect_mtx_);
    reconnect_generation_.fetch_add(1);
  }
  reconnect_cv_.notify_all();

  std::vector<std::thread> pending_threads;
  {
    std::lock_guard<std::mutex> lock(pending_threads_mtx_);
    pending_threads.swap(pending_threads_);
  }

  for (auto& thread : pending_threads) {
    JoinThread(thread);
  }
}

void WsClient::ScheduleReconnect(int delay_seconds, bool force) {
  // Serialize a forced wake with both scheduling and entering the wait, so a
  // click just before the worker starts waiting cannot be lost.
  std::unique_lock<std::mutex> schedule_lock(reconnect_mtx_);
  if (shutdown_) {
    return;
  }
  if (force) {
    reconnect_attempts_ = 0;
  }

  // ensure only one reconnect attempt is scheduled at a time.
  bool expected = false;
  if (!is_reconnecting_.compare_exchange_strong(expected, true)) {
    if (force && reconnect_waiting_) {
      reconnect_now_ = true;
      reconnect_cv_.notify_all();
    }
    LOG_DEBUG("Reconnect already in progress, skipping.");
    return;
  }
  if (delay_seconds < 0) {
    const int attempt = std::min(reconnect_attempts_.load() + 1, 6);
    reconnect_attempts_ = attempt;
    delay_seconds = std::min(1 << attempt, 60);
    LOG_INFO("Scheduling signaling reconnect: attempt={} base_seconds={}",
             attempt, delay_seconds);
  }
  reconnect_waiting_ = true;
  reconnect_now_ = force;

  auto delay = std::chrono::milliseconds::zero();
  if (!force) {
    // Add jitter rather than shortening the backoff, so spreading retries does
    // not increase sustained load. A 60s base waits 60-90s.
    // Manual retries remain immediate.
    static thread_local std::mt19937 random_engine(std::random_device{}());
    const int base_ms = delay_seconds * 1000;
    const int jitter_ms = base_ms > 0 ? base_ms / 2 : 1000;
    delay = std::chrono::milliseconds(
        base_ms + std::uniform_int_distribution<int>(0, jitter_ms)(random_engine));
  }

  LOG_INFO("Will retry connection after {} ms (base backoff: {} seconds)",
           delay.count(), delay_seconds);

  // capture current generation to detect cancellation
  uint64_t current_generation = reconnect_generation_.load();

  std::weak_ptr<WsClient> weak_self = shared_from_this();
  std::thread reconnect_thread([weak_self, delay,
                                current_generation]() {
    if (auto self = weak_self.lock()) {
      // Keep certificate failures visible during backoff instead of immediately
      // replacing the actionable error with a generic reconnecting status.
      if (self->GetStatus() != WsTlsCertError) {
        self->SetStatus(WsReconnecting);
      }
      {
        // use interruptible wait instead of sleep
        std::unique_lock<std::mutex> lock(self->reconnect_mtx_);
        self->reconnect_cv_.wait_for(
            lock, delay,
            [&self, current_generation]() {
              return self->shutdown_.load() || self->reconnect_now_ ||
                     self->reconnect_generation_.load() != current_generation;
            });
        self->reconnect_waiting_ = false;
        self->reconnect_now_ = false;

        // Generation changed or shutdown started; cancel this attempt.
        if (self->shutdown_ ||
            self->reconnect_generation_.load() != current_generation) {
          self->is_reconnecting_.store(false);
          return;
        }
      }

      self->ReConnect();
      std::lock_guard<std::mutex> lock(self->reconnect_mtx_);
      self->is_reconnecting_.store(false);
    }
  });

  // Serialize publication of the new worker with Shutdown(). Recheck the
  // terminal flag while holding the same mutex so Shutdown cannot miss a
  // reconnect thread created concurrently.
  {
    std::lock_guard<std::mutex> lock(pending_threads_mtx_);
    if (shutdown_) {
      reconnect_generation_.fetch_add(1);
      reconnect_cv_.notify_all();
      is_reconnecting_ = false;
    } else {
      if (reconnect_thread_.joinable()) {
        pending_threads_.push_back(std::move(reconnect_thread_));
      }
      reconnect_thread_ = std::move(reconnect_thread);
      return;
    }
  }

  schedule_lock.unlock();
  JoinThread(reconnect_thread);
}

void WsClient::RegisterHandlers() {
  m_endpoint_->set_error_channels(websocketpp::log::elevel::none);
  m_endpoint_->set_access_channels(websocketpp::log::alevel::none);

  std::weak_ptr<WsClient> weak_self = shared_from_this();
  // m_endpoint_->set_socket_init_handler(
  //     websocketpp::lib::bind(&WsClient::OnSocketInit, this,
  //     m_endpoint_.get(), websocketpp::lib::placeholders::_1));
  m_endpoint_->set_tls_init_handler(
      [weak_self](websocketpp::connection_hdl hdl) {
        if (auto self = weak_self.lock()) {
          return self->OnTlsInit(hdl);
        }
        return ssl_context_ptr();
      });
  m_endpoint_->set_open_handler([weak_self, endpoint = m_endpoint_.get()](
                                    websocketpp::connection_hdl hdl) {
    if (auto self = weak_self.lock()) {
      self->OnOpen(endpoint, hdl);
    }
  });
  m_endpoint_->set_fail_handler([weak_self, endpoint = m_endpoint_.get()](
                                    websocketpp::connection_hdl hdl) {
    if (auto self = weak_self.lock()) {
      self->OnFail(endpoint, hdl);
    }
  });
  m_endpoint_->set_close_handler([weak_self, endpoint = m_endpoint_.get()](
                                     websocketpp::connection_hdl hdl) {
    if (auto self = weak_self.lock()) {
      self->OnClose(endpoint, hdl);
    }
  });
  m_endpoint_->set_ping_handler(
      [weak_self](websocketpp::connection_hdl hdl, std::string msg) {
        if (auto self = weak_self.lock()) {
          return self->OnPing(hdl, msg);
        }
        return false;
      });
  m_endpoint_->set_pong_handler(
      [weak_self](websocketpp::connection_hdl hdl, std::string msg) {
        if (auto self = weak_self.lock()) {
          return self->OnPong(hdl, msg);
        }
        return false;
      });
  // PingLoop owns a steady-clock deadline. websocketpp restarts its pong timer
  // for every ping and cancels it for any pong, including unrelated replies.
  m_endpoint_->set_pong_timeout(0);
  m_endpoint_->set_message_handler(
      [weak_self](websocketpp::connection_hdl hdl, client::message_ptr msg) {
        if (auto self = weak_self.lock()) {
          self->OnMessage(hdl, msg);
        }
      });
}

int WsClient::Connect(const std::string& uri) {
  if (shutdown_) {
    return -1;
  }

  LOG_INFO("Connecting WebSocket");

  StopThreads();
  uri_ = uri;

  StartCallbacks();
  tls_failure_count_ = 0;
  {
    std::lock_guard lock(transport_mutex_);
    m_endpoint_ = std::make_shared<client>();
  }
  SetStatus(WsOpening);
  m_endpoint_->init_asio();
  m_endpoint_->start_perpetual();

  RegisterHandlers();
  // Keep the endpoint alive while run() unwinds, including when the final
  // WsClient reference is released by a callback on this worker.
  m_thread_ = std::thread([endpoint = m_endpoint_]() { endpoint->run(); });

  websocketpp::lib::error_code ec;
  auto con = m_endpoint_->get_connection(uri, ec);
  if (ec || !con) {
    LOG_ERROR("get_connection error: {}", ec.message());
    StopThreads();
    return -1;
  }

  m_endpoint_->connect(con);
  SetStatus(WsConnecting);
  return 0;
}

int WsClient::ReConnect() {
  if (shutdown_) {
    return -1;
  }

  // ScheduleReconnect serializes attempts and publishes WsReconnecting before
  // its backoff wait; that status must not prevent the actual connection work.
  LOG_INFO("Reconnecting WebSocket...");
  if (ws_status_ != WsReconnecting) {
    SetStatus(WsReconnecting);
  }

  websocketpp::lib::error_code ec;
  const auto transport = GetTransport();
  if (transport.endpoint && !transport.handle.expired()) {
    auto con = transport.endpoint->get_con_from_hdl(transport.handle, ec);
    if (!ec && con && con->get_state() == websocketpp::session::state::open) {
      transport.endpoint->close(transport.handle,
                                websocketpp::close::status::going_away,
                                "Reconnect", ec);
    }
  }

  StopThreads();

  return Connect(uri_);
}

void WsClient::AsyncReConnect() {
  if (shutdown_) {
    return;
  }

  ScheduleReconnect(-1);
}

void WsClient::ForceReconnect() {
  LOG_INFO("Forcing a signaling reconnect");
  ScheduleReconnect(0, true);
}

void WsClient::Close() {
  Shutdown();
}

WsClient::Transport WsClient::GetTransport() {
  std::lock_guard lock(transport_mutex_);
  return {m_endpoint_, connection_handle_};
}

void WsClient::Send(const std::string& message) {
  websocketpp::lib::error_code ec;
  const auto transport = GetTransport();
  if (!transport.endpoint) {
    LOG_WARN("Send failed: WebSocket endpoint is unavailable");
    return;
  }
  auto con = transport.endpoint->get_con_from_hdl(transport.handle, ec);
  if (ec || !con || con->get_state() != websocketpp::session::state::open) {
    LOG_WARN("Send failed: not connected or error: {}", ec.message());
    return;
  }
  transport.endpoint->send(transport.handle, message,
                           websocketpp::frame::opcode::text, ec);
  if (ec) {
    // Signaling payloads may contain passwords or reconnect bearer tokens.
    LOG_ERROR("Sending message error: {}", ec.message());
  }
}

WsStatus WsClient::GetStatus() { return ws_status_; }

void WsClient::SetStatus(WsStatus status) {
  ws_status_ = status;
  if (!shutdown_ && on_ws_status_) {
    QueueCallback([status](WsClient& self) { self.on_ws_status_(status); });
  }
}

void WsClient::RestartPingThread(websocketpp::connection_hdl hdl) {
  if (shutdown_) {
    return;
  }

  if (ping_thread_.joinable()) {
    running_ = false;
    cond_var_.notify_all();
    JoinThread(ping_thread_);
  }

  {
    std::lock_guard<std::mutex> lock(ping_mtx_);
    pending_ping_payload_.clear();
    connection_opened_at_ = Clock::now();
    next_heartbeat_report_ = connection_opened_at_ + std::chrono::seconds(60);
    connection_stable_ = false;
    last_pong_rtt_ms_ = 0;
  }
  running_ = true;
  ping_thread_ = std::thread(&WsClient::PingLoop, this, hdl);
  heartbeat_started_ = true;
}

void WsClient::PingLoop(websocketpp::connection_hdl hdl) {
  constexpr auto kWarningTimeout = std::chrono::seconds(5);
  constexpr auto kPongTimeout = std::chrono::seconds(15);
  const auto interval = std::chrono::seconds(ping_interval_seconds_);
  auto next_ping = Clock::now() + interval;
  const auto endpoint = m_endpoint_;
  std::weak_ptr<WsClient> weak_self = shared_from_this();
  std::unique_lock lock(ping_mtx_);
  while (running_) {
    cond_var_.wait_until(lock, next_ping, [this] { return !running_; });
    if (!running_) break;
    ping_queued_at_ = Clock::now();
    ping_dispatched_at_ = {};
    ping_send_failed_ = false;
    next_ping = ping_queued_at_ + interval;
    pending_ping_payload_ = std::to_string(++ping_sequence_);
    const auto payload = pending_ping_payload_;
    endpoint->get_io_service().post([weak_self, endpoint, hdl, payload] {
      auto self = weak_self.lock();
      if (!self) return;
      std::lock_guard lock(self->ping_mtx_);
      if (!self->running_ || self->pending_ping_payload_ != payload) return;
      self->ping_dispatched_at_ = Clock::now();
      websocketpp::lib::error_code ec;
      endpoint->ping(hdl, payload, ec);
      if (ec) {
        LOG_WARN("WebSocket ping dispatch failed: {}", ec.message());
        self->ping_send_failed_ = true;
        self->cond_var_.notify_all();
      }
    });
    const auto finished = [this] {
      return !running_ || ping_send_failed_ || pending_ping_payload_.empty();
    };
    if (!cond_var_.wait_until(lock, ping_queued_at_ + kWarningTimeout,
                              finished)) {
      const auto queue_ms =
          ping_dispatched_at_ == Clock::time_point{}
              ? -1
              : std::chrono::duration_cast<std::chrono::milliseconds>(
                    ping_dispatched_at_ - ping_queued_at_)
                    .count();
      LOG_WARN(
          "WebSocket heartbeat delayed: seq={} io_queue_ms={} "
          "callback_queue_ms={} callback_execution_ms={} timeout_ms=15000",
          ping_sequence_, queue_ms, callback_queue_ms_.load(),
          callback_execution_ms_.load());
      if (!cond_var_.wait_until(lock, ping_queued_at_ + kPongTimeout,
                                finished)) {
        LOG_WARN(
            "Pong timeout exceeded: seq={} dispatched={} last_rtt_ms={}; "
            "reconnecting",
            ping_sequence_, ping_dispatched_at_ != Clock::time_point{},
            last_pong_rtt_ms_);
        break;
      }
    }
    if (ping_send_failed_) break;
  }
  pending_ping_payload_.clear();
  const bool connection_lost = running_.exchange(false);
  lock.unlock();
  if (connection_lost && !shutdown_) AsyncReConnect();
}

// void WsClient::OnSocketInit(client *, websocketpp::connection_hdl) {
//   LOG_INFO("OnSocketInit");
// }

ssl_context_ptr WsClient::OnTlsInit(websocketpp::connection_hdl) {
  namespace asio = websocketpp::lib::asio;
  auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::sslv23);
  try {
    ctx->set_options(
        asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
        asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);
    ctx->set_verify_mode(asio::ssl::verify_peer |
                         asio::ssl::verify_fail_if_no_peer_cert);

    LoadTlsSystemRootCertificates(ctx->native_handle());

    std::weak_ptr<WsClient> weak_self = shared_from_this();
    ctx->set_verify_callback(
        [weak_self](bool preverified, asio::ssl::verify_context& ctx) {
          if (auto self = weak_self.lock()) {
            return self->OnTlsVerify(preverified, ctx);
          }
          return false;
        });
  } catch (std::exception& e) {
    LOG_ERROR("TLS init error: {}", e.what());
  }
  return ctx;
}

bool WsClient::OnTlsVerify(bool preverified,
                           websocketpp::lib::asio::ssl::verify_context& ctx) {
  if (!preverified &&
      VerifyTlsWithSystemTrust(ctx.native_handle(), uri_)) {
    tls_failure_count_ = 0;
    return true;
  }

#if defined(__ANDROID__)
  // A valid CA chain alone does not authenticate the configured endpoint.
  // Verify DNS names and IP SANs using the same host used by WebSocket SNI.
  if (preverified &&
      !websocketpp::lib::asio::ssl::host_name_verification(
          websocketpp::uri(uri_).get_host())(preverified, ctx)) {
    X509_STORE_CTX_set_error(ctx.native_handle(), X509_V_ERR_HOSTNAME_MISMATCH);
    preverified = false;
  }
#endif
  if (!preverified) {
#if defined(__ANDROID__)
    LogTlsVerificationError(ctx.native_handle());
    SetStatus(WsTlsCertError);
#else
    if (LogTlsVerificationError(ctx.native_handle())) {
      SetStatus(WsTlsCertError);
    }
#endif
    tls_failure_count_++;
  } else {
    tls_failure_count_ = 0;
  }
  return preverified;
}

void WsClient::OnOpen(client* c, websocketpp::connection_hdl hdl) {
  // Ignore callbacks during shutdown or from a replaced endpoint.
  if (shutdown_ || c != GetTransport().endpoint.get()) {
    return;
  }
  LOG_INFO("WebSocket connection opened");
  {
    std::lock_guard lock(transport_mutex_);
    connection_handle_ = hdl;
  }
  SetStatus(WsOpened);
  tls_failure_count_ = 0;
  RestartPingThread(hdl);
}

void WsClient::OnFail(client* c, websocketpp::connection_hdl hdl) {
  if (shutdown_ || c != GetTransport().endpoint.get()) {
    return;
  }
  auto con = c->get_con_from_hdl(hdl);
  std::string error_msg = con ? con->get_ec().message() : "unknown";
  websocketpp::lib::error_code ec =
      con ? con->get_ec() : websocketpp::lib::error_code();

  const bool certificate_failed = tls_failure_count_.load() > 0;
  const bool wrapped_transport_error =
      ec == websocketpp::transport::error::pass_through ||
      ec == websocketpp::transport::error::tls_error;
  const auto transport_error = con && wrapped_transport_error
                                   ? con->get_transport_ec()
                                   : websocketpp::lib::asio::error_code();
  // Fixed fields are safe for credential-filtered controller diagnostics.
  LOG_WARN("Signaling transport failure: category={} code={} transport_category={} transport_code={} certificate_failed={}",
           ec.category().name(), ec.value(), transport_error.category().name(),
           transport_error.value(), certificate_failed);
  bool is_tls_error = certificate_failed;
  if (con) {
    std::string ec_msg = error_msg;
    std::transform(ec_msg.begin(), ec_msg.end(), ec_msg.begin(), ::tolower);

    if (ec_msg.find("ssl") != std::string::npos ||
        ec_msg.find("tls") != std::string::npos ||
        ec_msg.find("certificate") != std::string::npos ||
        ec_msg.find("handshake") != std::string::npos ||
        ec_msg.find("verify") != std::string::npos) {
      is_tls_error = true;
    }

    if (ec.category() == websocketpp::lib::asio::error::get_ssl_category()) {
      is_tls_error = true;
    }
  }

  if (is_tls_error) {
    LOG_ERROR("TLS connection failed: {} (TLS failure count: {})", error_msg,
              tls_failure_count_.load());

    if (certificate_failed) {
      if (GetStatus() != WsTlsCertError) {
        SetStatus(WsTlsCertError);
      }
      LOG_WARN(
          "Server certificate verification failed; retrying automatically "
          "with certificate verification enabled");
    }

    if (!shutdown_) ScheduleReconnect(-1);
  } else {
    LOG_WARN("Connection failed (non-TLS error): {}", error_msg);
    if (!shutdown_) {
      AsyncReConnect();
    }
  }
}

void WsClient::OnClose(client* c, websocketpp::connection_hdl hdl) {
  if (c != GetTransport().endpoint.get()) {
    return;
  }
  auto con = c->get_con_from_hdl(hdl);
  // blank/abnormal_close means the transport died without a close frame, which
  // is what separates a server-initiated shutdown from a network drop.
  const websocketpp::close::status::value close_code =
      con ? con->get_remote_close_code()
          : websocketpp::close::status::blank;
  LOG_WARN("Signaling transport closed: code={}", static_cast<int>(close_code));
  running_ = false;
  cond_var_.notify_all();
  if (!shutdown_ && ws_status_ != WsReconnecting) {
    AsyncReConnect();
  }
}

bool WsClient::OnPing(websocketpp::connection_hdl, std::string) { return true; }

bool WsClient::OnPong(websocketpp::connection_hdl hdl, std::string msg) {
  std::lock_guard<std::mutex> lock(ping_mtx_);
  const auto transport = GetTransport();
  if (!running_ || pending_ping_payload_.empty() ||
      hdl.owner_before(transport.handle) ||
      transport.handle.owner_before(hdl) || msg != pending_ping_payload_) {
    return true;
  }
  const auto now = Clock::now();
  last_pong_rtt_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - ping_queued_at_)
                          .count();
  if (!connection_stable_ &&
      now - connection_opened_at_ >= std::chrono::seconds(60)) {
    connection_stable_ = true;
    reconnect_attempts_ = 0;
    LOG_INFO("Signaling connection stable; reconnect backoff reset");
  }
  if (now >= next_heartbeat_report_ || last_pong_rtt_ms_ >= 5000) {
    const auto queue_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              ping_dispatched_at_ - ping_queued_at_)
                              .count();
    LOG_INFO(
        "WebSocket heartbeat: seq={} rtt_ms={} io_queue_ms={} "
        "callback_queue_ms={} callback_execution_ms={}",
        ping_sequence_, last_pong_rtt_ms_, queue_ms, callback_queue_ms_.load(),
        callback_execution_ms_.load());
    next_heartbeat_report_ = now + std::chrono::seconds(60);
  }
  pending_ping_payload_.clear();
  cond_var_.notify_all();
  return true;
}

void WsClient::OnMessage(websocketpp::connection_hdl, client::message_ptr msg) {
  if (!on_receive_msg_ || shutdown_) return;
  const auto& payload = msg->get_payload();
  if (!QueueCallback(
          [payload](WsClient& self) { self.on_receive_msg_(payload); },
          payload.size())) {
    LOG_WARN("WebSocket application queue full; reconnecting");
    AsyncReConnect();
  }
}
}  // namespace minirtc
