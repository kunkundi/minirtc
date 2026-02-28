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
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "audio_decoder.h"
#include "audio_encoder.h"
#include "clock/system_clock.h"
#include "connection_interface.h"
#include "datachannel_connection.h"
#include "minirtc_connection.h"
#include "video_decoder_factory.h"
#include "video_encoder_factory.h"
#include "ws_client.h"

namespace minirtc {

typedef void (*OnReceiveBuffer)(const char*, size_t, const char*, const size_t,
                                const char*, const size_t, void*);

typedef void (*OnReceiveVideoFrame)(const XVideoFrame* video_frame, const char*,
                                    const size_t, const char*, const size_t,
                                    void*);

typedef void (*OnSignalStatus)(SignalStatus, const char*, const size_t, void*);

typedef void (*OnConnectionStatus)(ConnectionStatus, const char*, const size_t,
                                   void*);

typedef void (*OnNetStatusReport)(const char*, const size_t, TraversalMode,
                                  const XNetTrafficStats*, const char*,
                                  const size_t, void*);

typedef void (*OnSignalMessage)(const char*, size_t, void*);

typedef struct {
  bool use_cfg_file;
  const char* cfg_path;

  const char* signal_server_ip;
  int signal_server_port;
  const char* stun_server_ip;
  int stun_server_port;
  const char* turn_server_ip;
  int turn_server_port;
  const char* turn_server_username;
  const char* turn_server_password;
  bool hardware_acceleration;
  bool av1_encoding;
  bool enable_turn;
  bool enable_srtp;

  VideoQuality video_quality;

  OnReceiveBuffer on_receive_video_buffer;
  OnReceiveBuffer on_receive_audio_buffer;
  OnReceiveBuffer on_receive_data_buffer;

  OnReceiveVideoFrame on_receive_video_frame;

  OnSignalStatus on_signal_status;
  OnSignalMessage on_signal_message;
  OnConnectionStatus on_connection_status;
  OnNetStatusReport on_net_status_report;

  const char* user_id;
  void* user_data;
} PeerConnectionParams;

class PeerConnection {
 public:
  PeerConnection();
  ~PeerConnection();

 public:
  int Init(PeerConnectionParams params);

  int Join(const std::string& transmission_id);

  int Leave(const std::string& transmission_id);

  int AddVideoStream(const char* stream_id);
  int AddAudioStream(const char* stream_id);
  int AddDataStream(const char* stream_id, bool reliable);

  int ReleaseAllIceTransmission();

  int Destroy();

  SignalStatus GetSignalStatus();

  int SendVideoFrame(const XVideoFrame* video_frame, const char* stream_id);
  int SendAudioFrame(const char* data, size_t size, const char* stream_id);
  int SendDataFrame(const char* data, size_t size, const char* stream_id);
  int SendReliableDataFrame(const char* data, size_t size,
                            const char* stream_id);

  int SendVideoFrameToPeer(const XVideoFrame* video_frame,
                           const char* stream_id, const char* remote_peer_id,
                           size_t remote_peer_id_size);
  int SendAudioFrameToPeer(const char* data, size_t size, const char* stream_id,
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

 private:
  int Login();

  void ProcessSignal(const std::string& signal);

 private:
  void StartIceWorker();
  void StopIceWorker();
  void ProcessIceWorkMsg(const IceWorkMsg& msg);
  void PushIceWorkMsg(const IceWorkMsg& msg);

 private:
  std::string uri_ = "";
  std::string cfg_signal_server_ip_;
  std::string cfg_signal_server_port_;
  std::string cfg_stun_server_ip_;
  std::string cfg_stun_server_port_;
  std::string cfg_turn_server_ip_;
  std::string cfg_turn_server_port_;
  std::string cfg_turn_server_username_;
  std::string cfg_turn_server_password_;
  std::string cfg_hardware_acceleration_;
  std::string cfg_av1_encoding_;
  std::string cfg_enable_turn_;
  std::string cfg_enable_srtp_;
  std::string cfg_video_quality_;
  int signal_server_port_ = 0;
  int stun_server_port_ = 0;
  int turn_server_port_ = 0;
  bool hardware_acceleration_ = false;
  bool av1_encoding_ = false;
  bool enable_turn_ = false;
  bool enable_srtp_ = true;
  VideoQuality video_quality_ = VideoQuality::QualityHigh;
  bool trickle_ice_ = true;
  bool reliable_ice_ = false;
  bool try_rejoin_with_turn_ = false;
  int reconnect_count_ = 0;
  TraversalMode mode_ = TraversalMode::P2P;

  std::vector<int> video_payload_types_ = {rtp::PAYLOAD_TYPE::H264,
                                           rtp::PAYLOAD_TYPE::AV1};
  std::vector<int> audio_payload_types_ = {rtp::PAYLOAD_TYPE::OPUS};

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  std::shared_ptr<WsClient> ws_transport_ = nullptr;
  std::function<void(const std::string&)> on_receive_ws_msg_ = nullptr;
  std::function<void(WsStatus)> on_ws_status_ = nullptr;
  unsigned int ws_connection_id_ = 0;
  bool offer_peer_ = false;
  std::string user_id_ = "";
  std::string user_id_with_pwd_ = "";
  std::string remote_user_id_ = "";
  std::string local_transmission_id_ = "";
  std::string remote_transmission_id_ = "";
  std::vector<std::string> user_id_list_;
  WsStatus ws_status_ = WsStatus::WsClosed;
  SignalStatus signal_status_ = SignalStatus::SignalClosed;
  std::mutex signal_status_mutex_;
  std::atomic<bool> leave_{false};
  std::string sdp_without_cands_ = "";

 private:
  std::function<void(std::string, const std::string&)> on_ice_status_change_ =
      nullptr;
  bool ice_ready_ = false;

  OnReceiveBuffer on_receive_video_buffer_ = nullptr;
  OnReceiveBuffer on_receive_audio_buffer_ = nullptr;
  OnReceiveBuffer on_receive_data_buffer_ = nullptr;

  OnReceiveVideoFrame on_receive_video_frame_ = nullptr;

  OnSignalStatus on_signal_status_ = nullptr;
  OnSignalMessage on_signal_message_ = nullptr;
  OnConnectionStatus on_connection_status_ = nullptr;
  OnNetStatusReport on_net_status_report_ = nullptr;
  void* user_data_ = nullptr;

  bool inited_ = false;

 private:
  bool b_force_i_frame_ = false;

 private:
  std::thread ice_worker_;
  std::atomic<bool> ice_worker_running_{true};
  std::queue<IceWorkMsg> ice_work_msg_queue_;
  std::condition_variable ice_work_cv_;
  std::condition_variable empty_notify_cv_;
  std::mutex ice_work_mutex_;
  std::mutex empty_notify_mutex_;

 private:
  MediaStreamIds media_stream_ids_;
  ConnectionInfo connection_info_;
  ConnectionCallbacks connection_callbacks_;
  std::shared_ptr<ConnectionInterface> peer_connection_;

  std::unordered_map<std::string, std::shared_ptr<ConnectionInterface>>
      peer_connection_map_;
  std::shared_mutex peer_connection_map_mutex_;

  std::unordered_set<std::string> internal_signal_types_{
      "login",
      "transmission_id",
      "user_join_transmission",
      "user_leave_transmission",
      "offer",
      "answer",
      "new_candidate",
      "new_candidate_mid"};
};
}  // namespace minirtc

#endif