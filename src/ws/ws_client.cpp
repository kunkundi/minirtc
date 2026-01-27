#include "ws_client.h"

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "log.h"

namespace minirtc {

std::string ComputeFingerprint(X509 *cert) {
  unsigned int n = 0;
  unsigned char md[EVP_MAX_MD_SIZE];
  if (X509_digest(cert, EVP_sha256(), md, &n) != 1) {
    return "";
  }

  std::ostringstream oss;
  for (unsigned int i = 0; i < n; i++) {
    if (i) oss << ":";
    oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
        << (int)md[i];
  }
  return oss.str();
}

WsClient::WsClient(std::function<void(const std::string &)> on_receive_msg_cb,
                   std::function<void(WsStatus)> on_ws_status_cb)
    : on_receive_msg_(on_receive_msg_cb), on_ws_status_(on_ws_status_cb) {}

WsClient::~WsClient() {
  destructed_ = true;
  Shutdown();
}

void WsClient::Shutdown() {
  running_ = false;
  cond_var_.notify_all();

  if (ping_thread_.joinable()) {
    ping_thread_.join();
  }
  if (reconnect_thread_.joinable()) {
    reconnect_thread_.join();
  }

  if (m_endpoint_) {
    m_endpoint_->stop_perpetual();
  }
  if (m_thread_.joinable()) {
    m_thread_.join();
  }

  m_endpoint_.reset();
  heartbeat_started_ = false;
}

void WsClient::StopThreads() {
  running_ = false;
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

  m_endpoint_.reset();
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

int WsClient::Connect(
    const std::string &uri, const std::string &expected_fingerprint,
    std::function<void(const std::string &)> on_fingerprint_cb) {
  uri_ = uri;
  expected_fingerprint_ = expected_fingerprint;
  on_fingerprint_cb_ = on_fingerprint_cb;

  StopThreads();

  m_endpoint_ = std::make_unique<client>();
  SetStatus(WsOpening);
  m_endpoint_->init_asio();
  m_endpoint_->start_perpetual();

  RegisterHandlers();
  m_thread_ =
      std::thread([endpoint = m_endpoint_.get()]() { endpoint->run(); });

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

  return Connect(uri_, expected_fingerprint_, on_fingerprint_cb_);
}

void WsClient::AsyncReConnect() {
  if (destructed_) {
    return;
  }

  // Ensure only one reconnect attempt is scheduled at a time.
  bool expected = false;
  if (!is_reconnecting_.compare_exchange_strong(expected, true)) {
    return;
  }

  if (reconnect_thread_.joinable()) {
    if (reconnect_thread_.get_id() != std::this_thread::get_id()) {
      reconnect_thread_.join();
    } else {
      reconnect_thread_.detach();
    }
  }

  // Exponential backoff: 0s, 1s, 2s, 4s, 8s ... capped at 60s
  int attempt = reconnect_attempts_.fetch_add(1) + 1;
  int exponent = std::min(attempt - 1, 6);  // 2^6 = 64s
  int delay_seconds = (attempt <= 1) ? 0 : (1 << exponent);
  delay_seconds = std::min(delay_seconds, 60);

  LOG_INFO("Will retry connection after {} seconds (attempt {})", delay_seconds,
           attempt);

  std::weak_ptr<WsClient> weak_self = shared_from_this();
  reconnect_thread_ = std::thread([weak_self, delay_seconds]() {
    if (delay_seconds > 0) {
      std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
    }

    if (auto self = weak_self.lock()) {
      if (self->destructed_) {
        self->is_reconnecting_.store(false);
        return;
      }

      self->ReConnect();
      self->is_reconnecting_.store(false);
    }
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
    ctx->set_verify_mode(asio::ssl::verify_peer |
                         asio::ssl::verify_fail_if_no_peer_cert);

    std::weak_ptr<WsClient> weak_self = shared_from_this();
    ctx->set_verify_callback(
        [weak_self](bool preverified, asio::ssl::verify_context &ctx) {
          if (auto self = weak_self.lock()) {
            return self->OnTlsVerify(preverified, ctx);
          }
          return false;
        });
  } catch (std::exception &e) {
    LOG_ERROR("TLS init error: {}", e.what());
  }
  return ctx;
}

bool WsClient::OnTlsVerify(bool preverified,
                           websocketpp::lib::asio::ssl::verify_context &ctx) {
  X509_STORE_CTX *cts = ctx.native_handle();
  X509 *cert = X509_STORE_CTX_get_current_cert(cts);
  if (cert) {
    int depth = X509_STORE_CTX_get_error_depth(cts);

    // only verify the first certificate
    if (depth == 0) {
      std::string fingerprint = ComputeFingerprint(cert);

      if (fingerprint.empty()) {
        LOG_ERROR("Failed to compute certificate fingerprint");
        tls_failure_count_++;
        return false;
      }

      if (expected_fingerprint_.empty()) {
        LOG_INFO("First connection: saving certificate fingerprint");
        if (on_fingerprint_cb_) {
          on_fingerprint_cb_(fingerprint);
        }
        tls_failure_count_ = 0;
        return true;
      }

      if (fingerprint == expected_fingerprint_) {
        char subject[256];
        X509_NAME_oneline(X509_get_subject_name(cert), subject,
                          sizeof(subject));
        // LOG_INFO(
        //     "TLS certificate fingerprint verified successfully. Subject: {}",
        //     subject);
        tls_failure_count_ = 0;
        return true;
      } else {
        LOG_ERROR("Certificate fingerprint mismatch");
        tls_failure_count_++;
        return false;
      }
    }
  }

  return true;
}

void WsClient::OnOpen(client *, websocketpp::connection_hdl hdl) {
  LOG_INFO("WebSocket connection opened");
  connection_handle_ = hdl;
  SetStatus(WsOpened);
  reconnect_attempts_ = 0;
  tls_failure_count_ = 0;
  RestartPingThread(hdl);
}

void WsClient::OnFail(client *c, websocketpp::connection_hdl hdl) {
  auto con = c->get_con_from_hdl(hdl);
  std::string error_msg = con ? con->get_ec().message() : "unknown";
  websocketpp::lib::error_code ec =
      con ? con->get_ec() : websocketpp::lib::error_code();

  bool is_tls_error = false;
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

    int attempts = reconnect_attempts_.fetch_add(1);
    int delay_seconds = std::min(1 << attempts, 60);  // 1, 2, 4, 8, 16, 32, 60

    LOG_INFO("Will retry TLS connection after {} seconds (attempt {})",
             delay_seconds, attempts + 1);

    if (!destructed_) {
      if (reconnect_thread_.joinable()) {
        reconnect_thread_.join();
      }

      std::weak_ptr<WsClient> weak_self = shared_from_this();
      reconnect_thread_ = std::thread([weak_self, delay_seconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(delay_seconds));
        if (auto self = weak_self.lock()) {
          if (!self->destructed_) {
            self->ReConnect();
          }
        }
      });
    }
  } else {
    LOG_WARN("Connection failed (non-TLS error): {}", error_msg);
    if (!destructed_) {
      AsyncReConnect();
    }
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
}  // namespace minirtc