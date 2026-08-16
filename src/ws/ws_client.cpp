#include "ws_client.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#include <wincrypt.h>
#include <windows.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

#include "log.h"

namespace minirtc {

#ifdef _WIN32
namespace {
struct WindowsRootStoreLocation {
  DWORD flag;
  const char* name;
};

bool ShouldImportWindowsCertificateAsRoot(X509* x509,
                                          bool require_self_signed_ca) {
  if (!require_self_signed_ca) {
    return true;
  }

  return X509_check_issued(x509, x509) == X509_V_OK &&
         X509_check_ca(x509) > 0;
}

int LoadWindowsCertificateStore(X509_STORE* store, DWORD location_flag,
                                const wchar_t* store_name,
                                const char* location_name,
                                bool require_self_signed_ca) {
  HCERTSTORE sys_store =
      CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                    location_flag | CERT_STORE_READONLY_FLAG, store_name);
  if (!sys_store) {
    LOG_WARN("Failed to open Windows {} certificate store", location_name);
    return 0;
  }

  int imported_count = 0;
  PCCERT_CONTEXT cert_ctx = nullptr;
  while ((cert_ctx = CertEnumCertificatesInStore(sys_store, cert_ctx)) !=
         nullptr) {
    const unsigned char* cert_data = cert_ctx->pbCertEncoded;
    X509* x509 = d2i_X509(
        nullptr, &cert_data, static_cast<long>(cert_ctx->cbCertEncoded));
    if (!x509) {
      ERR_clear_error();
      continue;
    }

    if (!ShouldImportWindowsCertificateAsRoot(x509,
                                              require_self_signed_ca)) {
      X509_free(x509);
      continue;
    }

    if (X509_STORE_add_cert(store, x509) == 1) {
      ++imported_count;
    } else {
      ERR_clear_error();
    }
    X509_free(x509);
  }

  CertCloseStore(sys_store, 0);
  LOG_INFO("Loaded {} Windows certificates from {}", imported_count,
           location_name);
  return imported_count;
}

bool LoadWindowsRootCertificates(SSL_CTX* ssl_ctx) {
  if (!ssl_ctx) {
    return false;
  }

  X509_STORE* store = SSL_CTX_get_cert_store(ssl_ctx);
  if (!store) {
    LOG_WARN("Failed to get OpenSSL X509_STORE for Windows certificates");
    return false;
  }

  const WindowsRootStoreLocation locations[] = {
      {CERT_SYSTEM_STORE_CURRENT_USER, "CurrentUser"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE, "LocalMachine"},
      {CERT_SYSTEM_STORE_CURRENT_USER_GROUP_POLICY,
       "CurrentUserGroupPolicy"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY,
       "LocalMachineGroupPolicy"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE_ENTERPRISE,
       "LocalMachineEnterprise"},
  };

  int total_count = 0;
  for (const auto& location : locations) {
    total_count += LoadWindowsCertificateStore(store, location.flag, L"ROOT",
                                               location.name, false);
  }

  const WindowsRootStoreLocation compatibility_locations[] = {
      {CERT_SYSTEM_STORE_CURRENT_USER, "CurrentUserIntermediate"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE, "LocalMachineIntermediate"},
  };

  for (const auto& location : compatibility_locations) {
    total_count += LoadWindowsCertificateStore(store, location.flag, L"CA",
                                               location.name, true);
  }

  return total_count > 0;
}
}  // namespace
#endif

