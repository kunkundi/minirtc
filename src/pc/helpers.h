/*
 * @Author: DI JUNKUN
 * @Date: 2025-10-29
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _HELPERS_H_
#define _HELPERS_H_

#include <shared_mutex>

#include "rtc/rtc.hpp"

struct ClientTrackData {
  std::shared_ptr<rtc::Track> track;
  std::shared_ptr<rtc::RtcpSrReporter> sender;

  ClientTrackData(std::shared_ptr<rtc::Track> track,
                  std::shared_ptr<rtc::RtcpSrReporter> sender);
};

struct Client {
  enum class State { Waiting, WaitingForVideo, WaitingForAudio, Ready };
  const std::shared_ptr<rtc::PeerConnection>& peerConnection = _peerConnection;
  Client(std::shared_ptr<rtc::PeerConnection> pc) { _peerConnection = pc; }
  std::optional<std::shared_ptr<ClientTrackData>> video;
  std::optional<std::shared_ptr<ClientTrackData>> audio;
  std::optional<std::shared_ptr<rtc::DataChannel>> dataChannel;

  void setState(State state);
  State getState();

  uint32_t rtpStartTimestamp = 0;

 private:
  std::shared_mutex _mutex;
  State state = State::Waiting;
  std::string id;
  std::shared_ptr<rtc::PeerConnection> _peerConnection;
};

struct ClientTrack {
  std::string id;
  std::shared_ptr<ClientTrackData> trackData;
  ClientTrack(std::string id, std::shared_ptr<ClientTrackData> trackData);
};

uint64_t currentTimeInMicroSeconds();

#endif