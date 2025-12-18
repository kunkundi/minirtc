/*
 * @Author: DI JUNKUN
 * @Date: 2025-11-05
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CONNECTION_INTERFACE_H_
#define _CONNECTION_INTERFACE_H_

#include <string>
#include <vector>

#include "minirtc.h"

namespace minirtc {

struct MediaStreamIds {
  std::vector<std::string> video;
  std::vector<std::string> audio;
  std::vector<std::string> data;
};

struct ConnectionInfo {
  std::string stun_server_ip;
  int stun_server_port;
  std::string turn_server_ip;
  int turn_server_port;
  std::string turn_server_username;
  std::string turn_server_password;
  std::string transmission_id;
  std::string user_id;
  std::string remote_user_id;

  bool hardware_acceleration;
  bool trickle_ice;
  bool reliable_ice;
  bool enable_turn;
  bool enable_srtp;
  bool av1_encoding;
  VideoQuality video_quality;
};

struct ConnectionCallbacks {
  OnReceiveVideoFrame on_receive_video_frame;
  OnReceiveBuffer on_receive_audio_buffer;
  OnReceiveBuffer on_receive_data_buffer;
  OnConnectionStatus on_connection_status;
  NetStatusReport net_status_report;
  void* user_data = nullptr;
};

struct IceWorkMsg {
  enum Type {
    Login = 0,
    UserLeaveTransmission,
    UserJoinTransmission,
    Offer,
    Answer,
    NewCandidate,
    NewCandidateMid,
    RetryWithTurn
  };

  Type type;
  std::vector<std::string> user_id_list;
  std::string user_id;
  std::string transmission_id;
  std::string remote_user_id;
  std::string new_candidate;
  std::string candidate;
  std::string mid;
  std::string remote_sdp;
};

class ConnectionInterface {
 public:
  ConnectionInterface() {}
  virtual ~ConnectionInterface() {}

  virtual int Init() = 0;

  virtual int ReleaseAllIceTransmission() = 0;

  virtual int SendVideoFrame(const XVideoFrame* video_frame,
                             const char* stream_id) = 0;
  virtual int SendAudioFrame(const char* data, size_t size,
                             const char* stream_id) = 0;
  virtual int SendDataFrame(const char* data, size_t size,
                            const char* stream_id) = 0;
  virtual int SendReliableDataFrame(const char* data, size_t size,
                                    const char* stream_id) = 0;

  virtual void ProcessIceWorkMsg(const IceWorkMsg& msg) = 0;
};
}  // namespace minirtc

#endif