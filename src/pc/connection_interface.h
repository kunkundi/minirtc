/*
 * @Author: DI JUNKUN
 * @Date: 2025-11-05
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CONNECTION_INTERFACE_H_
#define _CONNECTION_INTERFACE_H_

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ice_server_config.h"
#include "minirtc.h"

namespace minirtc {

struct MediaStreamIds {
  std::vector<std::string> video;
  std::vector<std::string> audio;
  std::map<std::string, bool> data;
};

struct ConnectionInfo {
  std::optional<IceServerConfiguration> ice_config;
  std::string transmission_id;
  std::string user_id;
  std::string remote_user_id;

  bool hardware_acceleration;
  bool native_video_output;
  bool trickle_ice;
  bool reliable_ice;
  TurnMode turn_mode;
  bool enable_srtp;
  VideoContentType video_content_type;
  bool av1_encoding;
  VideoQuality video_quality;
  int video_frame_rate;
  VideoDegradationPreference video_degradation_preference;
};

inline bool ConnectionIceConfigFresh(const ConnectionInfo& info) {
  return info.ice_config && info.ice_config->Fresh();
}

struct ConnectionCallbacks {
  OnReceiveVideoFrame on_receive_video_frame;
  OnReceiveBuffer on_receive_audio_buffer;
  OnReceiveBuffer on_receive_data_buffer;
  std::function<void(ConnectionStatus, const char*, size_t, void*)>
      on_connection_status;
  OnNetStatusReport on_net_status_report;
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
  std::string candidate_ufrag;
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

  virtual int SendVideoFrame(const MiniRtcVideoFrame* video_frame,
                             const char* stream_id) = 0;
  virtual int RequestVideoKeyFrame(const char* stream_id) = 0;
  virtual int RequestAllVideoKeyFrames() = 0;
  // Legacy/web transports do not negotiate desktop live video settings.
  virtual int UpdateVideoSettings(VideoQuality, int,
                                  VideoDegradationPreference) {
    return -1;
  }
  virtual int SendAudioFrame(const MiniRtcAudioFrame* audio_frame,
                             const char* stream_id) = 0;
  virtual int SendDataFrame(const char* data, size_t size,
                            const char* stream_id) = 0;
  virtual int SendReliableDataFrame(const char* data, size_t size,
                                    const char* stream_id) = 0;

  virtual void ProcessIceWorkMsg(const IceWorkMsg& msg) = 0;
};
}  // namespace minirtc

#endif
