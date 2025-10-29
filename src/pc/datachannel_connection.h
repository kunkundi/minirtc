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

#include "audio_decoder.h"
#include "audio_encoder.h"
#include "clock/system_clock.h"
#include "datachannel_transport.h"
#include "ice_transport.h"
#include "minirtc.h"
#include "rtc/rtc.hpp"
#include "rtc/websocket.hpp"
#include "video_decoder_factory.h"
#include "video_encoder_factory.h"
#include "ws_client.h"

namespace minirtc {

typedef void (*OnReceiveBuffer)(const char*, size_t, const char*, const size_t,
                                void*);

typedef void (*OnReceiveVideoFrame)(const XVideoFrame* video_frame, const char*,
                                    const size_t, void*);

typedef void (*OnSignalStatus)(SignalStatus, const char*, const size_t, void*);

typedef void (*OnConnectionStatus)(ConnectionStatus, const char*, const size_t,
                                   void*);

typedef void (*NetStatusReport)(const char*, const size_t, TraversalMode,
                                const XNetTrafficStats*, const char*,
                                const size_t, void*);

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
  const char* tls_cert_path;
  bool hardware_acceleration;
  bool av1_encoding;
  bool enable_turn;
  bool enable_srtp;

  OnReceiveBuffer on_receive_video_buffer;
  OnReceiveBuffer on_receive_audio_buffer;
  OnReceiveBuffer on_receive_data_buffer;

  OnReceiveVideoFrame on_receive_video_frame;

  OnSignalStatus on_signal_status;
  OnConnectionStatus on_connection_status;
  NetStatusReport net_status_report;

  const char* user_id;
  void* user_data;
} PeerConnectionParams;

struct IceWorkMsg {
  enum Type {
    Login = 0,
    UserIdList,
    UserLeaveTransmission,
    Offer,
    Answer,
    NewCandidate,
    RetryWithTurn
  };

  Type type;
  std::vector<std::string> user_id_list;
  std::string user_id;
  std::string transmission_id;
  std::string remote_user_id;
  std::string new_candidate;
  std::string remote_sdp;
};

class DataChannelConnection {
 public:
  DataChannelConnection();
  ~DataChannelConnection();

 public:
  int Init(PeerConnectionParams params);

  int Join(const std::string& transmission_id);

  int Leave(const std::string& transmission_id);

  int AddVideoStream(const char* stream_id);
  int AddAudioStream(const char* stream_id);
  int AddDataStream(const char* stream_id);

  int ReleaseAllIceTransmission();

  int Destroy();

  SignalStatus GetSignalStatus();

  int SendVideoFrame(const XVideoFrame* video_frame, const char* stream_id);
  int SendAudioFrame(const char* data, size_t size, const char* stream_id);
  int SendDataFrame(const char* data, size_t size, const char* stream_id);

  int64_t GetSystemTimeMicros();

 private:
  int Login();

  void ProcessSignal(const std::string& signal);

  int RequestTransmissionMemberList(const std::string& transmission_id);

  int NegotiationFailed();

 private:
  void StartIceWorker();
  void StopIceWorker();
  void ProcessIceWorkMsg(const IceWorkMsg& msg);
  void PushIceWorkMsg(const IceWorkMsg& msg);

 private:
  std::shared_ptr<DataChannelTransport> CreateDataChannelConnection(
      const ::rtc::Configuration& config, std::weak_ptr<::rtc::WebSocket> wws,
      std::string id);

  std::shared_ptr<Stream> AddVideo(
      const std::shared_ptr<::rtc::PeerConnection> pc,
      const uint8_t payloadType, const uint32_t ssrc, const std::string cname,
      const std::string msid, const std::function<void(void)> onOpen);

  std::shared_ptr<Stream> AddAudio(
      const std::shared_ptr<::rtc::PeerConnection> pc,
      const uint8_t payloadType, const uint32_t ssrc, const std::string cname,
      const std::string msid, const std::function<void(void)> onOpen);

  std::shared_ptr<Stream> CreateStream(const std::string h264Samples,
                                       const unsigned fps,
                                       const std::string opusSamples);

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
  std::string cfg_tls_cert_path_;
  std::string cfg_hardware_acceleration_;
  std::string cfg_av1_encoding_;
  std::string cfg_enable_turn_;
  std::string cfg_enable_srtp_;
  int signal_server_port_ = 0;
  int stun_server_port_ = 0;
  int turn_server_port_ = 0;
  bool hardware_acceleration_ = false;
  bool av1_encoding_ = false;
  bool enable_turn_ = false;
  bool enable_srtp_ = true;
  bool trickle_ice_ = true;
  bool reliable_ice_ = false;
  bool try_rejoin_with_turn_ = false;
  int reconnect_count_ = 0;
  TraversalMode mode_ = TraversalMode::P2P;

  std::vector<int> video_payload_types_ = {rtp::PAYLOAD_TYPE::H264,
                                           rtp::PAYLOAD_TYPE::AV1};
  std::vector<int> audio_payload_types_ = {rtp::PAYLOAD_TYPE::OPUS};

  std::vector<std::string> video_stream_ids_;
  std::vector<std::string> audio_stream_ids_;
  std::vector<std::string> data_stream_ids_;

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  std::shared_ptr<::rtc::WebSocket> ws_transport_ = nullptr;
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
  std::map<std::string, std::shared_ptr<IceTransport>> ice_transport_list_;
  std::map<std::string, bool> is_ice_transport_ready_;
  std::shared_mutex ice_transport_list_mutex_;

  std::function<void(std::string, const std::string&)> on_ice_status_change_ =
      nullptr;
  bool ice_ready_ = false;

  OnReceiveBuffer on_receive_video_buffer_;
  OnReceiveBuffer on_receive_audio_buffer_;
  OnReceiveBuffer on_receive_data_buffer_;

  OnReceiveVideoFrame on_receive_video_frame_;

  OnSignalStatus on_signal_status_;
  OnConnectionStatus on_connection_status_;
  NetStatusReport net_status_report_;
  void* user_data_;

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
  std::unordered_map<std::string, std::shared_ptr<DataChannelTransport>>
      dc_transport_list_{};
  std::optional<std::shared_ptr<Stream>> avStream = std::nullopt;
};
}  // namespace minirtc

#endif