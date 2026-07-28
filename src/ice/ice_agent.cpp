#include "ice_agent.h"

#include <gio/gio.h>
#include <glib.h>

#include <algorithm>
#include <cassert>
#include <vector>

#include "log.h"

// #define SAVE_IO_STREAM

namespace minirtc {

namespace {

// libnice requires a numeric address in nice_agent_set_relay_info().  The
// application configuration normally uses the same hostname for signaling,
// STUN and TURN, so resolve it before registering relay servers.  Keep the
// address list bounded so candidate gathering does not fan out indefinitely.
std::vector<std::string> ResolveTurnServerAddresses(const std::string& server) {
  constexpr size_t kMaxAddresses = 4;
  std::vector<std::string> addresses;

  if (server.empty()) {
    return addresses;
  }

  GInetAddress* literal = g_inet_address_new_from_string(server.c_str());
  if (literal != nullptr) {
    addresses.push_back(server);
    g_object_unref(literal);
    return addresses;
  }

  GResolver* resolver = g_resolver_get_default();
  GError* error = nullptr;
  GList* resolved =
      g_resolver_lookup_by_name(resolver, server.c_str(), nullptr, &error);

  for (GList* item = resolved;
       item != nullptr && addresses.size() < kMaxAddresses; item = item->next) {
    auto* address = G_INET_ADDRESS(item->data);
    gchar* numeric_address = g_inet_address_to_string(address);
    if (numeric_address == nullptr) {
      continue;
    }

    if (std::find(addresses.begin(), addresses.end(), numeric_address) ==
        addresses.end()) {
      addresses.emplace_back(numeric_address);
    }
    g_free(numeric_address);
  }

  if (resolved != nullptr) {
    g_resolver_free_addresses(resolved);
  }
  g_object_unref(resolver);

  if (error != nullptr) {
    LOG_ERROR("Failed to resolve TURN server [{}]: {}", server, error->message);
    g_error_free(error);
  }

  return addresses;
}

bool AddTurnRelay(NiceAgent* agent, guint stream_id, const std::string& address,
                  guint port, const std::string& username,
                  const std::string& password, NiceRelayType type) {
  const gboolean accepted = nice_agent_set_relay_info(
      agent, stream_id, NICE_COMPONENT_TYPE_RTP, address.c_str(), port,
      username.c_str(), password.c_str(), type);
  const char* transport = type == NICE_RELAY_TYPE_TURN_UDP ? "UDP" : "TCP";

  if (accepted) {
    LOG_INFO("Registered TURN/{} relay [{}:{}]", transport, address, port);
    return true;
  }

  LOG_WARN("libnice rejected TURN/{} relay [{}:{}]", transport, address, port);
  return false;
}

const char* TurnModeName(TurnMode mode) {
  switch (mode) {
    case TurnMode::TurnDisabled:
      return "disabled";
    case TurnMode::TurnAutoUdpTcp:
      return "auto_udp_tcp";
    case TurnMode::TurnForceUdp:
      return "force_udp";
    case TurnMode::TurnForceTcp:
      return "force_tcp";
    default:
      return "invalid";
  }
}

bool IsTurnEnabled(TurnMode mode) { return mode != TurnMode::TurnDisabled; }

bool IsTurnForced(TurnMode mode) {
  return mode == TurnMode::TurnForceUdp || mode == TurnMode::TurnForceTcp;
}

bool UsesTurnUdp(TurnMode mode) {
  return mode == TurnMode::TurnAutoUdpTcp || mode == TurnMode::TurnForceUdp;
}

bool UsesTurnTcp(TurnMode mode) {
  return mode == TurnMode::TurnAutoUdpTcp || mode == TurnMode::TurnForceTcp;
}

}  // namespace

auto log_openssl_errors = []() {
  unsigned long e;
  while ((e = ERR_get_error()) != 0) {
    char buf[256];
    ERR_error_string_n(e, buf, sizeof(buf));
    LOG_ERROR("OpenSSL: {}", buf);
  }
};

static int DtlsVerifyCallback(X509_STORE_CTX* ctx, void* arg) {
  // always return true for self-signed certificate
  return 1;
}

IceAgent::IceAgent(bool offer_peer, bool use_trickle_ice, bool use_reliable_ice,
                   TurnMode turn_mode, bool enable_srtp, std::string& stun_ip,
                   uint16_t stun_port, std::string& turn_ip, uint16_t turn_port,
                   std::string& turn_username, std::string& turn_password)
    : stun_ip_(stun_ip),
      use_trickle_ice_(use_trickle_ice),
      use_reliable_ice_(use_reliable_ice),
      turn_mode_(turn_mode),
      enable_srtp_(enable_srtp),
      stun_port_(stun_port),
      turn_ip_(turn_ip),
      turn_port_(turn_port),
      turn_username_(turn_username),
      turn_password_(turn_password),
      controlling_(offer_peer) {}

IceAgent::~IceAgent() {
  if (!destroyed_) {
    DestroyIceAgent();
  }

  CleanupDtls();

  if (agent_) {
    g_object_unref(agent_);
  }
  g_free(ice_ufrag_);
  g_free(ice_password_);

#ifdef SAVE_IO_STREAM
  if (file_in_) {
    fflush(file_in_);
    fclose(file_in_);
    file_in_ = nullptr;
  }

  if (file_out_) {
    fflush(file_out_);
    fclose(file_out_);
    file_out_ = nullptr;
  }
#endif
}

int IceAgent::CreateIceAgent(nice_cb_state_changed_t on_state_changed,
                             nice_cb_new_candidate_t on_new_candidate,
                             nice_cb_gathering_done_t on_gathering_done,
                             nice_cb_new_selected_pair_t on_new_selected_pair,
                             nice_cb_recv_t on_recv,
                             nice_cb_dtls_done_t on_cb_dtls_done,
                             void* user_ptr) {
  if (nice_thread_.joinable() || nice_inited_) {
    LOG_ERROR("Nice agent has already been created");
    return -1;
  }

  {
    std::lock_guard<std::mutex> lk(init_mutex_);
    init_done_ = false;
    init_status_ = -1;
  }

  destroyed_ = false;
  nice_inited_ = false;
  init_failed_ = false;
  agent_closed_ = false;
  stream_id_ = 0;
  agent_ = nullptr;
  gloop_ = nullptr;

  on_state_changed_ = on_state_changed;
  on_new_selected_pair_ = on_new_selected_pair;
  on_new_candidate_ = on_new_candidate;
  on_gathering_done_ = on_gathering_done;
  on_recv_ = on_recv;
  on_cb_dtls_done_ = on_cb_dtls_done;
  user_ptr_ = user_ptr;

  g_networking_init();
  exit_nice_thread_ = false;

  nice_thread_ = std::thread([this]() {
    auto notify_init = [this](int status) {
      {
        std::lock_guard<std::mutex> lk(init_mutex_);
        init_status_ = status;
        init_done_ = true;
      }
      init_cv_.notify_one();
    };

    GMainLoop* loop = g_main_loop_new(nullptr, false);
    gloop_ = loop;
    if (loop == nullptr) {
      LOG_ERROR("Failed to create glib main loop");
      init_failed_ = true;
      notify_init(-1);
      exit_nice_thread_ = true;
      return;
    }

    NiceAgent* agent = nice_agent_new_full(
        g_main_loop_get_context(loop), NICE_COMPATIBILITY_RFC5245,
        (NiceAgentOption)(use_trickle_ice_
                              ? (NICE_AGENT_OPTION_ICE_TRICKLE |
                                 (use_reliable_ice_ ? NICE_AGENT_OPTION_RELIABLE
                                                    : NICE_AGENT_OPTION_NONE))
                              : (use_reliable_ice_ ? NICE_AGENT_OPTION_RELIABLE
                                                   : NICE_AGENT_OPTION_NONE)));
    agent_ = agent;

    LOG_INFO(
        "Nice agent init with [trickle ice|{}], [reliable mode|{}], "
        "[turn mode|{}]",
        use_trickle_ice_, use_reliable_ice_, TurnModeName(turn_mode_));

    if (agent == nullptr) {
      LOG_ERROR("Failed to create agent_");
      init_failed_ = true;
      g_main_loop_unref(loop);
      gloop_ = nullptr;
      notify_init(-1);
      exit_nice_thread_ = true;
      return;
    }

    g_object_set(agent, "stun-server", stun_ip_.c_str(), nullptr);
    g_object_set(agent, "stun-server-port", stun_port_, nullptr);
    g_object_set(agent, "controlling-mode", controlling_, nullptr);

    g_signal_connect(agent, "candidate-gathering-done",
                     G_CALLBACK(on_gathering_done_), user_ptr_);
    g_signal_connect(agent, "new-selected-pair",
                     G_CALLBACK(on_new_selected_pair_), user_ptr_);
    g_signal_connect(agent, "new-candidate", G_CALLBACK(on_new_candidate_),
                     user_ptr_);
    g_signal_connect(agent, "component-state-changed",
                     G_CALLBACK(&IceAgent::OnNiceStateChangedStatic), this);

    stream_id_ = nice_agent_add_stream(agent, n_components_);
    if (stream_id_ == 0) {
      LOG_ERROR("Failed to add stream");
      init_failed_ = true;
      g_object_unref(agent);
      agent_ = nullptr;
      g_main_loop_unref(loop);
      gloop_ = nullptr;
      notify_init(-1);
      exit_nice_thread_ = true;
      return;
    }

    if (has_video_stream_) {
      nice_agent_set_stream_name(agent, stream_id_, "video");
    }

    if (IsTurnEnabled(turn_mode_)) {
      const std::vector<std::string> relay_addresses =
          ResolveTurnServerAddresses(turn_ip_);
      bool relay_registered = false;

      for (const std::string& address : relay_addresses) {
        if (UsesTurnUdp(turn_mode_)) {
          relay_registered |= AddTurnRelay(
              agent, stream_id_, address, turn_port_, turn_username_,
              turn_password_, NICE_RELAY_TYPE_TURN_UDP);
        }
        if (UsesTurnTcp(turn_mode_)) {
          relay_registered |= AddTurnRelay(
              agent, stream_id_, address, turn_port_, turn_username_,
              turn_password_, NICE_RELAY_TYPE_TURN_TCP);
        }
      }

      if (!relay_registered) {
        LOG_ERROR(
            "TURN is enabled but no relay endpoint could be registered for "
            "[{}:{}]",
            turn_ip_, turn_port_);
        if (IsTurnForced(turn_mode_)) {
          init_failed_ = true;
          g_object_unref(agent);
          agent_ = nullptr;
          g_main_loop_unref(loop);
          gloop_ = nullptr;
          notify_init(-1);
          exit_nice_thread_ = true;
          return;
        }
      }
    }

    if (IsTurnForced(turn_mode_)) {
      g_object_set(agent, "force-relay", true, NULL);
    }

    nice_agent_attach_recv(agent, stream_id_, NICE_COMPONENT_TYPE_RTP,
                           g_main_loop_get_context(loop),
                           &IceAgent::OnNiceRecvStatic, this);

    nice_inited_ = true;
    init_failed_ = false;
    notify_init(0);
    g_main_loop_run(loop);
    exit_nice_thread_ = true;
  });

  {
    std::unique_lock<std::mutex> lk(init_mutex_);
    init_cv_.wait(lk, [this]() { return init_done_; });
  }

  if (init_status_ != 0 || init_failed_) {
    if (nice_thread_.joinable()) {
      nice_thread_.join();
    }
    LOG_ERROR("Nice agent initialization failed");
    return -1;
  }

#ifdef SAVE_IO_STREAM
  std::string in_file_name =
      "ice_in_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
      ".rtp";
  std::string out_file_name =
      "ice_out_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
      ".rtp";
  file_in_ = fopen(in_file_name.c_str(), "w+b");
  if (!file_in_) {
    LOG_WARN("Fail to open ice_in.rtp");
  }
  file_out_ = fopen(out_file_name.c_str(), "w+b");
  if (!file_out_) {
    LOG_WARN("Fail to open ice_out.rtp");
  }
#endif

  GenerateDtlsCertificate();
  // LOG_INFO("Generated DTLS fingerprint: {}", dtls_fingerprint_);

  LOG_INFO("Nice agent init finish");
  return 0;
}

void cb_closed(GObject* src, [[maybe_unused]] GAsyncResult* res,
               [[maybe_unused]] gpointer data) {
  [[maybe_unused]] NiceAgent* agent = NICE_AGENT(src);
  LOG_INFO("Nice agent closed");
}

int IceAgent::DestroyIceAgent() {
  if (destroyed_) {
    return 0;
  }

  destroyed_ = true;
  NiceAgent* agent = agent_.load();
  GMainLoop* loop = gloop_.load();

  if (nice_inited_ && agent != nullptr && stream_id_ != 0) {
    nice_agent_remove_stream(agent, stream_id_);
    nice_agent_close_async(agent, cb_closed, &agent_closed_);
  }

  if (loop != nullptr) {
    g_main_loop_quit(loop);
  }

  if (nice_thread_.joinable()) {
    nice_thread_.join();
  }

  if (loop != nullptr) {
    g_main_loop_unref(loop);
    gloop_ = nullptr;
  }

  {
    std::lock_guard<std::mutex> lk(init_mutex_);
    init_done_ = false;
    init_status_ = -1;
  }
  nice_inited_ = false;
  init_failed_ = false;
  stream_id_ = 0;

  CleanupDtls();

  LOG_INFO("Destroy nice agent success");
  return 0;
}

std::string IceAgent::GenerateLocalSdp() {
  if (!nice_inited_) {
    LOG_ERROR("Nice agent has not been initialized");
    return "";
  }

  if (nullptr == agent_) {
    LOG_ERROR("Nice agent is nullptr");
    return "";
  }

  if (destroyed_) {
    LOG_ERROR("Nice agent is destroyed");
    return "";
  }

  gchar* video_sdp_gstr = nice_agent_generate_local_sdp(agent_);
  video_stream_sdp_ = video_sdp_gstr;
  g_free(video_sdp_gstr);

  audio_stream_sdp_ = video_stream_sdp_;
  data_stream_sdp_ = video_stream_sdp_;
  local_sdp_ = video_stream_sdp_;

  if (has_audio_stream_) {
    std::string to_replace = "video";
    std::string replacement = "audio";
    size_t pos = 0;
    while ((pos = audio_stream_sdp_.find(to_replace, pos)) !=
           std::string::npos) {
      audio_stream_sdp_.replace(pos, to_replace.length(), replacement);
      pos += replacement.length();
    }
    local_sdp_ += audio_stream_sdp_;
  }

  if (has_data_stream_) {
    std::string to_replace = "video";
    std::string replacement = "data";
    size_t pos = 0;
    while ((pos = data_stream_sdp_.find(to_replace, pos)) !=
           std::string::npos) {
      data_stream_sdp_.replace(pos, to_replace.length(), replacement);
      pos += replacement.length();
    }
    local_sdp_ += data_stream_sdp_;
  }

  return local_sdp_;
}

std::string IceAgent::GetLocalStreamSdp(uint32_t stream_id) {
  if (!nice_inited_) {
    LOG_ERROR("Nice agent has not been initialized");
    return "";
  }

  if (nullptr == agent_) {
    LOG_ERROR("Nice agent is nullptr");
    return "";
  }

  if (destroyed_) {
    LOG_ERROR("Nice agent is destroyed");
    return "";
  }

  gchar* stream_sdp =
      nice_agent_generate_local_stream_sdp(agent_, stream_id, true);
  if (stream_sdp == nullptr) {
    LOG_ERROR("Failed to generate local stream sdp");
    return "";
  }
  local_sdp_ = stream_sdp;
  g_free(stream_sdp);
  return local_sdp_;
}

int IceAgent::SetRemoteSdp(const std::string& remote_sdp) {
  if (!nice_inited_) {
    LOG_ERROR("Nice agent has not been initialized");
    return -1;
  }

  if (nullptr == agent_) {
    LOG_ERROR("Nice agent is nullptr");
    return -1;
  }

  if (destroyed_) {
    LOG_ERROR("Nice agent is destroyed");
    return -1;
  }

  std::string sdp_no_fingerprint = ExtractAndStripFingerprint(remote_sdp);
  int ret = nice_agent_parse_remote_sdp(agent_, sdp_no_fingerprint.c_str());
  if (ret >= 0) {
    return 0;
  } else {
    LOG_ERROR("Failed to parse remote sdp: [{}]", sdp_no_fingerprint);
    return -1;
  }
}

int IceAgent::GatherCandidates() {
  if (!nice_inited_) {
    LOG_ERROR("Nice agent has not been initialized");
    return -1;
  }

  if (nullptr == agent_) {
    LOG_ERROR("Nice agent is nullptr");
    return -1;
  }

  if (destroyed_) {
    LOG_ERROR("Nice agent is destroyed");
    return -1;
  }

  if (!nice_agent_gather_candidates(agent_, stream_id_)) {
    LOG_ERROR("Failed to start candidate gathering");
    return -1;
  }

  return 0;
}

ICE_STATE IceAgent::GetIceState() {
  if (!nice_inited_) {
    return ICE_STATE_NOT_INITIALIZED;
  }
  if (nullptr == agent_) {
    return ICE_STATE_NULLPTR;
  }
  if (destroyed_) {
    return ICE_STATE_DESTROYED;
  }
  state_ = (ICE_STATE)nice_agent_get_component_state(agent_, stream_id_, 1);
  return state_;
}

int IceAgent::Send(const char* data, size_t size) {
  if (!nice_inited_) {
    LOG_ERROR("Nice agent has not been initialized");
    return -1;
  }

  if (nullptr == agent_) {
    LOG_ERROR("Nice agent is nullptr");
    return -1;
  }

  if (destroyed_) {
    return -1;
  }

  if (agent_closed_) {
    LOG_ERROR("Nice agent is closed");
    return -1;
  }

  gint ret = nice_agent_send(agent_, stream_id_, 1, (guint)size, data);

#ifdef SAVE_IO_STREAM
  if (file_out_) fwrite(data, 1, size, file_out_);
#endif

  return (ret >= 0) ? 0 : -1;
}

void IceAgent::CleanupDtls() {
  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (ssl_ctx_) {
    SSL_CTX_free(ssl_ctx_);
    ssl_ctx_ = nullptr;
  }
  bio_ = nullptr;
  dtls_started_ = false;
  dtls_handshake_done_ = false;

  {
    std::lock_guard<std::mutex> lk(dtls_mutex_);
    std::queue<std::vector<uint8_t>> empty;
    std::swap(dtls_incoming_, empty);
  }
}

bool IceAgent::IsDtlsRecord(const uint8_t* data, size_t len) {
  if (len < 13) return false;  // DTLS Record Header = 13 bytes
  uint8_t ct = data[0];
  if (ct < 20 || ct > 25) return false;  // 20..25
  // version: 0xFE FF (DTLS1.0), 0xFE FD (DTLS1.2), 0xFE FC (DTLS1.3 draft)
  return (data[1] == 0xFE) &&
         (data[2] == 0xFF || data[2] == 0xFD || data[2] == 0xFC);
}

BIO_METHOD* IceAgent::BIO_s_nice() {
  static BIO_METHOD* m = nullptr;
  if (m) return m;
  m = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "libnice-bio");
  BIO_meth_set_write(m, &IceAgent::bio_nice_write);
  BIO_meth_set_read(m, &IceAgent::bio_nice_read);
  BIO_meth_set_create(m, &IceAgent::bio_nice_new);
  BIO_meth_set_destroy(m, &IceAgent::bio_nice_free);
  BIO_meth_set_ctrl(m, &IceAgent::bio_nice_ctrl);
  return m;
}

