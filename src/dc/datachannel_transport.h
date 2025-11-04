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
#include <unordered_map>

#include "clock/system_clock.h"
#include "media_channel.h"
#include "media_codec.h"
#include "minirtc.h"
#include "rtc/rtc.hpp"
#include "rtc/websocket.hpp"
#include "task_queue.h"
#include "task_queue_lock_free.h"
#include "video_decoder_factory.h"
#include "video_encoder_factory.h"

namespace minirtc {

class Stream {
 public:
  Stream(std::shared_ptr<::rtc::Track> track,
         std::shared_ptr<::rtc::RtcpSrReporter> sender);

  std::shared_ptr<::rtc::Track> track_;
  std::shared_ptr<::rtc::RtcpSrReporter> sender_;
  std::shared_ptr<MediaCodec> codec_;
};

class DataChannelTransport
    : public std::enable_shared_from_this<DataChannelTransport> {
 public:
  enum class State { Waiting, WaitingForVideo, WaitingForAudio, Ready };

 public:
  DataChannelTransport(std::shared_ptr<SystemClock> clock,
                       std::shared_ptr<::rtc::PeerConnection> peer_connection,
                       std::string local_id, std::string remote_id,
                       bool offer_peer);
  ~DataChannelTransport();

 public:
  int SendVideoFrame(const XVideoFrame* video_frame,
                     const std::string& stream_id);

  int SendAudioFrame(const char* data, size_t size,
                     const std::string& stream_id);

  int SendDataFrame(const char* data, size_t size,
                    const std::string& stream_id);

 public:
  void setState(State state) { state_ = state; }
  State getState() { return state_; }

  std::unordered_map<std::string, std::shared_ptr<Stream>> video_streams_;
  std::unordered_map<std::string, std::shared_ptr<Stream>> audio_streams_;
  std::unordered_map<std::string, std::shared_ptr<::rtc::DataChannel>>
      data_streams_;

  uint32_t rtpStartTimestamp = 0;

  std::shared_mutex mutex_;
  State state_ = State::Waiting;
  std::string id_;
  std::shared_ptr<::rtc::PeerConnection> peer_connection_;

 private:
  std::string local_id_;
  std::string remote_id_;
  bool offer_peer_;

 private:
  std::shared_ptr<SystemClock> clock_;
  std::shared_ptr<TaskQueueLockFree> task_queue_encode_;
  std::shared_ptr<TaskQueueLockFree> task_queue_decode_;
};
}  // namespace minirtc
#endif