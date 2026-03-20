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
#include "nice/agent.h"

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
using nice_cb_new_candidate_t = void (*)(NiceAgent* agent, guint stream_id,
                                         guint component_id, gchar* foundation,
                                         gpointer data);
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
           bool enable_turn, bool force_turn, bool enable_srtp,
           std::string& stun_ip, uint16_t stun_port, std::string& turn_ip,
           uint16_t turn_port, std::string& turn_username,
           std::string& turn_password);
  ~IceAgent();

  int CreateIceAgent(nice_cb_state_changed_t on_state_changed,
                     nice_cb_new_candidate_t on_new_candidate,
                     nice_cb_gathering_done_t on_gathering_done,
                     nice_cb_new_selected_pair_t on_new_selected_pair,
                     nice_cb_recv_t on_recv,
                     nice_cb_dtls_done_t on_cb_dtls_done, void* user_ptr);

  int DestroyIceAgent();

  std::string GenerateLocalSdp();
  std::string AppendFingerprintLine(const std::string& sdp);
  std::string GetLocalStreamSdp(uint32_t stream_id);
  int SetRemoteSdp(const std::string& remote_sdp);
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
  bool enable_turn_ = false;
  bool force_turn_ = false;
  bool enable_srtp_ = true;

  std::string stun_ip_ = "";
  uint16_t stun_port_ = 0;
  std::string turn_ip_ = "";
  uint16_t turn_port_ = 0;
  std::string turn_username_ = "";
  std::string turn_password_ = "";

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
  std::atomic<GMainLoop*> gloop_{nullptr};
  std::atomic<bool> nice_inited_{false};
  std::atomic<bool> init_failed_{false};
  std::mutex init_mutex_;
  std::condition_variable init_cv_;
  bool init_done_ = false;
  int init_status_ = -1;

  gboolean exit_nice_thread_ = false;
  bool controlling_ = false;
  gchar* ice_ufrag_ = nullptr;
  gchar* ice_password_ = nullptr;
  uint32_t stream_id_ = 0;
  uint32_t n_components_ = 1;
  std::string local_sdp_ = "";
  ICE_STATE state_ = ICE_STATE_LAST;
  bool destroyed_ = false;
  gboolean agent_closed_ = false;

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
  // dtls
  SSL_CTX* ssl_ctx_ = nullptr;
  SSL* ssl_ = nullptr;
  BIO* bio_ = nullptr;
  EVP_PKEY* dtls_pkey_ = nullptr;
  X509* dtls_cert_ = nullptr;
  std::string dtls_fingerprint_;
  std::atomic<bool> dtls_started_{false};
  std::atomic<bool> dtls_handshake_done_{false};
  std::string remote_fingerprint_;

  static void OnNiceRecvStatic(NiceAgent* agent, guint stream_id,
                               guint component_id, guint size, gchar* buffer,
                               gpointer data);
  static void OnNiceStateChangedStatic(NiceAgent* agent, guint stream_id,
                                       guint component_id,
                                       NiceComponentState state, gpointer data);
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
  std::string ExtractAndStripFingerprint(const std::string& sdp);

  std::mutex dtls_mutex_;
  std::queue<std::vector<uint8_t>> dtls_incoming_;

  void CleanupDtls();
};

}  // namespace minirtc

#endif