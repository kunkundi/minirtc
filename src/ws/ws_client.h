/*
 * @Author: DI JUNKUN
 * @Date: 2024-09-10
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WS_CLIENT_H_
#define _WS_CLIENT_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "websocketpp/client.hpp"
#include "websocketpp/common/memory.hpp"
#include "websocketpp/config/asio_client.hpp"

struct WsClientConfig : websocketpp::config::asio_tls_client {
  struct transport_config
      : websocketpp::config::asio_tls_client::transport_config {
    static const long timeout_socket_post_init = 10000;
  };
  using transport_type =
      websocketpp::transport::asio::endpoint<transport_config>;
};
typedef websocketpp::client<WsClientConfig> client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>
    ssl_context_ptr;

enum WsStatus {
  WsOpening = 0,
  WsOpened,
  WsConnecting,
  WsFailed,
  WsClosed,
  WsReconnecting,
  WsServerClosed,
  WsTlsCertError
};

namespace minirtc {

class WsClient : public std::enable_shared_from_this<WsClient> {
 public:
  WsClient(std::function<void(const std::string&)> on_receive_msg_cb,
           std::function<void(WsStatus)> on_ws_status_cb);

  ~WsClient();

 public:
  void Shutdown();

  int Connect(const std::string& uri);

  int ReConnect();

  // Reset the backoff ladder and retry now. Used when the user explicitly asks
  // signaling to reconnect instead of waiting out the exponential delay.
  void ForceReconnect();

  void AsyncReConnect();

  void Close();

  void Send(const std::string& message);

  WsStatus GetStatus();

  // void OnSocketInit(client *c, websocketpp::connection_hdl hdl);

  ssl_context_ptr OnTlsInit(websocketpp::connection_hdl hdl);

  bool OnTlsVerify(bool preverified,
                   websocketpp::lib::asio::ssl::verify_context& ctx);

  void OnOpen(client* c, websocketpp::connection_hdl hdl);

  void OnFail(client* c, websocketpp::connection_hdl hdl);

  void OnClose(client* c, websocketpp::connection_hdl hdl);

  bool OnPing(websocketpp::connection_hdl hdl, std::string msg);

  bool OnPong(websocketpp::connection_hdl hdl, std::string msg);

  void OnMessage(websocketpp::connection_hdl hdl, client::message_ptr msg);

 private:
  static void LoadTlsSystemRootCertificates(SSL_CTX* ssl_ctx);

  static bool VerifyTlsWithSystemTrust(X509_STORE_CTX* store_ctx,
                                       const std::string& uri);

  static bool LogTlsVerificationError(X509_STORE_CTX* store_ctx);

  using Clock = std::chrono::steady_clock;
  struct Callback {
    uint64_t generation;
    Clock::time_point queued;
    size_t bytes;
    std::function<void(WsClient&)> run;
  };
  struct CallbackQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Callback> jobs;
    uint64_t generation = 0;
    size_t bytes = 0;
    bool executing = false;
    bool stopping = false;
  };
  void StartCallbacks();
  void InvalidateCallbacks();
  bool QueueCallback(std::function<void(WsClient&)> callback, size_t bytes = 0);
  void RegisterHandlers();

  void StopThreads();

  void RestartPingThread(websocketpp::connection_hdl hdl);

  void PingLoop(websocketpp::connection_hdl hdl);

  void SetStatus(WsStatus status);

  void CleanupPendingThreads();

  void ScheduleReconnect(int delay_seconds, bool force = false);

 private:
  struct Transport {
    std::shared_ptr<client> endpoint;
    websocketpp::connection_hdl handle;
  };
  Transport GetTransport();
  std::mutex transport_mutex_;
  // Also owned by the I/O worker until run() returns.
  std::shared_ptr<client> m_endpoint_;
  websocketpp::connection_hdl connection_handle_;

  std::thread m_thread_;
  std::thread ping_thread_;
  std::thread reconnect_thread_;
  std::vector<std::thread> pending_threads_;
  std::mutex pending_threads_mtx_;
  std::condition_variable reconnect_cv_;
  std::mutex reconnect_mtx_;
  // Protected by reconnect_mtx_. A force request wakes an existing delay but
  // does not start a second connection while the worker is already connecting.
  bool reconnect_waiting_ = false;
  bool reconnect_now_ = false;
  std::mutex shutdown_mtx_;

  std::string uri_;
  std::shared_ptr<CallbackQueue> callbacks_ = std::make_shared<CallbackQueue>();
  std::thread callback_thread_;
  std::atomic<int64_t> callback_queue_ms_{0}, callback_execution_ms_{0};

  std::atomic<bool> running_{false};
  std::mutex ping_mtx_;
  std::condition_variable cond_var_;

  std::atomic<bool> heartbeat_started_{false};
  unsigned int ping_interval_seconds_ = 3;
  // Protected by ping_mtx_. A new ping is sent only after the matching pong.
  std::string pending_ping_payload_;
  uint64_t ping_sequence_ = 0;
  Clock::time_point connection_opened_at_{}, ping_queued_at_{},
      ping_dispatched_at_{};
  Clock::time_point next_heartbeat_report_{};
  int64_t last_pong_rtt_ms_ = 0;
  bool ping_send_failed_ = false, connection_stable_ = false;

  std::atomic<WsStatus> ws_status_{WsStatus::WsClosed};
  std::atomic<bool> shutdown_{false};
  std::atomic<bool> is_reconnecting_{false};
  std::atomic<int> reconnect_attempts_{0};
  std::atomic<int> tls_failure_count_{0};
  std::atomic<uint64_t> reconnect_generation_{0};

  std::function<void(const std::string&)> on_receive_msg_ = nullptr;
  std::function<void(WsStatus)> on_ws_status_ = nullptr;
};

}  // namespace minirtc
#endif
