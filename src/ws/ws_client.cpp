#include "ws_client.h"

#include <cstdlib>
#include <iostream>
#include <sstream>

#include "log.h"

WsClient::WsClient(std::function<void(const std::string &)> on_receive_msg_cb,
                   std::function<void(WsStatus)> on_ws_status_cb)
    : on_receive_msg_(on_receive_msg_cb), on_ws_status_(on_ws_status_cb) {
  m_endpoint_.clear_access_channels(websocketpp::log::alevel::all);
  m_endpoint_.clear_error_channels(websocketpp::log::elevel::all);

  m_endpoint_.init_asio();
  m_endpoint_.start_perpetual();

  m_thread_ = std::thread(&client::run, &m_endpoint_);
}

WsClient::~WsClient() {
  destructed_ = true;
  running_ = false;

  cond_var_.notify_one();
  if (ping_thread_.joinable()) {
    ping_thread_.join();
    heartbeat_started_ = false;
  }

  m_endpoint_.stop_perpetual();

  if (m_thread_.joinable()) {
    m_thread_.join();
  }
}

int WsClient::Connect(std::string const &uri) {
  uri_ = uri;

  websocketpp::lib::error_code ec;

  client::connection_ptr con = m_endpoint_.get_connection(uri, ec);

  if (!con) {
    LOG_ERROR("Invalid connection, uri [{}], error msg [{}]", uri,
              ec.message());
    return -1;
  }

  connection_handle_ = con->get_handle();

  if (ec) {
    LOG_ERROR("Connect initialization error: {}", ec.message());
    return -1;
  }

  con->set_tls_init_handler([this](websocketpp::connection_hdl hdl) {
    return this->on_tls_init(hdl);
  });

  con->set_open_handler(
      websocketpp::lib::bind(&WsClient::OnOpen, this, &m_endpoint_,
                             websocketpp::lib::placeholders::_1));
  con->set_fail_handler(
      websocketpp::lib::bind(&WsClient::OnFail, this, &m_endpoint_,
                             websocketpp::lib::placeholders::_1));
  con->set_close_handler(
      websocketpp::lib::bind(&WsClient::OnClose, this, &m_endpoint_,
                             websocketpp::lib::placeholders::_1));

  con->set_ping_handler(websocketpp::lib::bind(
      &WsClient::OnPing, this, websocketpp::lib::placeholders::_1,
      websocketpp::lib::placeholders::_2));

  con->set_pong_handler(websocketpp::lib::bind(
      &WsClient::OnPong, this, websocketpp::lib::placeholders::_1,
      websocketpp::lib::placeholders::_2));

  con->set_pong_timeout(1000);

  con->set_pong_timeout_handler(websocketpp::lib::bind(
      &WsClient::OnPongTimeout, this, websocketpp::lib::placeholders::_1,
      websocketpp::lib::placeholders::_2));

  con->set_message_handler(websocketpp::lib::bind(
      &WsClient::OnMessage, this, websocketpp::lib::placeholders::_1,
      websocketpp::lib::placeholders::_2));

  m_endpoint_.connect(con);

  ws_status_ = WsStatus::WsOpening;
  on_ws_status_(WsStatus::WsOpening);

  return 0;
}

std::shared_ptr<websocketpp::lib::asio::ssl::context> WsClient::on_tls_init(
    websocketpp::connection_hdl) {
  namespace asio = websocketpp::lib::asio;
  auto ctx =
      std::make_shared<asio::ssl::context>(asio::ssl::context::tlsv12_client);

  try {
    ctx->set_options(
        asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
        asio::ssl::context::no_sslv3 | asio::ssl::context::single_dh_use);
    // ctx->load_verify_file("crossdesk.cn_bundle.crt");

    ctx->set_verify_mode(websocketpp::lib::asio::ssl::verify_none);
  } catch (std::exception &e) {
    LOG_ERROR("TLS init error: {}", e.what());
  }
  return ctx;
}

void WsClient::Close(websocketpp::close::status::value code,
                     std::string reason) {
  websocketpp::lib::error_code ec;
  running_ = false;
  m_endpoint_.close(connection_handle_, code, reason, ec);
  if (ec) {
    LOG_ERROR("Initiating close error: {}", ec.message());
  }
}

void WsClient::Send(std::string message) {
  websocketpp::lib::error_code ec;

  m_endpoint_.send(connection_handle_, message,
                   websocketpp::frame::opcode::text, ec);
  if (ec) {
    LOG_ERROR("Sending message error: {}", ec.message());
    return;
  }
}

void WsClient::Ping(websocketpp::connection_hdl hdl) {
  while (running_) {
    {
      std::unique_lock<std::mutex> lock(mtx_);
      cond_var_.wait_for(lock, std::chrono::seconds(interval_),
                         [this] { return !running_; });
    }

    if (!running_) {
      break;
    }

    if (hdl.expired()) {
      LOG_WARN("Websocket connection expired, reconnecting...");
    } else {
      auto con = m_endpoint_.get_con_from_hdl(hdl);
      if (con && con->get_state() == websocketpp::session::state::open) {
        websocketpp::lib::error_code ec;
        m_endpoint_.ping(hdl, "", ec);
        if (ec) {
          LOG_ERROR("Ping error: {}", ec.message());
          break;
        }
      }
    }
  }
}

WsStatus WsClient::GetStatus() { return ws_status_; }

void WsClient::OnOpen([[maybe_unused]] client *c,
                      websocketpp::connection_hdl hdl) {
  ws_status_ = WsStatus::WsOpened;
  on_ws_status_(WsStatus::WsOpened);

  if (!heartbeat_started_) {
    heartbeat_started_ = true;
    running_ = true;
    ping_thread_ = std::thread(&WsClient::Ping, this, hdl);
  } else {
    running_ = false;
    cond_var_.notify_one();
    if (ping_thread_.joinable()) {
      ping_thread_.join();
      running_ = true;
      ping_thread_ = std::thread(&WsClient::Ping, this, hdl);
    }
  }
}

void WsClient::OnFail([[maybe_unused]] client *c,
                      websocketpp::connection_hdl hdl) {
  ws_status_ = WsStatus::WsFailed;
  on_ws_status_(WsStatus::WsFailed);
  Connect(uri_);
}

void WsClient::OnClose([[maybe_unused]] client *c,
                       websocketpp::connection_hdl hdl) {
  ws_status_ = WsStatus::WsServerClosed;
  on_ws_status_(WsStatus::WsServerClosed);

  if (running_) {
    // try to reconnect
    Connect(uri_);
  }
}

bool WsClient::OnPing(websocketpp::connection_hdl hdl, std::string msg) {
  return true;
}

bool WsClient::OnPong(websocketpp::connection_hdl hdl, std::string msg) {
  return true;
}

void WsClient::OnPongTimeout(websocketpp::connection_hdl hdl, std::string msg) {
  if (timeout_count_ < 2) {
    timeout_count_++;
    return;
  }

  LOG_WARN("Pong timeout, reset connection");
  // m_endpoint_.close(hdl, websocketpp::close::status::normal,
  // "OnPongTimeout");
  ws_status_ = WsStatus::WsReconnecting;
  on_ws_status_(WsStatus::WsReconnecting);
  m_endpoint_.reset();
}

void WsClient::OnMessage(websocketpp::connection_hdl, client::message_ptr msg) {
  on_receive_msg_(msg->get_payload());
}