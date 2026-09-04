/*
 * @Author: DI JUNKUN
 * @Date: 2025-11-05
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _MINIRTC_CONNECTION_H_
#define _MINIRTC_CONNECTION_H_

#include <shared_mutex>
#include <unordered_map>

#include "clock/system_clock.h"
#include "connection_interface.h"
#include "ice_transport.h"
#include "ws_client.h"

namespace minirtc {

class MiniRtcConnection : public ConnectionInterface {
 public:
  MiniRtcConnection(std::shared_ptr<SystemClock> clock,
                    std::shared_ptr<WsClient> ws, const ConnectionInfo& info,
                    const MediaStreamIds& media_stream_ids,
                    const ConnectionCallbacks& callbacks);
  virtual ~MiniRtcConnection();

 public:
  int Init() override;

  int ReleaseAllIceTransmission() override;

  int SendVideoFrame(const MiniRtcVideoFrame* video_frame,
                     const char* stream_id) override;
  int RequestVideoKeyFrame(const char* stream_id) override;
  int RequestAllVideoKeyFrames() override;
  int SendAudioFrame(const MiniRtcAudioFrame* audio_frame,
                     const char* stream_id) override;
  int SendDataFrame(const char* data, size_t size,
                    const char* stream_id) override;
  int SendReliableDataFrame(const char* data, size_t size,
                            const char* stream_id) override;

  void ProcessIceWorkMsg(const IceWorkMsg& msg) override;

 private:
 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  std::shared_ptr<WsClient> ws_ = nullptr;
  ConnectionInfo info_;
  MediaStreamIds media_stream_ids_;
  ConnectionCallbacks callbacks_;

  std::shared_ptr<IceTransport> ice_transport_;
  std::atomic_bool is_ice_transport_ready_;

  std::function<void(std::string, const std::string&)> on_ice_status_change_;
  void* user_data_;

  bool b_force_i_frame_ = false;
  bool try_rejoin_with_turn_ = false;
  int reconnect_count_ = 0;

  std::vector<int> video_payload_types_ = {rtp::PAYLOAD_TYPE::H264,
                                           rtp::PAYLOAD_TYPE::AV1};
  std::vector<int> audio_payload_types_ = {rtp::PAYLOAD_TYPE::OPUS};

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
};
}  // namespace minirtc

#endif
