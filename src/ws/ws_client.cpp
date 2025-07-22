#include "ws_client.h"

#include <chrono>
#include <iostream>
#include <thread>

#include "log.h"

namespace minirtc {

WsClient::WsClient(std::function<void(const std::string &)> on_receive_msg_cb,
                   std::function<void(WsStatus)> on_ws_status_cb)
    : on_receive_msg_(on_receive_msg_cb), on_ws_status_(on_ws_status_cb) {}

WsClient::~WsClient() {
  destructed_ = true;
  Shutdown();
}

void WsClient::Shutdown() {
  Close();

  if (is_reconnecting_ && reconnect_thread_.joinable()) {
    reconnect_thread_.join();
  }

  StopThreads();
}

void WsClient::StopThreads() {
  if (!running_.exchange(false)) {
    return;
  }

  cond_var_.notify_all();

  if (ping_thread_.joinable()) {
    ping_thread_.join();
  }

  if (m_endpoint_) {
    m_endpoint_->stop_perpetual();
  }

  if (m_thread_.joinable()) {
    m_thread_.join();
  }

  delete m_endpoint_;
  m_endpoint_ = nullptr;

  heartbeat_started_ = false;
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
  m_endpoint_->set_open_handler(
      [weak_self, endpoint = m_endpoint_](websocketpp::connection_hdl hdl) {
        if (auto self = weak_self.lock()) {
          self->OnOpen(endpoint, hdl);
        }
      });
  m_endpoint_->set_fail_handler(
      [weak_self, endpoint = m_endpoint_](websocketpp::connection_hdl hdl) {
        if (auto self = weak_self.lock()) {
          self->OnFail(endpoint, hdl);
        }
      });
  m_endpoint_->set_close_handler(
      [weak_self, endpoint = m_endpoint_](websocketpp::connection_hdl hdl) {
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
  m_endpoint_->set_pong_timeout_handler(
      [weak_self](websocketpp::connection_hdl hdl, std::string msg) {
        if (auto self = weak_self.lock()) {
          self->OnPongTimeout(hdl, msg);
        }
      });
  m_endpoint_->set_message_handler(
      [weak_self](websocketpp::connection_hdl hdl, client::message_ptr msg) {
        if (auto self = weak_self.lock()) {
          self->OnMessage(hdl, msg);
        }
      });
}

int WsClient::Connect(const std::string &uri, const std::string &cert_path) {
  uri_ = uri;
  cert_path_ = cert_path;

  StopThreads();

  m_endpoint_ = new client();
  SetStatus(WsOpening);
  m_endpoint_->init_asio();
  m_endpoint_->start_perpetual();

  RegisterHandlers();
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
  if (ws_status_ == WsReconnecting) {
    LOG_INFO("Already reconnecting, ignore duplicate call.");
    return 0;
  }

  LOG_INFO("Reconnecting WebSocket...");
  SetStatus(WsReconnecting);

  websocketpp::lib::error_code ec;
  if (!connection_handle_.expired()) {
    auto con = m_endpoint_->get_con_from_hdl(connection_handle_, ec);
    if (!ec && con && con->get_state() == websocketpp::session::state::open) {
      m_endpoint_->close(connection_handle_,
                         websocketpp::close::status::going_away, "Reconnect",
                         ec);
    }
  }

  StopThreads();

  return Connect(uri_, cert_path_);
}

void WsClient::AsyncReConnect() {
  if (destructed_ || is_reconnecting_.exchange(true)) {
    return;
  }

  std::shared_ptr<WsClient> self = shared_from_this();
  reconnect_thread_ = std::thread([self]() {
    if (self->destructed_) {
      return;
    }
    self->ReConnect();
  });
}

void WsClient::Close() {
  if (connection_handle_.expired()) {
    return;
  }

  websocketpp::lib::error_code ec;
  auto con = m_endpoint_->get_con_from_hdl(connection_handle_, ec);
  if (!ec && con && con->get_state() == websocketpp::session::state::open) {
    m_endpoint_->close(connection_handle_, websocketpp::close::status::normal,
                       "Client requested close", ec);
  }
}

void WsClient::Send(const std::string &message) {
  websocketpp::lib::error_code ec;
  auto con = m_endpoint_->get_con_from_hdl(connection_handle_, ec);
  if (ec || con->get_state() != websocketpp::session::state::open) {
    LOG_WARN("Send failed: not connected or error: {}", ec.message());
    return;
  }
  m_endpoint_->send(connection_handle_, message,
                    websocketpp::frame::opcode::text, ec);
  if (ec) {
    LOG_ERROR("Sending message error: {}, [{}]", ec.message(), message);
  }
}

WsStatus WsClient::GetStatus() { return ws_status_; }

void WsClient::SetStatus(WsStatus status) {
  ws_status_ = status;
  if (on_ws_status_) {
    on_ws_status_(status);
  }
}

void WsClient::RestartPingThread(websocketpp::connection_hdl hdl) {
  if (ping_thread_.joinable()) {
    running_ = false;
    cond_var_.notify_all();
    ping_thread_.join();
  }

  running_ = true;
  ping_thread_ = std::thread(&WsClient::PingLoop, this, hdl);
  heartbeat_started_ = true;
}

void WsClient::PingLoop(websocketpp::connection_hdl hdl) {
  while (running_) {
    std::unique_lock<std::mutex> lock(ping_mtx_);
    cond_var_.wait_for(lock, std::chrono::seconds(ping_interval_seconds_),
                       [this] { return !running_; });
    if (!running_) break;

    if (hdl.expired()) {
      LOG_WARN("Websocket connection expired, cannot ping");
      break;
    }
    auto con = m_endpoint_->get_con_from_hdl(hdl);
    if (con && con->get_state() == websocketpp::session::state::open) {
      websocketpp::lib::error_code ec;
      m_endpoint_->ping(hdl, "", ec);
      if (ec) {
        LOG_ERROR("Ping error: {}", ec.message());
        break;
      }
    }
  }
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
    ctx->set_verify_mode(asio::ssl::verify_peer);
    if (!cert_path_.empty()) {
      ctx->load_verify_file(cert_path_);
    }
  } catch (std::exception &e) {
    LOG_ERROR("TLS init error: {}", e.what());
  }
  return ctx;
}

void WsClient::OnOpen(client *, websocketpp::connection_hdl hdl) {
  LOG_INFO("WebSocket connection opened");
  connection_handle_ = hdl;
  SetStatus(WsOpened);
  RestartPingThread(hdl);
}

void WsClient::OnFail(client *c, websocketpp::connection_hdl hdl) {
  auto con = c->get_con_from_hdl(hdl);
  LOG_WARN("Connection failed: {}", con ? con->get_ec().message() : "unknown");
  if (!destructed_) {
    AsyncReConnect();
  }
}

void WsClient::OnClose(client *c, websocketpp::connection_hdl hdl) {
  auto con = c->get_con_from_hdl(hdl);
  LOG_WARN("Connection closed");
  if (!destructed_) {
    AsyncReConnect();
  }
}

bool WsClient::OnPing(websocketpp::connection_hdl, std::string) { return true; }

bool WsClient::OnPong(websocketpp::connection_hdl, std::string) {
  timeout_count_ = 0;
  return true;
}

void WsClient::OnPongTimeout(websocketpp::connection_hdl, std::string) {
  if (++timeout_count_ >= 2) {
    LOG_WARN("Pong timeout exceeded, reconnecting...");
    AsyncReConnect();
  }
}

void WsClient::OnMessage(websocketpp::connection_hdl, client::message_ptr msg) {
  if (on_receive_msg_) {
    on_receive_msg_(msg->get_payload());
  }
}
}