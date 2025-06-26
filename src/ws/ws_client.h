/*
 * @Author: DI JUNKUN
 * @Date: 2024-09-10
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WS_CLIENT_H_
#define _WS_CLIENT_H_

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "websocketpp/client.hpp"
#include "websocketpp/common/memory.hpp"
#include "websocketpp/config/asio_client.hpp"

typedef websocketpp::client<websocketpp::config::asio_tls_client> client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>
    ssl_context_ptr;

enum WsStatus {
  WsOpening = 0,
  WsOpened,
  WsConnecting,
  WsFailed,
  WsClosed,
  WsReconnecting,
  WsServerClosed
};

class WsClient : public std::enable_shared_from_this<WsClient> {
 public:
  WsClient(std::function<void(const std::string &)> on_receive_msg_cb,
           std::function<void(WsStatus)> on_ws_status_cb);

  ~WsClient();

 public:
  void Shutdown();

  int Connect(const std::string &uri, const std::string &cert_path);

  int ReConnect();

  void AsyncReConnect();

  void Close();

  void Send(const std::string &message);

  WsStatus GetStatus();

  // void OnSocketInit(client *c, websocketpp::connection_hdl hdl);

  ssl_context_ptr OnTlsInit(websocketpp::connection_hdl hdl);

  void OnOpen(client *c, websocketpp::connection_hdl hdl);

  void OnFail(client *c, websocketpp::connection_hdl hdl);

  void OnClose(client *c, websocketpp::connection_hdl hdl);

  bool OnPing(websocketpp::connection_hdl hdl, std::string msg);

  bool OnPong(websocketpp::connection_hdl hdl, std::string msg);

  void OnPongTimeout(websocketpp::connection_hdl hdl, std::string msg);

  void OnMessage(websocketpp::connection_hdl hdl, client::message_ptr msg);

 private:
  void RegisterHandlers();

  void StopThreads();

  void RestartPingThread(websocketpp::connection_hdl hdl);

  void PingLoop(websocketpp::connection_hdl hdl);

  void SetStatus(WsStatus status);

 private:
  std::shared_ptr<client> m_endpoint_;
  websocketpp::connection_hdl connection_handle_;

  std::thread m_thread_;
  std::thread ping_thread_;
  std::thread reconnect_thread_;

  std::string uri_;
  std::string cert_path_;

  std::atomic<bool> running_{false};
  std::atomic<bool> is_reconnecting_{false};
  std::mutex ping_mtx_;
  std::condition_variable cond_var_;

  bool heartbeat_started_ = false;
  unsigned int ping_interval_seconds_ = 3;

  WsStatus ws_status_ = WsStatus::WsClosed;
  int timeout_count_ = 0;
  bool destructed_ = false;

  std::function<void(const std::string &)> on_receive_msg_ = nullptr;
  std::function<void(WsStatus)> on_ws_status_ = nullptr;
};

#endif
