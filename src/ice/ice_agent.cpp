#include "ice_agent.h"

#include <glib.h>

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

#include "ice_server_resolver.h"
#include "ice_utils.h"
#include "log.h"
#include "nice/punch.h"

// #define SAVE_IO_STREAM

namespace minirtc {

namespace {

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
                   TurnMode turn_mode, bool enable_srtp,
                   const IceServerConfiguration& ice_config)
    : use_trickle_ice_(use_trickle_ice),
      use_reliable_ice_(use_reliable_ice),
      turn_mode_(turn_mode),
      enable_srtp_(enable_srtp),
      ice_config_(ice_config),
      controlling_(offer_peer),
      punch_offer_peer_(offer_peer) {}

IceAgent::~IceAgent() {
  if (!destroyed_.load()) {
    DestroyIceAgent();
  }

  CleanupDtls();

  NiceAgent* agent = agent_.exchange(nullptr);
  if (agent != nullptr) {
    g_object_unref(agent);
  }

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
  if (!ice_config_.Fresh()) return -1;
  if (nice_thread_.joinable() || nice_inited_) {
    LOG_ERROR("Nice agent has already been created");
    return -1;
  }

  {
    std::lock_guard<std::mutex> lk(init_mutex_);
    init_done_ = false;
    init_status_ = -1;
  }

  destroyed_.store(false);
  nice_inited_.store(false);
  init_failed_.store(false);
  agent_closed_.store(false);
  send_disabled_.store(false);
  p2p_enhancement_enabled_.store(false);
  relay_selected_ms_ = 0;
  punch_remote_supported_ = false;
  punch_retry_supported_ = false;
  punch_negotiated_once_ = false;
  punch_attempted_ = false;
  punch_relay_ready_ms_ = 0;
  punch_original_deadline_ms_ = 0;
  ++punch_epoch_;  // Invalidates all observation/handle work from an earlier
                   // Create.
  {
    std::lock_guard<std::mutex> lock(nat_mutex_);
    nat_samples_.clear();
    punch_snapshots_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(prediction_mutex_);
    port_predictor_ = RemotePortPredictor{};
  }
  stream_id_ = 0;
  agent_.store(nullptr);
  gcontext_.store(nullptr);
  gloop_.store(nullptr);

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

    GMainContext* context = g_main_context_new();
    gcontext_.store(context);
    if (context == nullptr) {
      LOG_ERROR("Failed to create glib main context");
      init_failed_.store(true);
      notify_init(-1);
      exit_nice_thread_ = true;
      return;
    }

    // Keep all sources and asynchronous callbacks for this NiceAgent on its
    // own context. Using a nullptr context here would make every IceAgent
    // share GLib's global-default context across their worker threads.
    // GMainLoop* loop = g_main_loop_new(nullptr, false);
    g_main_context_push_thread_default(context);

    GMainLoop* loop = g_main_loop_new(context, false);
    gloop_.store(loop);
    if (loop == nullptr) {
      LOG_ERROR("Failed to create glib main loop");
      init_failed_.store(true);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
      gcontext_.store(nullptr);
      notify_init(-1);
      exit_nice_thread_ = true;
      return;
    }

    // Use regular nomination from the start: libnice rejects late TCP
    // candidates if aggressive UDP checks have already started. Background
    // upgrades are enabled separately after negotiating the MiniRTC extension.
    NiceAgentOption agent_options = static_cast<NiceAgentOption>(
        NICE_AGENT_OPTION_REGULAR_NOMINATION |
        NICE_AGENT_OPTION_SUPPORT_RENOMINATION |
        (use_trickle_ice_ ? NICE_AGENT_OPTION_ICE_TRICKLE
                          : NICE_AGENT_OPTION_NONE) |
        (use_reliable_ice_ ? NICE_AGENT_OPTION_RELIABLE
                           : NICE_AGENT_OPTION_NONE));
    NiceAgent* agent = nice_agent_new_full(g_main_loop_get_context(loop),
                                           NICE_COMPATIBILITY_RFC5245,
                                           agent_options);
    agent_.store(agent);

    LOG_INFO(
        "Nice agent init with [trickle ice|{}], [reliable mode|{}], "
        "[nomination|regular], [renomination|true], [turn mode|{}]",
        use_trickle_ice_, use_reliable_ice_, TurnModeName(turn_mode_));

    if (agent == nullptr) {
      LOG_ERROR("Failed to create agent_");
      init_failed_.store(true);
      g_main_loop_unref(loop);
      gloop_.store(nullptr);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
      gcontext_.store(nullptr);
      notify_init(-1);
      exit_nice_thread_ = true;
      return;
    }

    // Allow room for multi-interface candidates and promoted punch sockets.
    g_object_set(agent, "max-connectivity-checks", 512u, nullptr);

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(agent),
                                     "relay-upgrade-timeout") == nullptr) {
      LOG_WARN("libnice has no relay upgrade extension; rebuild dependencies");
    }

    std::vector<StunEndpoint> stun_endpoints;
    for (const auto& server : ice_config_.servers) {
      if (!server.turn) stun_endpoints.push_back({server.host, server.port});
    }
    if (!stun_endpoints.empty() && !IsTurnForced(turn_mode_)) {
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(agent),
                                       "stun-servers")) {
        std::string endpoints;
        for (const auto& endpoint : stun_endpoints) {
          if (!endpoints.empty()) endpoints += ',';
          endpoints += endpoint.ToString();
        }
        g_object_set(agent, "stun-servers", endpoints.c_str(), nullptr);
        g_signal_connect(agent, "stun-mapping", G_CALLBACK(OnStunMappingStatic),
                         this);
        LOG_INFO("ICE same-socket STUN endpoints [{}]", endpoints);
      } else {
        const auto& endpoint = stun_endpoints.front();
        g_object_set(agent, "stun-server", endpoint.host.c_str(),
                     "stun-server-port", static_cast<guint>(endpoint.port),
                     nullptr);
        LOG_WARN(
            "libnice lacks multi-STUN support; using first endpoint; rebuild "
            "dependencies");
      }
    }
    g_object_set(agent, "controlling-mode", controlling_, nullptr);
    // Enable port mapping unless the caller explicitly requires relay-only ICE.
    const bool enable_upnp = !IsTurnForced(turn_mode_);
    g_object_set(agent, "upnp", enable_upnp,
                 "upnp-timeout", 3000u, nullptr);
    LOG_INFO("ICE UPnP mapping [{}], discovery timeout 3000 ms",
             enable_upnp);

    g_signal_connect(agent, "candidate-gathering-done",
                     G_CALLBACK(on_gathering_done_), user_ptr_);
    g_signal_connect(agent, "new-selected-pair",
                     G_CALLBACK(OnNiceSelectedPairStatic), this);
    g_signal_connect(agent, "new-candidate-full", G_CALLBACK(on_new_candidate_),
                     user_ptr_);
    g_signal_connect(agent, "component-state-changed",
                     G_CALLBACK(&IceAgent::OnNiceStateChangedStatic), this);

    stream_id_ = nice_agent_add_stream(agent, n_components_);
    if (stream_id_ == 0) {
      LOG_ERROR("Failed to add stream");
      init_failed_.store(true);
      g_object_unref(agent);
      agent_.store(nullptr);
      g_main_loop_unref(loop);
      gloop_.store(nullptr);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
      gcontext_.store(nullptr);
      notify_init(-1);
      exit_nice_thread_ = true;
      return;
    }

    if (has_video_stream_) {
      nice_agent_set_stream_name(agent, stream_id_, "video");
    }

    if (IsTurnEnabled(turn_mode_)) {
      bool relay_registered = false;
      for (const auto& server :
           ResolveTurnServerEndpoints(ice_config_, turn_mode_)) {
        const auto type =
            server.tcp ? NICE_RELAY_TYPE_TURN_TCP : NICE_RELAY_TYPE_TURN_UDP;
        relay_registered |=
            AddTurnRelay(agent, stream_id_, server.host, server.port,
                         server.username, server.password, type);
      }

      if (!relay_registered || !ice_config_.Fresh()) {
        LOG_WARN(
            "No TURN endpoint could be registered from signaling configuration "
            "[{}]",
            ice_config_.id);
        if (IsTurnForced(turn_mode_) || !ice_config_.Fresh()) {
          init_failed_.store(true);
          g_object_unref(agent);
          agent_.store(nullptr);
          g_main_loop_unref(loop);
          gloop_.store(nullptr);
          g_main_context_pop_thread_default(context);
          g_main_context_unref(context);
          gcontext_.store(nullptr);
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

    nice_inited_.store(true);
    init_failed_.store(false);
    notify_init(0);
    g_main_loop_run(loop);
    g_main_context_pop_thread_default(context);
    exit_nice_thread_ = true;
  });

  {
    std::unique_lock<std::mutex> lk(init_mutex_);
    init_cv_.wait(lk, [this]() { return init_done_; });
  }

  if (init_status_ != 0 || init_failed_.load()) {
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

gboolean IceAgent::CloseNiceAgentStatic(gpointer data) {
  auto* self = static_cast<IceAgent*>(data);
  NiceAgent* agent = self->agent_.load();
  self->StopPunch();

  if (self->nice_inited_.load() && agent != nullptr &&
      self->stream_id_ != 0) {
    // close_async must run while this agent's thread-default context is still
    // being iterated. The completion callback removes the stream only after
    // libnice has released remote resources such as TURN allocations.
    nice_agent_close_async(agent, &IceAgent::OnNiceAgentClosedStatic, self);
  } else {
    self->agent_closed_.store(true);
    GMainLoop* loop = self->gloop_.load();
    if (loop != nullptr) {
      g_main_loop_quit(loop);
    }
  }

  return G_SOURCE_REMOVE;
}

void IceAgent::OnNiceAgentClosedStatic(
    GObject* source, [[maybe_unused]] GAsyncResult* result, gpointer data) {
  auto* self = static_cast<IceAgent*>(data);
  NiceAgent* agent = NICE_AGENT(source);

  if (self->stream_id_ != 0) {
    nice_agent_remove_stream(agent, self->stream_id_);
    self->stream_id_ = 0;
  }

  self->agent_closed_.store(true);
  LOG_INFO("Nice agent closed");

  GMainLoop* loop = self->gloop_.load();
  if (loop != nullptr) {
    g_main_loop_quit(loop);
  }
}

int IceAgent::DestroyIceAgent() {
  std::lock_guard<std::mutex> destroy_lock(destroy_mutex_);

  StopSending();
  if (destroyed_.exchange(true)) {
    return 0;
  }

  NiceAgent* agent = agent_.load();
  GMainContext* context = gcontext_.load();
  GMainLoop* loop = gloop_.load();

  if (context != nullptr && loop != nullptr && nice_thread_.joinable()) {
    g_main_context_invoke(context, &IceAgent::CloseNiceAgentStatic, this);
  } else if (loop != nullptr) {
    g_main_loop_quit(loop);
  }

  if (nice_thread_.joinable()) {
    nice_thread_.join();
  }

  // Destroy the agent while its private context is still alive so any
  // remaining libnice sources are detached before the context is released.
  {
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    if (agent != nullptr) {
      g_object_unref(agent);
      agent_.store(nullptr);
    }
  }

  if (loop != nullptr) {
    g_main_loop_unref(loop);
    gloop_.store(nullptr);
  }

  if (context != nullptr) {
    g_main_context_unref(context);
    gcontext_.store(nullptr);
  }

  {
    std::lock_guard<std::mutex> lk(init_mutex_);
    init_done_ = false;
    init_status_ = -1;
  }
  nice_inited_.store(false);
  init_failed_.store(false);
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

  if (!use_reliable_ice_ && !IsTurnForced(turn_mode_) &&
      g_object_class_find_property(G_OBJECT_GET_CLASS(agent_.load()),
                                   "relay-upgrade-timeout") != nullptr) {
    local_sdp_ += std::string(kRelayUpgradeAttribute) + "\r\n";
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(agent_.load()),
                                     "stun-servers") != nullptr) {
      local_sdp_ += std::string(kP2pEnhancementAttribute) + "\r\n";
    }
  }
  if (CanAdvertiseUdpPunch()) {
    local_sdp_ += std::string(kUdpPunchAttribute) + "\r\n" +
                  kUdpPunchRetryAttribute + "\r\n" +
                  kUdpPunchFingerprintAttribute + "sha-256 " +
                  dtls_fingerprint_ + "\r\n";
  }
  return local_sdp_;
}