#ifdef __APPLE__
namespace {
int AddMacCertificateToOpenSslStore(X509_STORE* store, SecCertificateRef cert) {
  if (!store || !cert) {
    return 0;
  }

  CFDataRef cert_data = SecCertificateCopyData(cert);
  if (!cert_data) {
    return 0;
  }

  int imported_count = 0;
  const unsigned char* data =
      reinterpret_cast<const unsigned char*>(CFDataGetBytePtr(cert_data));
  long data_len = static_cast<long>(CFDataGetLength(cert_data));
  if (data && data_len > 0) {
    const unsigned char* cursor = data;
    X509* x509 = d2i_X509(nullptr, &cursor, data_len);
    if (x509) {
      if (X509_STORE_add_cert(store, x509) == 1) {
        imported_count = 1;
      } else {
        ERR_clear_error();
      }
      X509_free(x509);
    } else {
      ERR_clear_error();
    }
  }

  CFRelease(cert_data);
  return imported_count;
}

bool MacTrustSettingsAllowRoot(SecCertificateRef cert,
                               SecTrustSettingsDomain domain) {
  CFArrayRef trust_settings = nullptr;
  OSStatus status =
      SecTrustSettingsCopyTrustSettings(cert, domain, &trust_settings);
  if (status != errSecSuccess || trust_settings == nullptr) {
    return false;
  }

  bool allow_root = false;
  CFIndex settings_count = CFArrayGetCount(trust_settings);
  if (settings_count == 0) {
    allow_root = true;
  }

  for (CFIndex i = 0; i < settings_count; ++i) {
    CFTypeRef setting = CFArrayGetValueAtIndex(trust_settings, i);
    if (!setting || CFGetTypeID(setting) != CFDictionaryGetTypeID()) {
      continue;
    }

    auto setting_dict =
        reinterpret_cast<CFDictionaryRef>(const_cast<void*>(setting));
    SecTrustSettingsResult result = kSecTrustSettingsResultTrustRoot;
    CFTypeRef result_value =
        CFDictionaryGetValue(setting_dict, kSecTrustSettingsResult);
    if (result_value) {
      if (CFGetTypeID(result_value) != CFNumberGetTypeID()) {
        continue;
      }

      SInt32 raw_result = kSecTrustSettingsResultInvalid;
      if (!CFNumberGetValue(reinterpret_cast<CFNumberRef>(
                                const_cast<void*>(result_value)),
                            kCFNumberSInt32Type, &raw_result)) {
        continue;
      }
      result = static_cast<SecTrustSettingsResult>(raw_result);
    }

    if (result == kSecTrustSettingsResultTrustRoot ||
        result == kSecTrustSettingsResultTrustAsRoot) {
      allow_root = true;
      break;
    }
  }

  CFRelease(trust_settings);
  return allow_root;
}

int LoadMacCertificatesFromArray(X509_STORE* store, CFArrayRef certs) {
  if (!store || !certs) {
    return 0;
  }

  int imported_count = 0;
  CFIndex cert_count = CFArrayGetCount(certs);
  for (CFIndex i = 0; i < cert_count; ++i) {
    auto cert = reinterpret_cast<SecCertificateRef>(
        const_cast<void*>(CFArrayGetValueAtIndex(certs, i)));
    imported_count += AddMacCertificateToOpenSslStore(store, cert);
  }
  return imported_count;
}

int LoadMacAnchorCertificates(X509_STORE* store) {
  CFArrayRef certs = nullptr;
  OSStatus status = SecTrustCopyAnchorCertificates(&certs);
  if (status != errSecSuccess || certs == nullptr) {
    LOG_WARN("SecTrustCopyAnchorCertificates failed: {}",
             static_cast<int>(status));
    return 0;
  }

  int imported_count = LoadMacCertificatesFromArray(store, certs);
  CFRelease(certs);

  LOG_INFO("Loaded {} anchor certificates from macOS default anchors",
           imported_count);
  return imported_count;
}

int LoadMacTrustSettingsCertificates(X509_STORE* store,
                                     SecTrustSettingsDomain domain,
                                     const char* domain_name) {
  CFArrayRef certs = nullptr;
  OSStatus status = SecTrustSettingsCopyCertificates(domain, &certs);
  if (status != errSecSuccess || certs == nullptr) {
    if (status != errSecNoTrustSettings) {
      LOG_WARN("SecTrustSettingsCopyCertificates({}) failed: {}",
               domain_name, static_cast<int>(status));
    }
    return 0;
  }

  int imported_count = 0;
  CFIndex cert_count = CFArrayGetCount(certs);
  for (CFIndex i = 0; i < cert_count; ++i) {
    auto cert = reinterpret_cast<SecCertificateRef>(
        const_cast<void*>(CFArrayGetValueAtIndex(certs, i)));
    if (MacTrustSettingsAllowRoot(cert, domain)) {
      imported_count += AddMacCertificateToOpenSslStore(store, cert);
    }
  }

  CFRelease(certs);
  LOG_INFO("Loaded {} trusted root certificates from macOS {} trust settings",
           imported_count, domain_name);
  return imported_count;
}

bool LoadMacSystemAnchorCertificates(SSL_CTX* ssl_ctx) {
  if (!ssl_ctx) {
    return false;
  }

  X509_STORE* store = SSL_CTX_get_cert_store(ssl_ctx);
  if (!store) {
    LOG_WARN("Failed to get OpenSSL X509_STORE for macOS system certificates");
    return false;
  }

  int total_count = LoadMacAnchorCertificates(store);
  total_count += LoadMacTrustSettingsCertificates(
      store, kSecTrustSettingsDomainAdmin, "Admin");
  total_count += LoadMacTrustSettingsCertificates(
      store, kSecTrustSettingsDomainUser, "User");

  LOG_INFO("Loaded {} certificates from macOS trust stores", total_count);
  return total_count > 0;
}
}  // namespace
#endif

