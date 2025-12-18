/*
 * @Author: DI JUNKUN
 * @Date: 2025-11-05
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DATACHANNEL_CONNECTION_H_
#define _DATACHANNEL_CONNECTION_H_

#include <shared_mutex>
#include <unordered_map>

#include "clock/system_clock.h"
#include "connection_interface.h"
#include "datachannel_transport.h"
#include "rtc/rtc.hpp"
#include "ws_client.h"

namespace minirtc {

class DataChannelConnection : public ConnectionInterface {
 public:
  DataChannelConnection(std::shared_ptr<SystemClock> clock,
                        std::shared_ptr<WsClient> ws,
                        const ConnectionInfo& info,
                        const MediaStreamIds& media_stream_ids,
                        const ConnectionCallbacks& callbacks);
  virtual ~DataChannelConnection();

 public:
  int Init() override;

  int ReleaseAllIceTransmission() override;

  int SendVideoFrame(const XVideoFrame* video_frame,
                     const char* stream_id) override;
  int SendAudioFrame(const char* data, size_t size,
                     const char* stream_id) override;
  int SendDataFrame(const char* data, size_t size,
                    const char* stream_id) override;
  int SendReliableDataFrame(const char* data, size_t size,
                            const char* stream_id) override;

  void ProcessIceWorkMsg(const IceWorkMsg& msg) override;

 private:
  std::shared_ptr<DataChannelTransport> CreateDataChannelConnection(
      const ::rtc::Configuration& config, std::shared_ptr<SystemClock> clock,
      std::weak_ptr<WsClient> wws, bool offer_peer, std::string transmission_id,
      std::string remote_user_id);

  std::shared_ptr<Stream> AddVideo(
      const std::shared_ptr<::rtc::PeerConnection> peer_connection,
      const rtp::PAYLOAD_TYPE payload_type, const uint32_t ssrc,
      const std::string cname, const std::string msid,
      const std::function<void(void)> onOpen);

  std::shared_ptr<Stream> AddAudio(
      const std::shared_ptr<::rtc::PeerConnection> peer_connection,
      const rtp::PAYLOAD_TYPE payload_type, const uint32_t ssrc,
      const std::string cname, const std::string msid,
      const std::function<void(void)> onOpen);

  std::shared_ptr<::rtc::DataChannel> AddData(
      const std::shared_ptr<::rtc::PeerConnection> peer_connection,
      const rtp::PAYLOAD_TYPE payload_type, const uint32_t ssrc,
      const std::string cname, const std::string msid,
      const ConnectionCallbacks& callbacks,
      const std::function<void(void)> onOpen);

  std::shared_ptr<Stream> CreateStream(const std::string h264Samples,
                                       const unsigned fps,
                                       const std::string opusSamples);

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  std::shared_ptr<WsClient> ws_ = nullptr;
  ConnectionInfo info_;
  MediaStreamIds media_stream_ids_;
  ConnectionCallbacks callbacks_;

  std::shared_ptr<DataChannelTransport> dc_transport_ = nullptr;
  std::atomic_bool dc_ready_ = false;

  ::rtc::Configuration peer_connection_config_;

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