bool IceAgent::CanAdvertiseUdpPunch() const {
  auto* agent = agent_.load();
  return agent && !destroyed_ &&
         PunchEligible(punch_config_, PunchPlatformSupported(),
                       nice_agent_punch_get_v1_abi(), use_reliable_ice_,
                       turn_mode_ == TurnMode::TurnAutoUdpTcp, n_components_) &&
         g_object_class_find_property(G_OBJECT_GET_CLASS(agent),
                                      "relay-upgrade-timeout") &&
         g_object_class_find_property(G_OBJECT_GET_CLASS(agent),
                                      "stun-servers");
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

  const std::string remote_ufrag = GetIceUsername(remote_sdp);
  if (remote_ufrag.empty() ||
      (!punch_remote_ufrag_.empty() && punch_remote_ufrag_ != remote_ufrag)) {
    return -1;
  }
  const auto fingerprints = SplitIceFingerprint(remote_sdp);
  if (!fingerprints || fingerprints->sdp.empty()) return -1;
  const auto& fingerprint = fingerprints->fingerprint.empty()
                                ? fingerprints->punch_fingerprint
                                : fingerprints->fingerprint;
  if (!remote_fingerprint_.empty() && remote_fingerprint_ != fingerprint)
    return -1;
  const bool standard_srtp_key_layout =
      HasIceAttribute(remote_sdp, kSrtpKeyLayoutAttribute);
  if (!punch_remote_ufrag_.empty() &&
      remote_standard_srtp_key_layout_ != standard_srtp_key_layout) {
    LOG_ERROR("Changed SRTP key layout requires a new ICE transport");
    return -1;
  }
  remote_fingerprint_ = fingerprint;
  remote_standard_srtp_key_layout_ = standard_srtp_key_layout;
  // Only the standard media fingerprint requests SRTP. The punch fingerprint
  // authenticates DTLS independently and preserves legacy RTP fallback.
  if (fingerprints->fingerprint.empty()) enable_srtp_ = false;
  // Both peers must agree to keep checking after the first regular nomination.
  // An unmodified peer continues to use the standard libnice behavior.
  const bool has_upgrade_extension =
      g_object_class_find_property(G_OBJECT_GET_CLASS(agent_.load()),
                                   "relay-upgrade-timeout") != nullptr;
  const bool upgrade = has_upgrade_extension && !use_reliable_ice_ &&
                       SupportsRelayUpgrade(remote_sdp) &&
                       !IsTurnForced(turn_mode_);
  p2p_enhancement_enabled_.store(
      upgrade && SupportsP2pEnhancement(remote_sdp) &&
      g_object_class_find_property(G_OBJECT_GET_CLASS(agent_.load()),
                                   "stun-servers"));
  if (has_upgrade_extension) {
    g_object_set(agent_.load(), "relay-upgrade-timeout", upgrade ? 30000u : 0u,
                 nullptr);
  }
  LOG_INFO("ICE relay upgrade peer_support={} forced_relay={} reliable={} window_ms={}",
           SupportsRelayUpgrade(remote_sdp), IsTurnForced(turn_mode_),
           use_reliable_ice_, upgrade ? 30000 : 0);
  // Parsing candidates can make ICE READY on its context immediately. Publish
  // the independent DTLS requirement before that callback can run.
  punch_remote_supported_ =
      upgrade && CanAdvertiseUdpPunch() && SupportsUdpPunch(remote_sdp) &&
      !remote_fingerprint_.empty() &&
      (enable_srtp_ || !fingerprints->punch_fingerprint.empty());
  punch_retry_supported_ =
      punch_remote_supported_ && SupportsUdpPunchRetry(remote_sdp);
  int ret = nice_agent_parse_remote_sdp(agent_, fingerprints->sdp.c_str());
  if (ret >= 0) {
    if (punch_remote_ufrag_.empty()) punch_remote_ufrag_ = remote_ufrag;
    if (punch_remote_supported_) punch_negotiated_once_ = true;
    ProbePredictedRemoteCandidates();
    return 0;
  } else {
    punch_remote_supported_ = false;
    punch_retry_supported_ = false;
    LOG_ERROR("Failed to parse remote sdp: [{}]", fingerprints->sdp);
    return -1;
  }
}

