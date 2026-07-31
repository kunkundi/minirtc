/*
 * @Author: DI JUNKUN
 * @Date: 2025-10-29
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DATACHANNEL_TRANSPORT_H_
#define _DATACHANNEL_TRANSPORT_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "audio_decoder.h"
#include "audio_encoder.h"
#include "clock/system_clock.h"
#include "media_channel.h"
#include "media_codec.h"
#include "minirtc.h"
#include "resolution_adapter.h"
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
  std::mutex audio_encode_mutex_;
  uint64_t audio_timestamp_us_ = 0;
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

  void Shutdown();

 public:
  int SendVideoFrame(const XVideoFrame* video_frame,
                     const std::string& stream_id);

  void RequestVideoKeyFrame(const std::string& stream_id) {
    if (stream_id.empty()) {
      b_force_i_frame_ = true;
      return;
    }
    std::lock_guard<std::mutex> lock(force_i_frame_streams_mutex_);
    force_i_frame_streams_.insert(stream_id);
  }
  void RequestAllVideoKeyFrames();

  int SendAudioFrame(const char* data, size_t size,
                     const std::string& stream_id);

  int SendDataFrame(const char* data, size_t size,
                    const std::string& stream_id);

  void AddVideoStream(std::string stream_id, std::shared_ptr<Stream> stream);

  void AddAudioStream(std::string stream_id, std::shared_ptr<Stream> stream);

  void AddDataStream(std::string stream_id,
                     std::shared_ptr<::rtc::DataChannel> stream);

  int CreateCodecs(std::shared_ptr<SystemClock> clock,
                   rtp::PAYLOAD_TYPE video_pt, bool hardware_acceleration);

 public:
  void setState(State state) { state_ = state; }

  State getState() { return state_; }

  const std::shared_ptr<::rtc::PeerConnection>& GetPeerConnection() const {
    return peer_connection_;
  }

 private:
  int CreateStreamCodecs(std::shared_ptr<SystemClock> clock,
                         bool hardware_acceleration, bool av1_encoding);

 private:
  std::unordered_map<std::string, std::shared_ptr<Stream>> video_streams_;
  std::unordered_map<std::string, std::shared_ptr<Stream>> audio_streams_;
  std::unordered_map<std::string, std::shared_ptr<::rtc::DataChannel>>
      data_streams_;

  std::shared_mutex video_streams_mutex_;
  std::shared_mutex audio_streams_mutex_;
  std::shared_mutex data_streams_mutex_;

  MediaCodecConfig media_config_;

 private:
  std::unique_ptr<ResolutionAdapter> resolution_adapter_ = nullptr;
  std::atomic<bool> b_force_i_frame_;
  std::mutex force_i_frame_streams_mutex_;
  std::unordered_set<std::string> force_i_frame_streams_;
  bool video_codec_inited_;
  bool audio_codec_inited_;
  bool load_nvcodec_dll_success_;
  bool hardware_acceleration_;

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
  std::atomic<bool> shutdown_{false};
};
}  // namespace minirtc
#endif
