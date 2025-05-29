/*
 * @Author: DI JUNKUN
 * @Date: 2025-02-11
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ICE_TRANSPORT_CONTROLLER_H_
#define _ICE_TRANSPORT_CONTROLLER_H_

#include "api/clock/clock.h"
#include "api/transport/network_types.h"
#include "api/units/timestamp.h"
#include "audio_channel_receive.h"
#include "audio_channel_send.h"
#include "audio_decoder.h"
#include "audio_encoder.h"
#include "bitrate_prober.h"
#include "clock/system_clock.h"
#include "congestion_control.h"
#include "congestion_control_feedback.h"
#include "data_channel_receive.h"
#include "data_channel_send.h"
#include "ice_agent.h"
#include "media_channel.h"
#include "media_codec.h"
#include "paced_sender.h"
#include "resolution_adapter.h"
#include "task_queue.h"
#include "task_queue_lock_free.h"
#include "transport_feedback_adapter.h"
#include "video_channel_receive.h"
#include "video_channel_send.h"
#include "video_decoder_factory.h"
#include "video_encoder_factory.h"

typedef void (*OnReceiveVideo)(const XVideoFrame *, const char *, const size_t,
                               void *);
typedef void (*OnReceiveAudio)(const char *, size_t, const char *, const size_t,
                               void *);
typedef void (*OnReceiveData)(const char *, size_t, const char *, const size_t,
                              void *);

class IceTransportController
    : public std::enable_shared_from_this<IceTransportController>,
      public ThreadBase {
 public:
  IceTransportController(std::shared_ptr<SystemClock> clock,
                         std::shared_ptr<IceAgent> ice_agent,
                         std::shared_ptr<IOStatistics> ice_io_statistics);
  ~IceTransportController();

 public:
  void Create(std::string remote_user_id,
              rtp::PAYLOAD_TYPE video_codec_payload_type,
              bool hardware_acceleration, OnReceiveVideo on_receive_video,
              OnReceiveAudio on_receive_audio, OnReceiveData on_receive_data,
              void *user_data);
  void Destroy();

  uint32_t AddVideoSendChannel(const std::string &channel_name);
  uint32_t AddAudioSendChannel(const std::string &channel_name);
  uint32_t AddDataSendChannel(const std::string &channel_name);

  uint32_t AddVideoReceiveChannel(const std::string &channel_name,
                                  uint32_t ssrc);
  uint32_t AddAudioReceiveChannel(const std::string &channel_name,
                                  uint32_t ssrc);
  uint32_t AddDataReceiveChannel(const std::string &channel_name,
                                 uint32_t ssrc);

  int SendVideo(const XVideoFrame *video_frame,
                const std::string &channel_name);
  int SendAudio(const char *data, size_t size, const std::string &channel_name);
  int SendData(const char *data, size_t size, const std::string &channel_name);

  void FullIntraRequest() { b_force_i_frame_ = true; }

  void UpdateNetworkAvaliablity(bool network_available);

  int OnReceiveVideoRtpPacket(const char *data, size_t size, uint32_t ssrc);
  int OnReceiveAudioRtpPacket(const char *data, size_t size, uint32_t ssrc);
  int OnReceiveDataRtpPacket(const char *data, size_t size, uint32_t ssrc);

  void OnReceiveCompleteFrame(std::unique_ptr<ReceivedFrame> received_frame,
                              const std::string &channel_name);
  void OnReceiveCompleteAudio(const char *data, size_t size,
                              const std::string &channel_name);
  void OnReceiveCompleteData(const char *data, size_t size,
                             const std::string &channel_name);

 public:
  void OnSenderReport(const SenderReport &sender_report);
  void OnReceiverReport(const std::vector<RtcpReportBlock> &report_block_datas);
  void OnCongestionControlFeedback(
      const webrtc::rtcp::CongestionControlFeedback &feedback);
  void OnReceiveNack(const std::vector<uint16_t> &nack_sequence_numbers);

 private:
  int CreateCodecs(std::shared_ptr<SystemClock> clock,
                   rtp::PAYLOAD_TYPE video_pt, bool hardware_acceleration);
  int CreateStreamCodecs(std::shared_ptr<SystemClock> clock,
                         bool hardware_acceleration, bool av1_encoding);

 private:
  void OnSentPacket(const webrtc::RtpPacketToSend &packet);
  void PostUpdates(webrtc::NetworkControlUpdate update);
  void UpdateControlState();
  void UpdateCongestedState();
  std::optional<bool> GetCongestedStateUpdate() const;

 private:
  bool Process() override;

 private:
  enum class StreamType { kAudio, kVideo, kData };
  enum class StreamDirection { kSend, kReceive };

  class StreamContext {
   public:
    std::string name;
    std::optional<uint32_t> ssrc;
    StreamType type;
    StreamDirection direction;

    std::optional<int> target_width;
    std::optional<int> target_height;
    std::optional<int64_t> last_active_time;

    std::shared_ptr<MediaChannel> transceiver;
    std::shared_ptr<MediaCodec> codec;
  };

  bool CheckSteamContext(const std::string &channel_name,
                         const std::shared_ptr<StreamContext> &context);

  std::map<std::string, std::shared_ptr<StreamContext>> stream_senders_;
  std::map<std::string, std::shared_ptr<StreamContext>> stream_receivers_;

  std::map<uint32_t, std::string> ssrc_to_name_;

  OnReceiveVideo on_receive_video_ = nullptr;
  OnReceiveAudio on_receive_audio_ = nullptr;
  OnReceiveData on_receive_data_ = nullptr;

 private:
  std::shared_ptr<IceAgent> ice_agent_ = nullptr;
  std::shared_ptr<IOStatistics> ice_io_statistics_ = nullptr;
  std::unique_ptr<RtpPacketizer> rtp_packetizer_ = nullptr;
  std::shared_ptr<PacedSender> paced_sender_ = nullptr;
  std::string remote_user_id_;
  void *user_data_ = nullptr;
  std::atomic<bool> is_running_;

 private:
  std::shared_ptr<SystemClock> clock_;
  std::shared_ptr<webrtc::Clock> webrtc_clock_ = nullptr;
  webrtc::TransportFeedbackAdapter transport_feedback_adapter_;
  std::unique_ptr<CongestionControl> controller_;
  BitrateProber prober_;
  std::shared_ptr<TaskQueue> task_queue_cc_;
  std::shared_ptr<TaskQueueLockFree> task_queue_encode_;
  std::shared_ptr<TaskQueueLockFree> task_queue_decode_;
  std::shared_ptr<TaskQueueLockFree> task_queue_trans_fb_;
  webrtc::DataSize congestion_window_size_;
  bool is_congested_ = false;
  std::string last_active_stream_;

 private:
  std::unique_ptr<ResolutionAdapter> resolution_adapter_ = nullptr;
  bool b_force_i_frame_;
  bool video_codec_inited_;
  bool load_nvcodec_dll_success_;
  bool hardware_acceleration_;

 private:
  std::unique_ptr<AudioEncoder> audio_encoder_ = nullptr;
  std::unique_ptr<AudioDecoder> audio_decoder_ = nullptr;
  std::map<std::string, std::unique_ptr<AudioEncoder>> audio_encoders_;
  std::map<std::string, std::unique_ptr<AudioDecoder>> audio_decoders_;

  bool audio_codec_inited_ = false;

 private:
  int64_t target_bitrate_ = 0;

  struct LossReport {
    uint32_t extended_highest_sequence_number = 0;
    int cumulative_lost = 0;
  };
  std::map<uint32_t, LossReport> last_report_blocks_;
  webrtc::Timestamp last_report_block_time_;
};

#endif