int IceAgent::AddRemoteCandidate(const std::string& candidate_sdp) {
  if (!nice_inited_ || agent_ == nullptr || destroyed_ || stream_id_ == 0) {
    LOG_ERROR("Cannot add remote candidate to an inactive ICE agent");
    return -1;
  }

  NiceCandidate* candidate = nice_agent_parse_remote_candidate_sdp(
      agent_, stream_id_, candidate_sdp.c_str());
  if (candidate == nullptr) {
    LOG_ERROR("Failed to parse remote ICE candidate: [{}]", candidate_sdp);
    return -1;
  }

  if (candidate->component_id != NICE_COMPONENT_TYPE_RTP) {
    LOG_WARN("Reject ICE candidate for unsupported component {}",
             candidate->component_id);
    nice_candidate_free(candidate);
    return -1;
  }

  GSList* candidates = nullptr;
  candidates = g_slist_append(candidates, candidate);
  const int added = nice_agent_set_remote_candidates(
      agent_, stream_id_, candidate->component_id, candidates);
  if (added > 0) {
    char address[NICE_ADDRESS_STRING_LEN] = {};
    nice_address_to_string(&candidate->addr, address);
    LOG_INFO("Remote ICE candidate type={} transport={} address={}:{} priority={}",
             nice_candidate_type_to_string(candidate->type),
             nice_candidate_transport_to_string(candidate->transport), address,
             nice_address_get_port(&candidate->addr), candidate->priority);
  }
  g_slist_free(candidates);
  nice_candidate_free(candidate);

  if (added <= 0) {
    LOG_ERROR("libnice rejected remote ICE candidate: [{}]", candidate_sdp);
    return -1;
  }

  ProbePredictedRemoteCandidates();
  return 0;
}