int IceAgent::bio_nice_write(BIO* b, const char* buf, int len) {
  IceAgent* self = reinterpret_cast<IceAgent*>(BIO_get_data(b));
  if (!self || !buf || len <= 0) return -1;
  int r = self->Send(buf, (size_t)len);
  if (r == 0) return len;
  return -1;
}

int IceAgent::bio_nice_read(BIO* b, char* buf, int len) {
  IceAgent* self = reinterpret_cast<IceAgent*>(BIO_get_data(b));
  if (!self || !buf || len <= 0) return -1;

  std::lock_guard<std::mutex> lk(self->dtls_mutex_);
  if (self->dtls_incoming_.empty()) {
    BIO_set_retry_read(b);
    return -1;
  }
  auto pkt = std::move(self->dtls_incoming_.front());
  self->dtls_incoming_.pop();

  int copy = std::min<int>(len, (int)pkt.size());
  std::memcpy(buf, pkt.data(), copy);
  return copy;
}

int IceAgent::bio_nice_new(BIO* b) {
  BIO_set_init(b, 1);
  return 1;
}

int IceAgent::bio_nice_free(BIO* b) { return 1; }

long IceAgent::bio_nice_ctrl(BIO* b, int cmd, long num, void* ptr) {
  switch (cmd) {
    case BIO_CTRL_FLUSH:
      return 1;
    case BIO_CTRL_DGRAM_QUERY_MTU:
      return 1200;
    case BIO_CTRL_DGRAM_GET_MTU:
      return 1200;
    case BIO_CTRL_DGRAM_SET_CONNECTED:
      return 1;
    case BIO_CTRL_PENDING:
    case BIO_CTRL_WPENDING:
      return 0;
    default:
      return 0;
  }
}