WsClient::WsClient(std::function<void(const std::string&)> on_receive_msg_cb,
                   std::function<void(WsStatus)> on_ws_status_cb)
    : on_receive_msg_(on_receive_msg_cb), on_ws_status_(on_ws_status_cb) {}

namespace {
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
  std::lock_guard<std::mutex> shutdown_lock(shutdown_mtx_);

  // Shutdown is terminal. In particular, wake and invalidate a delayed
  // reconnect before waiting for any worker so an obsolete client cannot be
  // revived after its PeerConnection has been destroyed.
  shutdown_ = true;
  running_ = false;
  is_reconnecting_ = false;
  cond_var_.notify_all();
  reconnect_generation_.fetch_add(1);
  reconnect_cv_.notify_all();

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

  // A reconnect that was already running may have restarted the heartbeat.
  running_ = false;
  cond_var_.notify_all();
  JoinThread(ping_thread_);

  if (m_endpoint_) {
    m_endpoint_->stop_perpetual();
    m_endpoint_->stop();
  }
  JoinThread(m_thread_);

  m_endpoint_.reset();
  heartbeat_started_ = false;
}

void WsClient::StopThreads() {
  running_ = false;
  cond_var_.notify_all();

  JoinThread(ping_thread_);

  CleanupPendingThreads();

  if (m_endpoint_) {
    m_endpoint_->stop_perpetual();
  }
  JoinThread(m_thread_);

  m_endpoint_.reset();
  heartbeat_started_ = false;
}