void IceAgent::OnStunMappingStatic(NiceAgent*, NiceCandidate* sample,
                                   const gchar* server_ip, guint server_port,
                                   guint sequence, gpointer data) {
  auto* self = static_cast<IceAgent*>(data);
  if (!sample || !server_ip || self->destroyed_) return;
  char base[NICE_ADDRESS_STRING_LEN] = {}, mapped[NICE_ADDRESS_STRING_LEN] = {};
  nice_address_to_string(&sample->base_addr, base);
  nice_address_to_string(&sample->addr, mapped);
  const std::string key =
      std::to_string(sample->stream_id) + "/" +
      std::to_string(sample->component_id) + "/" + base + ":" +
      std::to_string(nice_address_get_port(&sample->base_addr));
  NatMappingAnalysis analysis;
  {
    std::lock_guard<std::mutex> lock(self->nat_mutex_);
    if (!self->nat_samples_.count(key) && self->nat_samples_.size() >= 64)
      return;
    auto& samples = self->nat_samples_[key];
    if (samples.size() >= 32) return;
    samples.push_back(
        {server_ip, static_cast<uint16_t>(server_port), mapped,
         static_cast<uint16_t>(nice_address_get_port(&sample->addr)),
         sequence});
    if (self->punch_config_.enabled() && PunchPlatformSupported()) {
      auto& snapshot = self->punch_snapshots_[key];
      if (!snapshot.socket) {
        snapshot.socket = ++self->punch_snapshot_id_;
        snapshot.generation = self->punch_epoch_;
        snapshot.local_ip = base;
        snapshot.local_port = nice_address_get_port(&sample->base_addr);
        snapshot.component = sample->component_id;
        // The STUN result's foundation describes srflx. Resolve its actual
        // original host foundation, so later base-lifetime checks compare host.
        GSList* candidates = nice_agent_get_local_candidates(
            self->agent_, sample->stream_id, sample->component_id);
        for (auto* i = candidates; i; i = i->next) {
          const auto* c = static_cast<NiceCandidate*>(i->data);
          if (c->type == NICE_CANDIDATE_TYPE_HOST &&
              c->transport == NICE_CANDIDATE_TRANSPORT_UDP &&
              nice_address_equal(&c->addr, &sample->base_addr))
            snapshot.foundation = c->foundation;
        }
        g_slist_free_full(
            candidates, reinterpret_cast<GDestroyNotify>(nice_candidate_free));
      }
      snapshot.samples.push_back(
          {snapshot.socket, snapshot.generation, server_ip, mapped,
           static_cast<uint16_t>(server_port),
           static_cast<uint16_t>(nice_address_get_port(&sample->addr)),
           g_get_monotonic_time() / 1000, sequence});
    }
    analysis = AnalyzeNatMappings(samples);
  }
  LOG_INFO(
      "ICE STUN sample base={} server={}:{} mapped={}:{} sequence={} "
      "samples={} mapping={} port_pattern={} step={}",
      key, server_ip, server_port, mapped, nice_address_get_port(&sample->addr),
      sequence, analysis.samples, analysis.mapping, analysis.port_pattern,
      analysis.port_step);
}

