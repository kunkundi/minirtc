/*
 * @Author: DI JUNKUN
 * @Date: 2024-09-10
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WS_CLIENT_H_
#define _WS_CLIENT_H_

#include <condition_variable>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "websocketpp/client.hpp"
#include "websocketpp/common/memory.hpp"
#include "websocketpp/common/thread.hpp"
#include "websocketpp/config/asio_client.hpp"

typedef websocketpp::client<websocketpp::config::asio_tls_client> client;

enum WsStatus {
  WsOpening = 0,
  WsOpened,
  WsFailed,
  WsClosed,
  WsReconnecting,
  WsServerClosed
};

class WsClient {
 public:
  WsClient(std::function<void(const std::string &)> on_receive_msg_cb,
           std::function<void(WsStatus)> on_ws_status_cb);

  ~WsClient();

 public:
  int Connect(std::string const &uri);

  void Close(websocketpp::close::status::value code =
                 websocketpp::close::status::going_away,
             std::string reason = "");

  void Send(std::string message);

  void Ping(websocketpp::connection_hdl hdl);

  WsStatus GetStatus();

  // Callback
  void OnOpen(client *c, websocketpp::connection_hdl hdl);

  void OnFail(client *c, websocketpp::connection_hdl hdl);

  void OnClose(client *c, websocketpp::connection_hdl hdl);

  bool OnPing(websocketpp::connection_hdl hdl, std::string msg);

  bool OnPong(websocketpp::connection_hdl hdl, std::string msg);

  void OnPongTimeout(websocketpp::connection_hdl hdl, std::string msg);

  void OnMessage(websocketpp::connection_hdl hdl, client::message_ptr msg);

 private:
  client m_endpoint_;
  websocketpp::connection_hdl connection_handle_;
  std::thread m_thread_;
  std::thread ping_thread_;
  std::atomic<bool> running_{false};
  std::mutex mtx_;
  unsigned int interval_ = 3;
  std::condition_variable cond_var_;
  bool heartbeat_started_ = false;

  WsStatus ws_status_ = WsStatus::WsClosed;
  int timeout_count_ = 0;
  std::string uri_;
  bool destructed_ = false;

  std::function<void(const std::string &)> on_receive_msg_ = nullptr;
  std::function<void(WsStatus)> on_ws_status_ = nullptr;

  std::shared_ptr<websocketpp::lib::asio::ssl::context> on_tls_init(
      websocketpp::connection_hdl);
};

#endif