int IceAgent::StartDtls(bool is_client) {
  if (dtls_started_ || !enable_srtp_) return 0;

  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_ssl_algorithms();

  ssl_ctx_ = SSL_CTX_new(DTLS_method());
  if (!ssl_ctx_) {
    LOG_ERROR("SSL_CTX_new failed");
    return -1;
  }

  if (SSL_CTX_set_tlsext_use_srtp(ssl_ctx_,
                                  "SRTP_AEAD_AES_128_GCM:SRTP_AES128_CM_SHA1_"
                                  "80:SRTP_AES128_CM_SHA1_32") != 0) {
    LOG_ERROR("SSL_CTX_set_tlsext_use_srtp failed");
    return -1;
  }

  if (!dtls_cert_ || !dtls_pkey_) {
    LOG_ERROR("DTLS cert/key not generated");
    return -1;
  }
  if (SSL_CTX_use_certificate(ssl_ctx_, dtls_cert_) != 1) {
    LOG_ERROR("use_certificate failed");
    return -1;
  }
  if (SSL_CTX_use_PrivateKey(ssl_ctx_, dtls_pkey_) != 1) {
    LOG_ERROR("use_private_key failed");
    return -1;
  }
  if (SSL_CTX_check_private_key(ssl_ctx_) != 1) {
    LOG_ERROR("check_private_key failed");
    return -1;
  }

  SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
  SSL_CTX_set_cert_verify_callback(ssl_ctx_, DtlsVerifyCallback, this);

  ssl_ = SSL_new(ssl_ctx_);
  if (!ssl_) {
    LOG_ERROR("SSL_new failed");
    return -1;
  }

  SSL_set_app_data(ssl_, this);

  bio_ = BIO_new(BIO_s_nice());
  BIO_set_data(bio_, this);
  SSL_set_bio(ssl_, bio_, bio_);

  if (is_client)
    SSL_set_connect_state(ssl_);
  else
    SSL_set_accept_state(ssl_);

  dtls_started_ = true;

  int ret = SSL_do_handshake(ssl_);
  if (ret == 1) {
    dtls_handshake_done_ = true;
    const SRTP_PROTECTION_PROFILE* prof = SSL_get_selected_srtp_profile(ssl_);
    LOG_INFO("DTLS handshake completed immediately. SRTP profile: {}",
             prof ? prof->name : "(none)");
    return 0;
  }
  int err = SSL_get_error(ssl_, ret);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
    LOG_INFO("DTLS handshake started, awaiting peer packets...");
    return 0;
  }
  LOG_ERROR("DTLS handshake start failed, err={}", err);

  log_openssl_errors();
  return -1;
}