void IceAgent::ProbePredictedRemoteCandidates() {
  NiceAgent* agent = agent_.load();
  if (!agent || destroyed_ || !p2p_enhancement_enabled_) return;
  NiceCandidate *selected_local = nullptr, *selected_remote = nullptr;
  if (nice_agent_get_selected_pair(agent, stream_id_, NICE_COMPONENT_TYPE_RTP,
                                   &selected_local, &selected_remote) &&
      selected_local && selected_remote &&
      selected_local->type != NICE_CANDIDATE_TYPE_RELAYED &&
      selected_remote->type != NICE_CANDIDATE_TYPE_RELAYED)
    return;

  GSList* remote = nice_agent_get_remote_candidates(agent, stream_id_,
                                                    NICE_COMPONENT_TYPE_RTP);
  GSList* predicted = nullptr;
  std::set<std::string> known_addresses;
  for (GSList* item = remote; item; item = item->next) {
    auto* c = static_cast<NiceCandidate*>(item->data);
    if (c->transport != NICE_CANDIDATE_TRANSPORT_UDP) continue;
    char ip[NICE_ADDRESS_STRING_LEN] = {};
    nice_address_to_string(&c->addr, ip);
    known_addresses.insert(std::string(ip) + ":" +
                           std::to_string(nice_address_get_port(&c->addr)));
  }
  {
    std::lock_guard<std::mutex> lock(prediction_mutex_);
    for (GSList* item = remote; item; item = item->next) {
      auto* candidate = static_cast<NiceCandidate*>(item->data);
      if (candidate->type != NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE ||
          candidate->transport != NICE_CANDIDATE_TRANSPORT_UDP ||
          strncmp(candidate->foundation, "px-", 3) == 0 ||
          !nice_address_is_valid(&candidate->base_addr) ||
          nice_address_get_port(&candidate->base_addr) == 0)
        continue;
      char ip[NICE_ADDRESS_STRING_LEN] = {}, base[NICE_ADDRESS_STRING_LEN] = {};
      nice_address_to_string(&candidate->addr, ip);
      if (!IsPublicIpv4ForPrediction(ip)) continue;
      if (nice_address_is_valid(&candidate->base_addr))
        nice_address_to_string(&candidate->base_addr, base);
      const std::string group =
          std::string(ip) + "/" + base + ":" +
          std::to_string(nice_address_get_port(&candidate->base_addr));
      const auto ports = port_predictor_.Observe(
          group, nice_address_get_port(&candidate->addr));
      for (uint16_t port : ports) {
        // set_remote_candidates updates existing candidates by address. A
        // hypothesis must never overwrite a real candidate from another group.
        if (!known_addresses
                 .insert(std::string(ip) + ":" + std::to_string(port))
                 .second)
          continue;
        NiceCandidate* hypothesis = nice_candidate_copy(candidate);
        nice_address_set_port(&hypothesis->addr, port);
        g_snprintf(hypothesis->foundation, sizeof(hypothesis->foundation),
                   "px-%u", port);
        // Preserve the normal preference for observed srflx and host
        // candidates.
        if (hypothesis->priority > 256) hypothesis->priority -= 256;
        predicted = g_slist_prepend(predicted, hypothesis);
      }
      if (!ports.empty()) {
        LOG_INFO("ICE bounded port prediction group={} new={} total={}/{}",
                 group, ports.size(), port_predictor_.predictions(),
                 RemotePortPredictor::kMaxPredictions);
      }
    }
  }
  g_slist_free_full(remote,
                    reinterpret_cast<GDestroyNotify>(nice_candidate_free));
  if (predicted) {
    // Normal libnice checks provide pacing, credentials, peer-reflexive
    // learning and nomination. Never send application traffic to an unvalidated
    // guess.
    const int added = nice_agent_set_remote_candidates(
        agent, stream_id_, NICE_COMPONENT_TYPE_RTP, predicted);
    LOG_INFO(
        "ICE added {} predicted remote candidates for authenticated checks",
        added);
    g_slist_free_full(predicted,
                      reinterpret_cast<GDestroyNotify>(nice_candidate_free));
  }
}