void WsClient::CleanupPendingThreads() {
  // Increment generation to signal all pending threads to exit.
  reconnect_generation_.fetch_add(1);
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

void WsClient::ScheduleReconnect(int delay_seconds) {
  if (shutdown_) {
    return;
  }

  // ensure only one reconnect attempt is scheduled at a time.
  bool expected = false;
  if (!is_reconnecting_.compare_exchange_strong(expected, true)) {
    LOG_INFO("Reconnect already in progress, skipping.");
    return;
  }

  LOG_INFO("Will retry connection after {} seconds", delay_seconds);

  // capture current generation to detect cancellation
  uint64_t current_generation = reconnect_generation_.load();

  std::weak_ptr<WsClient> weak_self = shared_from_this();
  std::thread reconnect_thread([weak_self, delay_seconds,
                                current_generation]() {
    if (auto self = weak_self.lock()) {
      if (delay_seconds > 0) {
        // use interruptible wait instead of sleep
        std::unique_lock<std::mutex> lock(self->reconnect_mtx_);
        self->reconnect_cv_.wait_for(
            lock, std::chrono::seconds(delay_seconds),
            [&self, current_generation]() {
              return self->shutdown_.load() ||
                     self->reconnect_generation_.load() != current_generation;
            });
      }

      // Generation changed or shutdown started; cancel this reconnect attempt.
      if (self->shutdown_ ||
          self->reconnect_generation_.load() != current_generation) {
        self->is_reconnecting_.store(false);
        return;
      }

      self->ReConnect();
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

int WsClient::Connect(const std::string& uri) {
  if (shutdown_) {
    return -1;
  }

  uri_ = uri;

  LOG_INFO("Connecting WebSocket: {}", uri);

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
  if (shutdown_) {
    return -1;
  }

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

  return Connect(uri_);
}

void WsClient::AsyncReConnect() {
  if (shutdown_) {
    return;
  }

  // Exponential backoff: 0s, 1s, 2s, 4s, 8s ... capped at 60s
  int attempt = reconnect_attempts_.fetch_add(1) + 1;
  int exponent = std::min(attempt - 1, 6);  // 2^6 = 64s
  int delay_seconds = (attempt <= 1) ? 0 : (1 << exponent);
  delay_seconds = std::min(delay_seconds, 60);

  LOG_INFO("AsyncReConnect: scheduling reconnect (attempt {})", attempt);
  ScheduleReconnect(delay_seconds);
}

void WsClient::Close() {
  Shutdown();
}

void WsClient::Send(const std::string& message) {
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
  if (!shutdown_ && on_ws_status_) {
    on_ws_status_(status);
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

    // Load system CA certificates (no need to bundle cert files with client)
#ifdef _WIN32
    // On Windows, OpenSSL's set_default_verify_paths() does NOT read the
    // Windows certificate store. We must manually load trusted root CAs
    // from the Windows system store into the OpenSSL X509_STORE.
    // Load from Current User, Local Machine, Group Policy, and Enterprise
    // ROOT stores. If the Windows import wizard auto-placed a self-signed CA
    // into Intermediate, accept only that narrow misplaced-root case.
    if (!LoadWindowsRootCertificates(ctx->native_handle())) {
      LOG_WARN("Unable to load Windows Root certificates");
    }
#else
    bool loaded_macos_anchors = false;
#ifdef __APPLE__
    loaded_macos_anchors =
        LoadMacSystemAnchorCertificates(ctx->native_handle());
    if (!loaded_macos_anchors) {
      LOG_WARN(
          "Failed to load certificates from macOS system trust store, fallback "
          "to OpenSSL default verify paths");
    }
#endif

    bool loaded_system_certs = false;
    try {
      ctx->set_default_verify_paths();
      loaded_system_certs = true;
    } catch (const std::exception& e) {
      LOG_WARN(
          "Failed to load system CA certificates from default verify paths: {}",
          e.what());
    }

#if defined(__linux__)
    const char* ca_bundle_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL/CentOS/Fedora
        "/etc/ssl/ca-bundle.pem",              // openSUSE/SLES
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/cert.pem"  // Arch, Alpine, etc.
    };

    bool loaded_linux_bundle = false;
    for (const char* path : ca_bundle_paths) {
      try {
        ctx->load_verify_file(path);
        LOG_INFO("Loaded Linux system CA bundle from {}", path);
        loaded_linux_bundle = true;
        break;
      } catch (const std::exception&) {
        // Ignore and try the next candidate path.
      }
    }

    if (!loaded_system_certs && !loaded_linux_bundle) {
      LOG_WARN(
          "Unable to load Linux system CA bundle from any known path; TLS "
          "verification may fail if no custom CA is provided");
    }
#endif  // defined(__linux__)
#endif  // _WIN32

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
  if (!preverified) {
    // Provide more details than the generic "TLS handshake failed".
    X509_STORE_CTX* store_ctx = ctx.native_handle();
    if (store_ctx) {
      int err = X509_STORE_CTX_get_error(store_ctx);
      int depth = X509_STORE_CTX_get_error_depth(store_ctx);
      const char* err_str = X509_verify_cert_error_string(err);
      LOG_ERROR("TLS certificate verify failed: {} (err={}, depth={})",
                (err_str ? err_str : "unknown"), err, depth);
      if (err == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN ||
          err == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
          err == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY ||
          err == X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE) {
        SetStatus(WsTlsCertError);
      }
    } else {
      LOG_ERROR("TLS certificate verify failed: no store_ctx");
    }
    tls_failure_count_++;
  } else {
    tls_failure_count_ = 0;
  }
  return preverified;
}

void WsClient::OnOpen(client*, websocketpp::connection_hdl hdl) {
  LOG_INFO("WebSocket connection opened");
  connection_handle_ = hdl;
  SetStatus(WsOpened);
  reconnect_attempts_ = 0;
  tls_failure_count_ = 0;
  RestartPingThread(hdl);
}

void WsClient::OnFail(client* c, websocketpp::connection_hdl hdl) {
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

    if (ws_status_ == WsTlsCertError) {
      LOG_WARN(
          "TLS certificate is not trusted. For self-hosted servers, install "
          "the self-signed root certificate into the trusted root store.");
      return;
    }

    int attempts = reconnect_attempts_.fetch_add(1);
    int delay_seconds = std::min(1 << attempts, 60);  // 1, 2, 4, 8, 16, 32, 60

    LOG_INFO("Will retry TLS connection after {} seconds (attempt {})",
             delay_seconds, attempts + 1);

    if (!shutdown_) {
      ScheduleReconnect(delay_seconds);
    }
  } else {
    LOG_WARN("Connection failed (non-TLS error): {}", error_msg);
    if (!shutdown_) {
      AsyncReConnect();
    }
  }
}

void WsClient::OnClose(client* c, websocketpp::connection_hdl hdl) {
  auto con = c->get_con_from_hdl(hdl);
  LOG_WARN("Connection closed");
  if (!shutdown_) {
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