void IceAgent::GenerateDtlsCertificate(int days_valid) {
  EVP_PKEY_CTX* keygen_ctx = nullptr;
  EVP_PKEY* pkey = nullptr;
  X509* x509 = nullptr;

  auto fail = [&]() {
    log_openssl_errors();
    if (x509) {
      X509_free(x509);
      x509 = nullptr;
    }
    if (pkey) {
      EVP_PKEY_free(pkey);
      pkey = nullptr;
    }
    if (keygen_ctx) {
      EVP_PKEY_CTX_free(keygen_ctx);
      keygen_ctx = nullptr;
    }
  };

  keygen_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
  if (!keygen_ctx) {
    LOG_ERROR("EVP_PKEY_CTX_new_id failed");
    fail();
    return;
  }

  if (EVP_PKEY_keygen_init(keygen_ctx) <= 0) {
    LOG_ERROR("EVP_PKEY_keygen_init failed");
    fail();
    return;
  }

  if (EVP_PKEY_CTX_set_rsa_keygen_bits(keygen_ctx, 2048) <= 0) {
    LOG_ERROR("EVP_PKEY_CTX_set_rsa_keygen_bits failed");
    fail();
    return;
  }

  if (EVP_PKEY_keygen(keygen_ctx, &pkey) <= 0) {
    LOG_ERROR("EVP_PKEY_keygen failed");
    fail();
    return;
  }

  x509 = X509_new();
  if (!x509) {
    LOG_ERROR("X509_new failed");
    fail();
    return;
  }

  if (ASN1_INTEGER_set(X509_get_serialNumber(x509), 1) != 1 ||
      X509_gmtime_adj(X509_get_notBefore(x509), 0) == nullptr ||
      X509_gmtime_adj(X509_get_notAfter(x509),
                      (long)60 * 60 * 24 * days_valid) == nullptr ||
      X509_set_pubkey(x509, pkey) != 1) {
    LOG_ERROR("Failed to initialize X509 fields");
    fail();
    return;
  }

  X509_NAME* name = X509_get_subject_name(x509);
  if (!name ||
      X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                 (unsigned char*)"DTLS-SRTP Self-Signed", -1,
                                 -1, 0) != 1 ||
      X509_set_issuer_name(x509, name) != 1) {
    LOG_ERROR("Failed to set X509 subject/issuer");
    fail();
    return;
  }

  if (X509_sign(x509, pkey, EVP_sha256()) <= 0) {
    LOG_ERROR("X509_sign failed");
    fail();
    return;
  }

  std::string fingerprint = ComputeFingerprint(x509);

  if (dtls_pkey_) {
    EVP_PKEY_free(dtls_pkey_);
  }
  if (dtls_cert_) {
    X509_free(dtls_cert_);
  }

  dtls_pkey_ = pkey;
  dtls_cert_ = x509;
  dtls_fingerprint_ = std::move(fingerprint);

  EVP_PKEY_CTX_free(keygen_ctx);
}

