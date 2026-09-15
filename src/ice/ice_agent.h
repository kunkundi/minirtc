/*
 * @Author: DI JUNKUN
 * @Date: 2025-09-16
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ICE_AGENT_H_
#define _ICE_AGENT_H_

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "gio/gnetworking.h"
#include "glib.h"
#include "ice_server_config.h"
#include "minirtc.h"
#include "nat_traversal.h"
#include "nice/agent.h"
#include "punch_config.h"
#include "punch_protocol.h"
#include "punch_runtime.h"

namespace minirtc {

typedef enum {
  ICE_STATE_DISCONNECTED,
  ICE_STATE_GATHERING,
  ICE_STATE_CONNECTING,
  ICE_STATE_CONNECTED,
  ICE_STATE_READY,
  ICE_STATE_FAILED,
  ICE_STATE_NOT_INITIALIZED,
  ICE_STATE_DESTROYED,
  ICE_STATE_NULLPTR,
  ICE_STATE_LAST
} ICE_STATE;

using nice_cb_state_changed_t = void (*)(NiceAgent* agent, guint stream_id,
                                         guint component_id,
                                         NiceComponentState state,
                                         gpointer data);
using nice_cb_new_candidate_t = void (*)(NiceAgent* agent,
                                         NiceCandidate* candidate, gpointer data);
using nice_cb_new_selected_pair_t = void (*)(NiceAgent* agent, guint stream_id,
                                             guint component_id,
                                             const char* lfoundation,
                                             const char* rfoundation,
                                             gpointer data);
using nice_cb_gathering_done_t = void (*)(NiceAgent* agent, guint stream_id,
                                          gpointer data);
using nice_cb_recv_t = void (*)(NiceAgent* agent, guint stream_id,
                                guint component_id, guint size, gchar* buffer,
                                gpointer data);
using nice_cb_dtls_done_t = void (*)(gpointer data);

typedef struct {
  void* user_ptr_1_;
  void* user_ptr_2_;
} UserPtrSt;

class IceAgent {
 public:
  IceAgent(bool offer_peer, bool use_trickle_ice, bool use_reliable_ice,
           TurnMode turn_mode, bool enable_srtp,
           const IceServerConfiguration& ice_config);
  ~IceAgent();

  int CreateIceAgent(nice_cb_state_changed_t on_state_changed,
                     nice_cb_new_candidate_t on_new_candidate,
                     nice_cb_gathering_done_t on_gathering_done,
                     nice_cb_new_selected_pair_t on_new_selected_pair,
                     nice_cb_recv_t on_recv,
                     nice_cb_dtls_done_t on_cb_dtls_done, void* user_ptr);

  int DestroyIceAgent();
  void StopSending();

  std::string GenerateLocalSdp();
  std::string AppendFingerprintLine(const std::string& sdp);
  std::string GetLocalStreamSdp(uint32_t stream_id);
  int SetRemoteSdp(const std::string& remote_sdp);
  int AddRemoteCandidate(const std::string& candidate_sdp);
  int SetRemoteCandidateGatheringDone();
  int GatherCandidates();
  ICE_STATE GetIceState();

  int Send(const char* data, size_t size);

  int StartDtls(bool is_client);

  bool IsDtlsHandshakeDone() const { return dtls_handshake_done_; }

  bool ExportSrtpKeys(std::vector<uint8_t>& local_key,
                      std::vector<uint8_t>& local_salt,
                      std::vector<uint8_t>& remote_key,
                      std::vector<uint8_t>& remote_salt,
                      bool local_is_client_sender) const;

 public:
  bool use_trickle_ice_ = true;
  bool use_reliable_ice_ = false;
  TurnMode turn_mode_ = TurnMode::TurnDisabled;
  bool enable_srtp_ = true;

  IceServerConfiguration ice_config_;

  bool has_video_stream_ = true;
  uint32_t n_video_streams_ = 1;
  std::string video_stream_sdp_;
  bool has_audio_stream_ = true;
  uint32_t n_audio_streams_ = 1;
  std::string audio_stream_sdp_;
  bool has_data_stream_ = true;
  uint32_t n_data_streams_ = 1;
  std::string data_stream_sdp_;

 public:
  std::thread nice_thread_;
  std::atomic<NiceAgent*> agent_{nullptr};
  std::atomic<GMainContext*> gcontext_{nullptr};
  std::atomic<GMainLoop*> gloop_{nullptr};
  std::atomic<bool> nice_inited_{false};
  std::atomic<bool> init_failed_{false};
  std::mutex init_mutex_;
  std::condition_variable init_cv_;
  bool init_done_ = false;
  int init_status_ = -1;

  gboolean exit_nice_thread_ = false;
  bool controlling_ = false;
  uint32_t stream_id_ = 0;
  uint32_t n_components_ = 1;
  std::string local_sdp_ = "";
  ICE_STATE state_ = ICE_STATE_LAST;
  std::atomic<bool> destroyed_{false};
  std::atomic<bool> agent_closed_{false};
  std::atomic<bool> send_disabled_{false};

  nice_cb_state_changed_t on_state_changed_{};
  nice_cb_new_selected_pair_t on_new_selected_pair_{};
  nice_cb_new_candidate_t on_new_candidate_{};
  nice_cb_gathering_done_t on_gathering_done_{};
  nice_cb_recv_t on_recv_{};
  nice_cb_dtls_done_t on_cb_dtls_done_{};
  void* user_ptr_{};

  UserPtrSt user_prt_st_{};

  FILE* file_in_ = nullptr;
  FILE* file_out_ = nullptr;

 private:
  static void OnStunMappingStatic(NiceAgent* agent, NiceCandidate* sample,
                                  const gchar* server_ip, guint server_port,
                                  guint sequence, gpointer data);
  void ProbePredictedRemoteCandidates();
  std::atomic<bool> p2p_enhancement_enabled_{false};
  std::mutex nat_mutex_;
  std::map<std::string, std::vector<NatMappingSample>> nat_samples_;
  std::mutex prediction_mutex_;
  RemotePortPredictor port_predictor_;

  // dtls
  SSL_CTX* ssl_ctx_ = nullptr;
  SSL* ssl_ = nullptr;
  BIO* bio_ = nullptr;
  EVP_PKEY* dtls_pkey_ = nullptr;
  X509* dtls_cert_ = nullptr;
  std::string dtls_fingerprint_;
  std::atomic<bool> dtls_started_{false};
  std::atomic<bool> dtls_handshake_done_{false};
  std::atomic<bool> dtls_peer_verified_{false};
  std::atomic<bool> remote_standard_srtp_key_layout_{false};
  std::string remote_fingerprint_;
  std::string punch_remote_ufrag_;
  const PunchConfig punch_config_ = ProcessPunchConfig();
  const bool punch_offer_peer_;
  std::atomic<bool> punch_remote_supported_{false};
  std::atomic<bool> punch_negotiated_once_{false};
  std::map<std::string, PunchMappingSnapshot> punch_snapshots_;
  uint64_t punch_epoch_ = 1, punch_snapshot_id_ = 0;
  std::unique_ptr<PunchRuntime> punch_runtime_;
  GSource* punch_source_ = nullptr;
  gulong punch_packet_handler_ = 0;
  std::atomic<int64_t> punch_relay_ready_ms_{0};
  int64_t punch_original_deadline_ms_ = 0;
  bool punch_attempted_ = false;
  // Only the owning Nice/DTLS context can export. No keys before fingerprint
  // verification; output is cleared even on failure.
  bool ExportPunchKeys(punch::Keys& keys, punch::Id& generation) const;
  bool CanAdvertiseUdpPunch() const;

  punch::Bytes PunchGenerationContext() const;
  void MaybeStartPunch();
  void StopPunch();
  static gboolean PunchTickStatic(gpointer data);
  static void OnPunchPacketStatic(NiceAgent*, guint, guint, guint64, guint64,
                                  NiceCandidate*, GBytes*, gpointer);

  static void OnNiceRecvStatic(NiceAgent* agent, guint stream_id,
                               guint component_id, guint size, gchar* buffer,
                               gpointer data);
  static void OnNiceStateChangedStatic(NiceAgent* agent, guint stream_id,
                                       guint component_id,
                                       NiceComponentState state, gpointer data);
  static gboolean CloseNiceAgentStatic(gpointer data);
  static void OnNiceAgentClosedStatic(GObject* source, GAsyncResult* result,
                                      gpointer data);
  void OnNiceStateChanged(guint stream_id, guint component_id,
                          NiceComponentState state);
  void OnNiceRecv(NiceAgent* agent, guint stream_id, guint component_id,
                  guint size, gchar* buffer);

  static bool IsDtlsRecord(const uint8_t* data, size_t len);

  static BIO_METHOD* BIO_s_nice();
  static int bio_nice_write(BIO* b, const char* buf, int len);
  static int bio_nice_read(BIO* b, char* buf, int len);
  static int bio_nice_new(BIO* b);
  static int bio_nice_free(BIO* b);
  static long bio_nice_ctrl(BIO* b, int cmd, long num, void* ptr);

  void GenerateDtlsCertificate(int days_valid = 365 * 30);
  std::string ComputeFingerprint(X509* cert);
  bool ShouldUseDtls() const;

  std::mutex dtls_mutex_;
  std::mutex destroy_mutex_;
  std::mutex send_mutex_;
  std::queue<std::vector<uint8_t>> dtls_incoming_;

  void CleanupDtls();
  bool CompleteDtlsHandshake();
};

}  // namespace minirtc

#endif
