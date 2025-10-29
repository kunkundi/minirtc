/*
 * @Author: DI JUNKUN
 * @Date: 2025-10-29
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DATACHANNEL_TRANSPORT_H_
#define _DATACHANNEL_TRANSPORT_H_

#include <memory>
#include <optional>
#include <shared_mutex>

#include "rtc/rtc.hpp"
#include "rtc/websocket.hpp"

namespace minirtc {

class DataChannelTransport {
 public:
  enum class State { Waiting, WaitingForVideo, WaitingForAudio, Ready };

  struct Stream {
    std::shared_ptr<::rtc::Track> track;
    std::shared_ptr<::rtc::RtcpSrReporter> sender;

    Stream(std::shared_ptr<::rtc::Track> track,
           std::shared_ptr<::rtc::RtcpSrReporter> sender);
  };

 public:
  DataChannelTransport(std::shared_ptr<::rtc::PeerConnection> peer_connection);
  ~DataChannelTransport();

 public:
  void setState(State state) { state_ = state; }
  State getState() { return state_; }

  std::shared_ptr<Stream> video_stream_;
  std::shared_ptr<Stream> audio_stream_;

  std::shared_ptr<::rtc::DataChannel> dataChannel_;

  uint32_t rtpStartTimestamp = 0;

 private:
  std::shared_mutex mutex_;
  State state_ = State::Waiting;
  std::string id_;
  std::shared_ptr<rtc::PeerConnection> peer_connection_;
};
}  // namespace minirtc
#endif