std::string IceAgent::AppendFingerprintLine(const std::string& sdp) {
  return sdp + "a=fingerprint:sha-256 " + dtls_fingerprint_ + "\r\n";
}

std::string IceAgent::ComputeFingerprint(X509* cert) {
  unsigned int n = 0;
  unsigned char md[EVP_MAX_MD_SIZE];
  if (X509_digest(cert, EVP_sha256(), md, &n) != 1) {
    throw std::runtime_error("Failed to compute DTLS fingerprint");
  }

  std::ostringstream oss;
  for (unsigned int i = 0; i < n; i++) {
    if (i) oss << ":";
    oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
        << (int)md[i];
  }
  return oss.str();
}

std::string IceAgent::ExtractAndStripFingerprint(const std::string& sdp) {
  if (!remote_fingerprint_.empty()) {
    return sdp;
  }

  std::size_t pos = sdp.rfind("a=fingerprint:");
  if (pos != std::string::npos) {
    std::size_t space_pos = sdp.find(' ', pos);
    if (space_pos != std::string::npos) {
      remote_fingerprint_ = sdp.substr(space_pos + 1);

      while (!remote_fingerprint_.empty() &&
             (remote_fingerprint_.back() == '\r' ||
              remote_fingerprint_.back() == '\n')) {
        remote_fingerprint_.pop_back();
      }
    }

    std::size_t cut_pos =
        (pos > 0 && (sdp[pos - 1] == '\n' || sdp[pos - 1] == '\r')) ? pos - 1
                                                                    : pos;
    LOG_INFO("Got fingerprint");
    return sdp.substr(0, cut_pos);
  } else {
    enable_srtp_ = false;
  }

  return sdp;
}

