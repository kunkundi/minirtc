/*
 * @Author: DI JUNKUN
 * @Date: 2024-11-26
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _PEER_CONNECTION_H_
#define _PEER_CONNECTION_H_

#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "nlohmann/json.hpp"

#include "audio_decoder.h"
#include "audio_encoder.h"
#include "clock/system_clock.h"
#include "connection_interface.h"
#include "datachannel_connection.h"
#include "minirtc_connection.h"
#include "video_decoder_factory.h"
#include "video_encoder_factory.h"
#include "ws_client.h"
#include "reconnect_login.h"

namespace minirtc {

typedef void (*MiniRtcOnReceiveBuffer)(const char*, size_t, const char*, const size_t,
                                const char*, const size_t, void*);

typedef void (*MiniRtcOnReceiveVideoFrame)(const MiniRtcVideoFrame* video_frame, const char*,
                                    const size_t, const char*, const size_t,
                                    void*);

typedef void (*MiniRtcOnSignalStatus)(MiniRtcSignalStatus, const char*, const size_t, void*);

typedef void (*MiniRtcOnConnectionStatus)(MiniRtcConnectionStatus, const char*, const size_t,
                                   void*);

typedef void (*MiniRtcOnNetStatusReport)(const char*, const size_t, MiniRtcTraversalMode,
                                  const MiniRtcNetTrafficStats*, const char*,
                                  const size_t, void*);

typedef void (*MiniRtcOnSignalMessage)(const char*, size_t, void*);

typedef struct {
  bool use_cfg_file;
  const char* cfg_path;

  const char* signal_server_ip;
  int signal_server_port;
  bool hardware_acceleration;
  bool native_video_output;
  bool av1_encoding;
  MiniRtcTurnMode turn_mode;
  bool enable_srtp;
  MiniRtcVideoContentType video_content_type;

  MiniRtcVideoQuality video_quality;
  uint32_t video_frame_rate;
  MiniRtcVideoDegradationPreference video_degradation_preference;

  MiniRtcOnReceiveBuffer on_receive_video_buffer;
  MiniRtcOnReceiveBuffer on_receive_audio_buffer;
  MiniRtcOnReceiveBuffer on_receive_data_buffer;

  MiniRtcOnReceiveVideoFrame on_receive_video_frame;

  MiniRtcOnSignalStatus on_signal_status;
  MiniRtcOnSignalMessage on_signal_message;
  MiniRtcOnConnectionStatus on_connection_status;
  MiniRtcOnNetStatusReport on_net_status_report;

  const char* user_id;
  void* user_data;
  MiniRtcOnSignalExchange on_signal_exchange;
  MiniRtcOnReceiveEncodedVideo on_receive_encoded_video;
} PeerConnectionParams;

class PeerConnection {
 public:
  PeerConnection();
  ~PeerConnection();

 public:
  int Init(PeerConnectionParams params);
  int InitDirect(PeerConnectionParams params,
                 const MiniRtcDirectOptions& options);
  int StartDirect(bool offerer);
  int ReceiveDirect(const char* message, size_t size);
  int CloseDirect();
  void ReportDirectError(MiniRtcDirectError error);
  int UpdateConnectionSettings(bool hardware_acceleration, bool av1_encoding,
                                MiniRtcTurnMode turn_mode);

  int Join(const std::string& transmission_id);

  int Leave(const std::string& transmission_id);
  int DisconnectPeer(const std::string& transmission_id,
                      const std::string& remote_user_id);

  int AddVideoStream(const char* stream_id);
  int AddAudioStream(const char* stream_id);
  int AddDataStream(const char* stream_id, bool reliable);

  int ReleaseAllIceTransmission();

  int Destroy();

  MiniRtcSignalStatus GetSignalStatus();

  int SendVideoFrame(const MiniRtcVideoFrame* video_frame, const char* stream_id);
  int RequestVideoKeyFrame(const char* stream_id);
  int RequestAllVideoKeyFrames();
  int UpdateVideoBitrateLimit(const std::string& remote_id, int max_bitrate_bps);
  int SetVideoSendPaused(const std::string& remote_id, bool paused);
  int SetAudioSendPaused(const std::string& remote_id, bool paused);
  int UpdateVideoResolution(const std::string& remote_id, int width, int height);
  int UpdateVideoSettings(const std::string& remote_id, MiniRtcVideoQuality quality,
                          int frame_rate,
                          MiniRtcVideoDegradationPreference preference);
  int SendAudioFrame(const MiniRtcAudioFrame* audio_frame, const char* stream_id);
  int SendDataFrame(const char* data, size_t size, const char* stream_id);
  int SendReliableDataFrame(const char* data, size_t size,
                            const char* stream_id);

  int SendVideoFrameToPeer(const MiniRtcVideoFrame* video_frame,
                           const char* stream_id, const char* remote_peer_id,
                           size_t remote_peer_id_size);
  int SendAudioFrameToPeer(const MiniRtcAudioFrame* audio_frame,
                           const char* stream_id,
                           const char* remote_peer_id,
                           size_t remote_peer_id_size);
  int SendDataFrameToPeer(const char* data, size_t size, const char* stream_id,
                          const char* remote_peer_id,
                          size_t remote_peer_id_size);
  int SendReliableDataFrameToPeer(const char* data, size_t size,
                                  const char* stream_id,
                                  const char* remote_peer_id,
                                  size_t remote_peer_id_size);

  int64_t GetSystemTimeMicros();

  int SendSignalMessage(const char* message, size_t size);

  int ForceSignalReconnect();

 private:
  int Login();
  int ExchangeSignal(MiniRtcSignalExchangeStage stage, const std::string& message);
  int SendJoinRequestLocked(const std::string& transmission_id);
  void SetSignalStatus(MiniRtcSignalStatus signal_status);

  void ProcessSignal(const std::string& signal);
  bool ApplyTurnCredentials(const nlohmann::json& message);
  bool BuildConnectionInfo(const nlohmann::json& message,
                           const std::string& transmission_id,
                           const std::string& remote_user_id,
                           ConnectionInfo& info);
  bool IsTerminalConnectionStatus(MiniRtcConnectionStatus status) const;
  std::shared_ptr<ConnectionInterface> CreateManagedPeerConnection(
      const std::string& remote_user_id, const ConnectionInfo& info);
  bool RetirePeerConnection(
      const std::string& remote_user_id,
      const std::shared_ptr<ConnectionInterface>& connection,
      MiniRtcConnectionStatus status);
  bool IsCurrentPeerConnection(
      const std::string& remote_user_id,
      const std::shared_ptr<ConnectionInterface>& connection);
  std::shared_ptr<ConnectionInterface> ReplaceOrCreatePeerConnection(
      const std::string& remote_user_id, const char* context,
      const ConnectionInfo& info);
  void ClearPeerConnections(const char* reason);
  void ReportSessions();

 private:
  void StartIceWorker();
  void StopIceWorker();
  void ProcessIceWorkMsg(const IceWorkMsg& msg);
  void PushIceWorkMsg(const IceWorkMsg& msg);

 private:
  std::string uri_ = "";
  std::string cfg_signal_server_ip_;
  std::string cfg_signal_server_port_;
  std::optional<IceServerConfiguration> legacy_turn_config_;
  bool per_connection_ice_config_ = false;
  std::string cfg_hardware_acceleration_;
  std::string cfg_native_video_output_;
  std::string cfg_av1_encoding_;
  std::string cfg_turn_mode_;
  std::string cfg_enable_turn_;
  std::string cfg_enable_srtp_;
  std::string cfg_video_content_type_;
  std::string cfg_video_quality_;
  std::string cfg_video_frame_rate_;
  std::string cfg_video_degradation_preference_;
  int signal_server_port_ = 0;
  bool hardware_acceleration_ = false;
  bool native_video_output_ = false;
  bool av1_encoding_ = false;
  MiniRtcTurnMode turn_mode_ = MiniRtcTurnMode::MiniRtcTurnDisabled;
  bool enable_srtp_ = true;
  MiniRtcVideoContentType video_content_type_ = MiniRtcVideoContentType::ScreenContent;
  MiniRtcVideoQuality video_quality_ = MiniRtcVideoQuality::MiniRtcVideoQualityHigh;
  int video_frame_rate_ = 60;
  MiniRtcVideoDegradationPreference video_degradation_preference_ =
      MiniRtcVideoDegradationPreference::MaintainResolution;
  bool trickle_ice_ = true;
  bool reliable_ice_ = false;
  bool try_rejoin_with_turn_ = false;
  int reconnect_count_ = 0;
  MiniRtcTraversalMode mode_ = MiniRtcTraversalMode::MiniRtcTraversalP2P;

  std::vector<int> video_payload_types_ = {rtp::PAYLOAD_TYPE::H264,
                                           rtp::PAYLOAD_TYPE::AV1};
  std::vector<int> audio_payload_types_ = {rtp::PAYLOAD_TYPE::OPUS};

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  std::shared_ptr<WsClient> ws_transport_ = nullptr;
  std::shared_ptr<SignalSender> signal_sender_;
  std::function<void(const std::string&)> on_receive_ws_msg_ = nullptr;
  std::function<void(WsStatus)> on_ws_status_ = nullptr;
  unsigned int ws_connection_id_ = 0;
  bool offer_peer_ = false;
  std::string user_id_ = "";
  std::string user_id_with_pwd_ = "";
  ReconnectLogin reconnect_login_;
  std::string remote_user_id_ = "";
  std::string local_transmission_id_ = "";
  std::string remote_transmission_id_ = "";
  std::vector<std::string> user_id_list_;
  WsStatus ws_status_ = WsStatus::WsClosed;
  MiniRtcSignalStatus signal_status_ = MiniRtcSignalStatus::MiniRtcSignalClosed;
  std::mutex signal_status_mutex_;
  std::optional<std::string> pending_join_transmission_id_;
  std::atomic<bool> leave_{false};
  std::string sdp_without_cands_ = "";

 private:
  std::function<void(std::string, const std::string&)> on_ice_status_change_ =
      nullptr;
  bool ice_ready_ = false;

  MiniRtcOnReceiveBuffer on_receive_video_buffer_ = nullptr;
  MiniRtcOnReceiveBuffer on_receive_audio_buffer_ = nullptr;
  MiniRtcOnReceiveBuffer on_receive_data_buffer_ = nullptr;

  MiniRtcOnReceiveVideoFrame on_receive_video_frame_ = nullptr;

  MiniRtcOnSignalStatus on_signal_status_ = nullptr;
  MiniRtcOnSignalMessage on_signal_message_ = nullptr;
  MiniRtcOnSignalExchange on_signal_exchange_ = nullptr;
  MiniRtcOnConnectionStatus on_connection_status_ = nullptr;
  MiniRtcOnNetStatusReport on_net_status_report_ = nullptr;
  void* user_data_ = nullptr;

  bool inited_ = false;
  bool direct_ = false;
  bool direct_offerer_ = false;
  bool direct_started_ = false;
  bool direct_remote_sdp_ = false;
  bool direct_remote_end_ = false;
  size_t direct_received_bytes_ = 0;
  size_t direct_received_messages_ = 0;
  std::string direct_remote_ufrag_;
  std::set<std::string> direct_candidates_;
  std::mutex direct_mutex_;
  std::atomic<bool> direct_closing_{false};
  std::atomic<bool> direct_error_reported_{false};
  std::atomic<bool> direct_terminal_reported_{false};
  void (*direct_error_)(MiniRtcDirectError, void*) = nullptr;
  void* direct_context_ = nullptr;

 private:
  bool b_force_i_frame_ = false;

 private:
  std::thread ice_worker_;
  std::atomic<bool> ice_worker_running_{true};
  std::queue<IceWorkMsg> ice_work_msg_queue_;
  std::queue<std::shared_ptr<ConnectionInterface>>
      terminal_connection_cleanup_queue_;
  std::condition_variable ice_work_cv_;
  std::condition_variable empty_notify_cv_;
  std::mutex ice_work_mutex_;
  std::mutex empty_notify_mutex_;

 private:
  MediaStreamIds media_stream_ids_;
  std::mutex connection_info_mutex_;
  ConnectionInfo connection_info_;
  ConnectionCallbacks connection_callbacks_;
  std::shared_ptr<ConnectionInterface> peer_connection_;

  std::unordered_map<std::string, std::shared_ptr<ConnectionInterface>>
      peer_connection_map_;
  std::shared_mutex peer_connection_map_mutex_;
  std::unordered_map<std::string, nlohmann::json> session_tickets_;
  std::unordered_set<std::string> connected_peers_;

  std::unordered_set<std::string> internal_signal_types_{
      "session_ticket", "session_resumed",
      "login",
      "transmission_id",
      "user_join_transmission",
      "user_leave_transmission",
      "offer",
      "answer",
      "new_candidate",
      "new_candidate_mid",
      "turn_credentials"};
};
}  // namespace minirtc

#endif