int IceAgent::SetRemoteCandidateGatheringDone() {
  if (!nice_inited_ || agent_ == nullptr || destroyed_ || stream_id_ == 0) {
    LOG_ERROR("Cannot finish remote gathering on an inactive ICE agent");
    return -1;
  }

  if (!nice_agent_peer_candidate_gathering_done(agent_, stream_id_)) {
    LOG_ERROR("libnice rejected remote candidate-gathering-done");
    return -1;
  }

  LOG_INFO("Remote ICE candidate gathering is complete");
  return 0;
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
  if (send_disabled_) {
    return ICE_STATE_DESTROYED;
  }
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
  if (!data || size == 0 || size > static_cast<size_t>(G_MAXUINT) ||
      send_disabled_) {
    return -1;
  }

  std::lock_guard<std::mutex> send_lock(send_mutex_);
  if (send_disabled_) {
    return -1;
  }
  if (!nice_inited_) {
    LOG_ERROR("Nice agent has not been initialized");
    return -1;
  }

  NiceAgent* agent = agent_.load();
  if (nullptr == agent) {
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

  const gint ret = nice_agent_send(
      agent, stream_id_, 1, static_cast<guint>(size), data);

#ifdef SAVE_IO_STREAM
  if (file_out_) fwrite(data, 1, size, file_out_);
#endif

  return ret == static_cast<gint>(size) ? 0 : -1;
}

void IceAgent::StopSending() { send_disabled_.store(true); }

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
  dtls_peer_verified_ = false;
  remote_standard_srtp_key_layout_ = false;
  punch_remote_ufrag_.clear();
  remote_fingerprint_.clear();

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
  static BIO_METHOD* m = [] {
    BIO_METHOD* method = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "libnice-bio");
    if (method) {
      BIO_meth_set_write(method, &IceAgent::bio_nice_write);
      BIO_meth_set_read(method, &IceAgent::bio_nice_read);
      BIO_meth_set_create(method, &IceAgent::bio_nice_new);
      BIO_meth_set_destroy(method, &IceAgent::bio_nice_free);
      BIO_meth_set_ctrl(method, &IceAgent::bio_nice_ctrl);
    }
    return method;
  }();
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
  if (dtls_started_ || !ShouldUseDtls()) return 0;

  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_ssl_algorithms();

  ssl_ctx_ = SSL_CTX_new(DTLS_method());
  if (!ssl_ctx_) {
    LOG_ERROR("SSL_CTX_new failed");
    return -1;
  }

  if (enable_srtp_ &&
      SSL_CTX_set_tlsext_use_srtp(ssl_ctx_,
                                "SRTP_AEAD_AES_128_GCM") != 0) {
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
    return CompleteDtlsHandshake() ? 0 : -1;
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
  return sdp + kSrtpKeyLayoutAttribute + "\r\n" +
         "a=fingerprint:sha-256 " + dtls_fingerprint_ + "\r\n";
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

bool IceAgent::ShouldUseDtls() const {
  return !remote_fingerprint_.empty() &&
         (enable_srtp_ || punch_remote_supported_);
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

  if (punch_remote_supported_ && dtls_peer_verified_) MaybeStartPunch();
  if (punch_negotiated_once_ &&
      punch::IsMagic(reinterpret_cast<uint8_t*>(buffer), size)) {
    if (punch_runtime_ && !send_disabled_)
      punch_runtime_->Control(reinterpret_cast<uint8_t*>(buffer), size,
                              g_get_monotonic_time() / 1000);
    return;  // Includes relay-carried PROBE: never forward to
             // DTLS/SRTP/business.
  }

  const bool looks_dtls =
      IsDtlsRecord(reinterpret_cast<uint8_t*>(buffer), size);
  const bool use_dtls = ShouldUseDtls();
  if (use_dtls && looks_dtls) {
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

      if (SSL_is_init_finished(ssl_) && !dtls_handshake_done_)
        CompleteDtlsHandshake();
      return;
    }

    // A ClientHello can race the selected-pair/state callback. Keep a bounded
    // flight for StartDtls() once a usable ICE pair is available.
    if (!remote_fingerprint_.empty() && !send_disabled_ && size <= 2048) {
      std::lock_guard<std::mutex> lock(dtls_mutex_);
      if (dtls_incoming_.size() < 4) {
        dtls_incoming_.emplace(reinterpret_cast<uint8_t*>(buffer),
                               reinterpret_cast<uint8_t*>(buffer) + size);
      }
    }
    return;
  }

  if (!use_dtls && looks_dtls) {
    LOG_WARN("Ignoring DTLS record because DTLS was not negotiated");
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

void IceAgent::MaybeStartDtls() {
  if (dtls_started_ || !ShouldUseDtls() || send_disabled_ || destroyed_) return;
  const auto state = nice_agent_get_component_state(agent_, stream_id_, 1);
  if (state != NICE_COMPONENT_STATE_CONNECTED &&
      state != NICE_COMPONENT_STATE_READY)
    return;
  NiceCandidate *local = nullptr, *remote = nullptr;
  if (!nice_agent_get_selected_pair(agent_, stream_id_, 1, &local, &remote) ||
      !local || !remote)
    return;
  // The selected pair is already usable in CONNECTED. Authenticate while the
  // remaining ICE checks finish; punching still requires the READY backend.
  if (StartDtls(controlling_) != 0) LOG_ERROR("StartDtls failed");
}

void IceAgent::OnNiceStateChanged(guint stream_id, guint component_id,
                                  NiceComponentState state) {
  if (stream_id != stream_id_ || component_id != NICE_COMPONENT_TYPE_RTP) {
    return;
  }
  if (state == NICE_COMPONENT_STATE_READY) {
    int64_t no_ready = 0;
    punch_relay_ready_ms_.compare_exchange_strong(
        no_ready, g_get_monotonic_time() / 1000);
    // The backend's deadline query is READY-only. Remember its actual deadline
    // before late candidates can temporarily return ICE to CONNECTED; never
    // derive a fresh window from the time at which READY is restored.
    if (CanAdvertiseUdpPunch() && punch_remote_supported_ &&
        !punch_original_deadline_ms_)
      punch_original_deadline_ms_ =
          nice_agent_punch_get_deadline(agent_, stream_id_, 1) / 1000;
  }
  MaybeStartDtls();
  if (state == NICE_COMPONENT_STATE_READY) MaybeStartPunch();
  if (on_state_changed_) {
    on_state_changed_(agent_, stream_id, component_id, state, user_ptr_);
  }
}

bool IceAgent::CompleteDtlsHandshake() {
  if (dtls_handshake_done_) return dtls_peer_verified_;
  dtls_handshake_done_ = true;
  X509* peer = SSL_get_peer_certificate(ssl_);
  if (!peer) {
    LOG_ERROR("DTLS peer certificate missing");
    return false;
  }
  const std::string fingerprint = ComputeFingerprint(peer);
  X509_free(peer);
  if (remote_fingerprint_.empty() || fingerprint != remote_fingerprint_) {
    LOG_ERROR("DTLS peer fingerprint mismatch");
    return false;
  }
  dtls_peer_verified_ = true;
  MaybeStartPunch();
  LOG_INFO("DTLS peer fingerprint verified");
  if (on_cb_dtls_done_) on_cb_dtls_done_(user_ptr_);
  return true;
}

void IceAgent::OnNiceSelectedPairStatic(NiceAgent* agent, guint stream,
                                        guint component, const char* local,
                                        const char* remote, gpointer data) {
  auto* self = static_cast<IceAgent*>(data);
  if (stream == self->stream_id_ && component == NICE_COMPONENT_TYPE_RTP) {
    NiceCandidate *selected_local = nullptr, *selected_remote = nullptr;
    if (!self->relay_selected_ms_ &&
        nice_agent_get_selected_pair(agent, stream, component, &selected_local,
                                     &selected_remote) &&
        selected_local && selected_remote &&
        (selected_local->type == NICE_CANDIDATE_TYPE_RELAYED ||
         selected_remote->type == NICE_CANDIDATE_TYPE_RELAYED)) {
      // Start the trigger delay at relay selection, before READY/DTLS finish.
      self->relay_selected_ms_ = g_get_monotonic_time() / 1000;
    }
    self->MaybeStartDtls();
  }
  if (self->on_new_selected_pair_)
    self->on_new_selected_pair_(agent, stream, component, local, remote,
                                self->user_ptr_);
}

void IceAgent::MaybeStartPunch() {
  if (!gcontext_ || !g_main_context_is_owner(gcontext_)) return;
  if (!CanAdvertiseUdpPunch() || !punch_remote_supported_ ||
      !dtls_peer_verified_ || send_disabled_ || punch_source_ ||
      punch_attempted_ || !punch_relay_ready_ms_)
    return;
  NiceCandidate *local = nullptr, *remote = nullptr;
  if (!nice_agent_get_selected_pair(agent_, stream_id_, 1, &local, &remote) ||
      !local || !remote ||
      (local->type != NICE_CANDIDATE_TYPE_RELAYED &&
       remote->type != NICE_CANDIDATE_TYPE_RELAYED)) {
    return;
  }
  punch_packet_handler_ = g_signal_connect(
      agent_, "punch-packet", G_CALLBACK(OnPunchPacketStatic), this);
  punch_source_ = g_timeout_source_new(10);
  g_source_set_callback(punch_source_, PunchTickStatic, this, nullptr);
  g_source_attach(punch_source_, gcontext_);
}
void IceAgent::StopPunch() {
  punch_attempted_ = true;
  if (punch_source_) {
    g_source_destroy(punch_source_);
    g_source_unref(punch_source_);
    punch_source_ = nullptr;
  }
  if (punch_packet_handler_ && agent_) {
    g_signal_handler_disconnect(agent_, punch_packet_handler_);
    punch_packet_handler_ = 0;
  }
  if (punch_runtime_) {
    punch_runtime_->Stop("disconnect", false);
    punch_runtime_.reset();
  }
}
gboolean IceAgent::PunchTickStatic(gpointer data) {
  auto* self = static_cast<IceAgent*>(data);
  const auto now = g_get_monotonic_time() / 1000;
  bool keep = false;
  if (!self->destroyed_ && !self->send_disabled_ &&
      self->dtls_peer_verified_ && self->punch_remote_supported_) {
    if (self->punch_runtime_)
      keep = self->punch_runtime_->Tick(now);
    else if (!self->relay_selected_ms_ ||
             now - self->relay_selected_ms_ <
                 self->punch_config_.trigger_delay_ms)
      keep = true;
    else if (now < self->punch_original_deadline_ms_ &&
             nice_agent_get_component_state(self->agent_, self->stream_id_,
                                            1) ==
                 NICE_COMPONENT_STATE_CONNECTED) {
      // An established relay can remain usable during a normal ICE recheck.
      // Wait only within the original window; no probing/opening is permitted
      // until the backend is READY again and validates that window itself.
      keep = true;
    } else {
      self->punch_attempted_ = true;
      const auto deadline =
          nice_agent_punch_get_deadline(self->agent_, self->stream_id_, 1) /
          1000;
      punch::Keys keys;
      punch::Id generation{};
      char selected_ip[NICE_ADDRESS_STRING_LEN] = {};
      uint16_t selected_port = 0;
      NiceCandidate *local = nullptr, *remote = nullptr;
      if (nice_agent_get_selected_pair(self->agent_, self->stream_id_, 1,
                                       &local, &remote) &&
          local) {
        // A srflx/relay address is public. Its base identifies the interface
        // that actually carries the selected connection, including TURN/TCP
        // where the observed UDP base may have a different local port.
        const auto& base = nice_address_is_valid(&local->base_addr)
                               ? local->base_addr
                               : local->addr;
        if (nice_address_is_valid(&base)) {
          nice_address_to_string(&base, selected_ip);
          selected_port = nice_address_get_port(&base);
        }
      }
      std::vector<PunchMappingSnapshot> snapshots;
      {
        std::lock_guard<std::mutex> lock(self->nat_mutex_);
        for (const auto& entry : self->punch_snapshots_)
          snapshots.push_back(entry.second);
      }
      auto chosen = SelectPunchMapping(
          snapshots, selected_ip, selected_port, self->punch_epoch_, now,
          self->punch_config_.mapping_sample_max_age_ms);
      if (deadline > now && chosen && self->ExportPunchKeys(keys, generation)) {
        self->punch_runtime_ = std::make_unique<PunchRuntime>(
            self->agent_, self->stream_id_, self->punch_offer_peer_,
            self->punch_epoch_, self->punch_config_, keys, generation,
            std::move(*chosen), deadline, self->punch_retry_supported_.load());
        keep = self->punch_runtime_->Begin(now);
      }
    }
  }
  if (!keep) {
    if (self->punch_runtime_) self->punch_runtime_->Stop("state-change", false);
    if (self->punch_source_) {
      g_source_unref(self->punch_source_);
      self->punch_source_ = nullptr;
    }
    if (self->punch_packet_handler_) {
      g_signal_handler_disconnect(self->agent_, self->punch_packet_handler_);
      self->punch_packet_handler_ = 0;
    }
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}
void IceAgent::OnPunchPacketStatic(NiceAgent*, guint stream, guint component,
                                   guint64 epoch, guint64 handle,
                                   NiceCandidate* packet, GBytes* data,
                                   gpointer value) {
  auto* self = static_cast<IceAgent*>(value);
  if (!self->punch_runtime_ || self->destroyed_ || self->send_disabled_ ||
      stream != self->stream_id_ || component != 1)
    return;
  gsize size = 0;
  const auto* bytes =
      static_cast<const uint8_t*>(g_bytes_get_data(data, &size));
  self->punch_runtime_->Direct(epoch, handle, packet, bytes, size,
                               g_get_monotonic_time() / 1000);
}

punch::Bytes IceAgent::PunchGenerationContext() const {
  auto* agent = agent_.load();
  if (!agent || destroyed_ || !stream_id_ || punch_remote_ufrag_.empty())
    return {};
  // The backend owns credentials from add_stream onward, including before an
  // answerer generates SDP. Read the current scope for the exporter;
  // never rely on an uninitialized or previous-generation credential cache.
  gchar *local_ufrag = nullptr, *password = nullptr;
  const bool found = nice_agent_get_local_credentials(agent, stream_id_,
                                                      &local_ufrag, &password);
  const std::string local = found && local_ufrag ? local_ufrag : "";
  g_free(local_ufrag);
  g_free(password);
  return punch::GenerationContext(
      punch_offer_peer_ ? local : punch_remote_ufrag_,
      punch_offer_peer_ ? punch_remote_ufrag_ : local);
}

bool IceAgent::ExportPunchKeys(punch::Keys& keys, punch::Id& generation) const {
  keys.Clear();
  generation = {};
  auto* context = gcontext_.load();
  if (!context || !g_main_context_is_owner(context) || !agent_ || destroyed_ ||
      send_disabled_ || !punch_remote_supported_ || !dtls_peer_verified_ || !ssl_)
    return false;
  const auto scope = PunchGenerationContext();
  if (scope.empty()) return false;
  std::array<uint8_t, 128> material{};
  constexpr char label[] = "EXPORTER-MiniRTC-UDP-Punch-v1";
  const int result = SSL_export_keying_material(
      ssl_, material.data(), material.size(), label, sizeof(label) - 1,
      scope.data(), scope.size(), 1);
  if (result == 1) {
    std::copy_n(material.begin(), 32, keys.offer_control.begin());
    std::copy_n(material.begin() + 32, 32, keys.answer_control.begin());
    std::copy_n(material.begin() + 64, 32, keys.offer_probe.begin());
    std::copy_n(material.begin() + 96, 32, keys.answer_probe.begin());
    generation = punch::GenerationId(scope);
  }
  OPENSSL_cleanse(material.data(), material.size());
  return result == 1;
}

bool IceAgent::ExportSrtpKeys(std::vector<uint8_t>& local_key,
                              std::vector<uint8_t>& local_salt,
                              std::vector<uint8_t>& remote_key,
                              std::vector<uint8_t>& remote_salt,
                              bool local_is_client_sender) const {
  if (!dtls_peer_verified_ || !ssl_) {
    LOG_ERROR("DTLS peer not verified");
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

  const size_t total = 2 * (key_len + salt_len);

  std::vector<uint8_t> material(total);
  static const char kLabel[] = "EXTRACTOR-dtls_srtp";
  if (SSL_export_keying_material(ssl_, material.data(), total, kLabel,
                                 sizeof(kLabel) - 1, nullptr, 0, 0) != 1) {
    LOG_ERROR("SSL_export_keying_material failed");
    return false;
  }

  // RFC 5764 section 4.2: client key, server key, client salt, server salt.
  const uint8_t* client_key = material.data();
  const uint8_t* server_key = material.data() + key_len;
  const uint8_t* client_salt = material.data() + 2 * key_len;
  const uint8_t* server_salt = material.data() + 2 * key_len + salt_len;

  // Peers declaring legacy (or v1.5.0 without an attribute) split client
  // key/salt before server key/salt. Both peers must advertise rfc5764 to use it.
  if (!remote_standard_srtp_key_layout_) {
    client_salt = material.data() + key_len;
    server_key = material.data() + key_len + salt_len;
  }

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