void IceAgent::OnNiceRecvStatic(NiceAgent* agent, guint stream_id,
                                guint component_id, guint size, gchar* buffer,
                                gpointer data) {
  IceAgent* self = reinterpret_cast<IceAgent*>(data);
  if (!self) return;
  self->OnNiceRecv(agent, stream_id, component_id, size, buffer);
}

void IceAgent::OnNiceRecv(NiceAgent* agent, guint stream_id, guint component_id,
                          guint size, gchar* buffer) {
#ifdef SAVE_IO_STREAM
  if (file_in_) fwrite(buffer, 1, size, file_in_);
#endif

  const bool looks_dtls =
      IsDtlsRecord(reinterpret_cast<uint8_t*>(buffer), size);
  if (enable_srtp_ && looks_dtls) {
    if (dtls_started_) {
      {
        std::lock_guard<std::mutex> lk(dtls_mutex_);
        dtls_incoming_.push(
            std::vector<uint8_t>((uint8_t*)buffer, (uint8_t*)buffer + size));
      }

      int ret = SSL_do_handshake(ssl_);
      if (ret <= 0) {
        int err = SSL_get_error(ssl_, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
          return;
        } else {
          LOG_ERROR("SSL_do_handshake failed, err={}", err);
          log_openssl_errors();
          return;
        }
      }

      if (SSL_is_init_finished(ssl_) && !dtls_handshake_done_) {
        dtls_handshake_done_ = true;

        const SRTP_PROTECTION_PROFILE* prof =
            SSL_get_selected_srtp_profile(ssl_);
        LOG_INFO("DTLS handshake done. SRTP profile: {}",
                 prof ? prof->name : "(none)");

        bool verified = false;
        X509* peer = SSL_get_peer_certificate(ssl_);
        if (peer) {
          std::string fp = ComputeFingerprint(peer);
          X509_free(peer);

          if (fp != remote_fingerprint_) {
            LOG_ERROR("DTLS fingerprint mismatch! expected {} got {}",
                      remote_fingerprint_, fp);
          } else {
            LOG_INFO("DTLS peer fingerprint verified");
            verified = true;
          }
        } else {
          LOG_ERROR("Peer certificate missing");
        }

        if (verified && on_cb_dtls_done_) {
          on_cb_dtls_done_(user_ptr_);
        }
      }
      return;
    }

    LOG_WARN("Received DTLS record before the local handshake started");
    return;
  }

  if (!enable_srtp_ && looks_dtls) {
    LOG_WARN("Ignoring DTLS record because SRTP is disabled");
    return;
  }

  if (on_recv_) {
    on_recv_(agent, stream_id, component_id, size, buffer, user_ptr_);
  }
}

void IceAgent::OnNiceStateChangedStatic(NiceAgent* agent, guint stream_id,
                                        guint component_id,
                                        NiceComponentState state,
                                        gpointer data) {
  auto* self = reinterpret_cast<IceAgent*>(data);
  if (self) {
    self->OnNiceStateChanged(stream_id, component_id, state);
  }
}

void IceAgent::OnNiceStateChanged(guint stream_id, guint component_id,
                                  NiceComponentState state) {
  if (stream_id != stream_id_ || component_id != NICE_COMPONENT_TYPE_RTP) {
    return;
  }
  if (state == NICE_COMPONENT_STATE_READY) {
    if (!dtls_started_ && !remote_fingerprint_.empty() && enable_srtp_) {
      if (StartDtls(controlling_) != 0) {
        LOG_ERROR("StartDtls failed");
      } else {
        LOG_INFO("DTLS handshake initiated");
      }
    }
  }
  if (on_state_changed_) {
    on_state_changed_(agent_, stream_id, component_id, state, user_ptr_);
  }
}

bool IceAgent::ExportSrtpKeys(std::vector<uint8_t>& local_key,
                              std::vector<uint8_t>& local_salt,
                              std::vector<uint8_t>& remote_key,
                              std::vector<uint8_t>& remote_salt,
                              bool local_is_client_sender) const {
  if (!dtls_handshake_done_ || !ssl_) {
    LOG_ERROR("DTLS handshake not done");
    return false;
  }

  const SRTP_PROTECTION_PROFILE* prof = SSL_get_selected_srtp_profile(ssl_);
  if (!prof) {
    LOG_ERROR("No SRTP profile selected");
    return false;
  }

  size_t key_len = 0, salt_len = 0;
  if (std::strcmp(prof->name, "SRTP_AEAD_AES_128_GCM") == 0) {
    key_len = 16;
    salt_len = 12;  // AEAD GCM
  } else if (std::strcmp(prof->name, "SRTP_AES128_CM_SHA1_80") == 0 ||
             std::strcmp(prof->name, "SRTP_AES128_CM_SHA1_32") == 0) {
    key_len = 16;
    salt_len = 14;  // CTR + HMAC
  } else {
    key_len = 16;
    salt_len = 14;
  }

  const size_t block = key_len + salt_len;
  const size_t total = 2 * block;

  std::vector<uint8_t> material(total);
  static const char kLabel[] = "EXTRACTOR-dtls_srtp";
  if (SSL_export_keying_material(ssl_, material.data(), total, kLabel,
                                 sizeof(kLabel) - 1, nullptr, 0, 0) != 1) {
    LOG_ERROR("SSL_export_keying_material failed");
    return false;
  }

  const uint8_t* client_key = material.data();
  const uint8_t* client_salt = material.data() + key_len;
  const uint8_t* server_key = material.data() + block;
  const uint8_t* server_salt = material.data() + block + key_len;

  const uint8_t* local_k = local_is_client_sender ? client_key : server_key;
  const uint8_t* local_s = local_is_client_sender ? client_salt : server_salt;
  const uint8_t* remote_k = local_is_client_sender ? server_key : client_key;
  const uint8_t* remote_s = local_is_client_sender ? server_salt : client_salt;

  local_key.assign(local_k, local_k + key_len);
  local_salt.assign(local_s, local_s + salt_len);
  remote_key.assign(remote_k, remote_k + key_len);
  remote_salt.assign(remote_s, remote_s + salt_len);

  return true;
}

}  // namespace minirtc
