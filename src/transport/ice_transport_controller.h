/*
 * @Author: DI JUNKUN
 * @Date: 2025-02-11
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ICE_TRANSPORT_CONTROLLER_H_
#define _ICE_TRANSPORT_CONTROLLER_H_
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

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
#include "srtp_engine.h"
#include "task_queue.h"
#include "task_queue_lock_free.h"
#include "transport_feedback_adapter.h"
#include "video_channel_receive.h"
#include "video_channel_send.h"
#include "video_decoder_factory.h"
#include "video_encoder_factory.h"

typedef void (*OnReceiveVideo)(const MiniRtcVideoFrame*, const char*, const size_t,
                               const char*, const size_t, void*);
typedef void (*OnReceiveAudio)(const char*, size_t, const char*, const size_t,
                               const char*, const size_t, void*);
typedef void (*OnReceiveData)(const char*, size_t, const char*, const size_t,
                              const char*, const size_t, void*);

namespace minirtc {
class ResolutionAdapter;

class IceTransportController
    : public std::enable_shared_from_this<IceTransportController>,
      public ThreadBase {
 public:
  IceTransportController(std::shared_ptr<SystemClock> clock,
                         std::shared_ptr<IceAgent> ice_agent,
                         std::shared_ptr<IOStatistics> ice_io_statistics,
                         bool enable_srtp, VideoQuality video_quality,
                         int video_frame_rate,
                         VideoContentType video_content_type,
                         VideoDegradationPreference
                             video_degradation_preference);
  ~IceTransportController();

 public:
  void Create(bool offer_peer, std::string remote_user_id,
              rtp::PAYLOAD_TYPE video_codec_payload_type,
              bool video_rtx_enabled, bool hardware_acceleration,
              bool native_video_output,
              OnReceiveVideo on_receive_video, OnReceiveAudio on_receive_audio,
              OnReceiveData on_receive_data, void* user_data);
  void Destroy();

  // SRTP is negotiated from the local setting and the remote SDP before
  // Create() starts the media pipelines. Keep the controller in sync when the
  // remote peer does not advertise a DTLS fingerprint.
  void SetSrtpEnabled(bool enable_srtp) { enable_srtp_ = enable_srtp; }

  uint32_t AddVideoSendChannel(const std::string& channel_name);
  uint32_t GetVideoRtxSsrc(const std::string& channel_name);
  uint32_t AddAudioSendChannel(const std::string& channel_name);
  uint32_t AddDataSendChannel(const std::string& channel_name, bool reliable);

  uint32_t AddVideoReceiveChannel(const std::string& channel_name,
                                  uint32_t ssrc, uint32_t rtx_ssrc = 0);
  uint32_t AddAudioReceiveChannel(const std::string& channel_name,
                                  uint32_t ssrc);
  uint32_t AddDataReceiveChannel(const std::string& channel_name, uint32_t ssrc,
                                 bool reliable = false);

  int SendVideo(const MiniRtcVideoFrame* video_frame,
                const std::string& channel_name);
  int SendAudio(const MiniRtcAudioFrame* audio_frame,
                const std::string& channel_name);
  int SendData(const char* data, size_t size, const std::string& channel_name);
  int SendReliableData(const char* data, size_t size,
                       const std::string& channel_name);

  void FullIntraRequest();
  void FullIntraRequest(const std::string& channel_name) {
    if (channel_name.empty()) {
      FullIntraRequest();
      return;
    }
    std::lock_guard<std::mutex> lock(force_i_frame_streams_mutex_);
    force_i_frame_streams_.insert(channel_name);
  }
  void FullIntraRequest(uint32_t media_ssrc);
  void FullIntraRequestAllVideoStreams();

  void UpdateNetworkAvaliablity(bool network_available);

  bool DecryptIncomingPacket(uint8_t* buffer, int* size, uint32_t* out_ssrc);

  int OnReceiveVideoRtpPacket(const char* data, size_t size, uint32_t ssrc);
  int OnReceiveAudioRtpPacket(const char* data, size_t size, uint32_t ssrc);
  int OnReceiveDataRtpPacket(const char* data, size_t size, uint32_t ssrc);
  int OnReceiveDataAckRtpPacket(const char* data, size_t size, uint32_t ssrc,
                                const std::string& channel_name);

  void OnReceiveCompleteFrame(std::unique_ptr<ReceivedFrame> received_frame,
                              const std::string& channel_name);
  void OnReceiveCompleteAudio(const char* data, size_t size,
                              const std::string& channel_name);
  void OnReceiveCompleteData(const char* data, size_t size,
                             const std::string& channel_name);

  void OnDtlsHandshakeDone(void* user_ptr);

 public:
  void OnSenderReport(const SenderReport& sender_report);
  void OnReceiverReport(const std::vector<RtcpReportBlock>& report_block_datas);
  void OnCongestionControlFeedback(
      const webrtc::rtcp::CongestionControlFeedback& feedback);
  void OnReceiveNack(uint32_t media_ssrc,
                     const std::vector<uint16_t>& nack_sequence_numbers);

 private:
  int CreateCodecs(std::shared_ptr<SystemClock> clock,
                   rtp::PAYLOAD_TYPE video_pt, bool hardware_acceleration);
  int CreateStreamCodecs(std::shared_ptr<SystemClock> clock,
                         bool hardware_acceleration,
                         VideoCodecType codec_type);

 private:
  struct PacketFeedbackRegistration {
    int64_t send_time_ms = 0;
    bool tracked = false;
  };

  PacketFeedbackRegistration RegisterPacketForFeedback(
      const webrtc::RtpPacketToSend& packet,
      const webrtc::PacedPacketInfo& pacing_info);
  void RollbackPacketFeedback(
      const webrtc::RtpPacketToSend& packet,
      const PacketFeedbackRegistration& registration);
  void OnSentPacket(const webrtc::RtpPacketToSend& packet,
                    const PacketFeedbackRegistration& registration);
  void PostUpdates(webrtc::NetworkControlUpdate update);
  void UpdateVideoBitrateAllocation();
  void UpdateControlState();
  void UpdateCongestedState();
  bool CanProbeWithoutMedia();
  void UpdateMediaTransportState();
  std::optional<bool> GetCongestedStateUpdate() const;
  void MaybeDegradeResolutionOnEncodeTime(const std::string& channel_name,
                                          int queue_delay_ms,
                                          const EncodedFrame& encoded_frame);

 private:
  bool Process() override;

 private:
  enum class StreamType { kAudio, kVideo, kData };
  enum class StreamDirection { kSend, kReceive };

  class StreamContext {
   public:
    std::string name;
    std::optional<uint32_t> ssrc;
    std::optional<uint32_t> rtx_ssrc;
    StreamType type;
    StreamDirection direction;

    std::optional<int> target_width;
    std::optional<int> target_height;
    int source_width = 0;   // original capture width  (aspect-ratio anchor)
    int source_height = 0;  // original capture height (aspect-ratio anchor)
    std::optional<int64_t> last_active_time;
    std::optional<int> desired_target_bitrate;
    std::optional<int> applied_target_bitrate;
    bool bitrate_update_queued = false;
    int encode_exceed_count = 0;
    int encode_below_threshold_count = 0;
    bool encoding_speed_priority_enabled = false;
    std::optional<int> mapped_target_width;
    std::optional<int> mapped_target_height;
    bool freeze_resolution = false;
    bool static_content_candidate = false;
    bool static_content_candidate_initialized = false;
    int64_t static_content_candidate_since_ms = 0;
    int64_t source_resolution_initialized_ms = 0;
    std::optional<int> pending_mapped_width;
    std::optional<int> pending_mapped_height;
    int mapping_stability_count = 0;
    int64_t pending_mapped_since_ms = 0;
    int64_t last_resolution_change_ms = 0;
    bool resolution_upgrade_probe_active = false;
    int resolution_upgrade_probe_base_width = 0;
    int resolution_upgrade_probe_base_height = 0;
    int resolution_upgrade_probe_target_width = 0;
    int resolution_upgrade_probe_target_height = 0;
    int resolution_upgrade_probe_sample_count = 0;
    int64_t resolution_upgrade_probe_started_ms = 0;
    int resolution_upgrade_probe_failure_count = 0;
    int64_t next_resolution_upgrade_probe_ms = 0;
    int64_t encoded_frame_rate_window_started_ms = 0;
    int encoded_frame_rate_window_frame_count = 0;
    int measured_encoded_frame_rate = 0;
    bool encoded_frame_rate_ready = false;
    int64_t encoded_frame_rate_healthy_since_ms = 0;
    std::array<int, 3> encoded_frame_rate_windows{};
    size_t encoded_frame_rate_valid_window_count = 0;
    std::atomic<uint64_t> capture_input_frame_total{0};
    std::atomic<uint64_t> pacer_rejected_frame_total{0};
    std::atomic<uint64_t> encode_queue_dropped_frame_total{0};
    int64_t frame_admission_window_started_ms = 0;
    uint64_t frame_admission_window_capture_start = 0;
    uint64_t frame_admission_window_pacer_rejected_start = 0;
    uint64_t frame_admission_window_encode_queue_dropped_start = 0;
    uint64_t frame_admission_capture_samples = 0;
    uint64_t frame_admission_pacer_rejected_samples = 0;
    uint64_t frame_admission_encode_queue_dropped_samples = 0;
    int measured_capture_input_frame_rate = 0;
    int measured_pacer_rejection_percent = 0;
    int measured_encode_queue_drop_percent = 0;
    bool frame_admission_metrics_ready = false;
    int64_t low_capture_frame_rate_since_ms = 0;
    int64_t high_pacer_rejection_since_ms = 0;
    int64_t high_encode_queue_drop_since_ms = 0;
    EncoderQualityStats last_encoder_quality_stats;
    float normalized_qp_ewma = -1.0f;
    std::deque<std::pair<int64_t, int>> encode_queue_delay_samples;
    int64_t encode_queue_delay_tracking_started_ms = 0;
    bool encode_queue_delay_window_ready = false;
    int average_encode_queue_delay_ms = 0;
    int p95_encode_queue_delay_ms = 0;
    int64_t encode_backlog_since_ms = 0;
    int64_t severe_encode_backlog_since_ms = 0;
    int64_t critical_encode_backlog_since_ms = 0;
    int64_t post_upgrade_protection_until_ms = 0;

    void ClearResolutionUpgradeProbe() {
      resolution_upgrade_probe_active = false;
      resolution_upgrade_probe_base_width = 0;
      resolution_upgrade_probe_base_height = 0;
      resolution_upgrade_probe_target_width = 0;
      resolution_upgrade_probe_target_height = 0;
      resolution_upgrade_probe_sample_count = 0;
      resolution_upgrade_probe_started_ms = 0;
    }

    void ResetResolutionUpgradeProbe() {
      ClearResolutionUpgradeProbe();
      resolution_upgrade_probe_failure_count = 0;
      next_resolution_upgrade_probe_ms = 0;
    }

    void ResetFrameAdmissionTracking() {
      frame_admission_window_started_ms = 0;
      frame_admission_window_capture_start =
          capture_input_frame_total.load(std::memory_order_acquire);
      frame_admission_window_pacer_rejected_start =
          pacer_rejected_frame_total.load(std::memory_order_acquire);
      frame_admission_window_encode_queue_dropped_start =
          encode_queue_dropped_frame_total.load(std::memory_order_acquire);
      frame_admission_capture_samples = 0;
      frame_admission_pacer_rejected_samples = 0;
      frame_admission_encode_queue_dropped_samples = 0;
      measured_capture_input_frame_rate = 0;
      measured_pacer_rejection_percent = 0;
      measured_encode_queue_drop_percent = 0;
      frame_admission_metrics_ready = false;
      low_capture_frame_rate_since_ms = 0;
      high_pacer_rejection_since_ms = 0;
      high_encode_queue_drop_since_ms = 0;
    }

    void ResetEncodedFrameRateTracking() {
      encoded_frame_rate_window_started_ms = 0;
      encoded_frame_rate_window_frame_count = 0;
      measured_encoded_frame_rate = 0;
      encoded_frame_rate_ready = false;
      encoded_frame_rate_healthy_since_ms = 0;
      encoded_frame_rate_windows.fill(0);
      encoded_frame_rate_valid_window_count = 0;
      ResetFrameAdmissionTracking();
    }

    void ResetEncoderQualityTracking() {
      last_encoder_quality_stats = {};
      normalized_qp_ewma = -1.0f;
    }

    void ResetEncodeQueueDelayTracking() {
      encode_queue_delay_samples.clear();
      encode_queue_delay_tracking_started_ms = 0;
      encode_queue_delay_window_ready = false;
      average_encode_queue_delay_ms = 0;
      p95_encode_queue_delay_ms = 0;
      encode_backlog_since_ms = 0;
      severe_encode_backlog_since_ms = 0;
      critical_encode_backlog_since_ms = 0;
    }

    std::shared_ptr<MediaChannel> transceiver;
    std::shared_ptr<MediaCodec> codec;

    bool reliable;
  };

  void PostVideoBitrateUpdate(
      const std::string& channel_name,
      const std::shared_ptr<StreamContext>& context,
      const std::shared_ptr<MediaCodec>& codec);
  void ApplyVideoBitrateUpdateOnEncodeQueue(
      const std::string& channel_name,
      const std::shared_ptr<StreamContext>& context,
      const std::shared_ptr<MediaCodec>& codec);

  bool CheckSteamContext(const std::string& channel_name,
                         const std::shared_ptr<StreamContext>& context);
  int OnVideoEncoded(const std::string& channel_name,
                     const std::shared_ptr<StreamContext>& context,
                     int queue_delay_ms, bool measure_encode_delay,
                     const EncodedFrame& encoded_frame);

  std::map<std::string, std::shared_ptr<StreamContext>> stream_senders_;
  std::map<std::string, std::shared_ptr<StreamContext>> stream_receivers_;
  std::shared_mutex stream_senders_mutex_;
  std::shared_mutex stream_receivers_mutex_;

  std::map<uint32_t, std::shared_ptr<SrtpEngine::SrtpSession>>
      ssrc_to_srtp_sender_;
  std::map<uint32_t, std::shared_ptr<SrtpEngine::SrtpSession>>
      ssrc_to_srtp_receiver_;

  std::map<uint32_t, std::string> ssrc_to_name_;

  MediaCodecConfig media_config_;

  OnReceiveVideo on_receive_video_ = nullptr;
  OnReceiveAudio on_receive_audio_ = nullptr;
  OnReceiveData on_receive_data_ = nullptr;

 private:
  std::shared_ptr<IceAgent> ice_agent_ = nullptr;
  std::shared_ptr<IOStatistics> ice_io_statistics_ = nullptr;
  std::unique_ptr<RtpPacketizer> rtp_packetizer_ = nullptr;
  std::shared_ptr<PacedSender> paced_sender_ = nullptr;
  std::string remote_user_id_;
  bool offer_peer_ = false;
  void* user_data_ = nullptr;
  std::atomic<bool> is_running_;

  bool enable_srtp_;
  bool video_rtx_enabled_ = false;
  std::atomic<bool> ice_ready_{false};
  std::atomic<bool> dtls_ready_{false};
  std::atomic<bool> media_transport_ready_{false};
  VideoQuality video_quality_;

  std::vector<uint8_t> local_key_;
  std::vector<uint8_t> local_salt_;
  std::vector<uint8_t> remote_key_;
  std::vector<uint8_t> remote_salt_;

 private:
  std::shared_ptr<SystemClock> clock_;
  std::shared_ptr<webrtc::Clock> webrtc_clock_ = nullptr;
  webrtc::TransportFeedbackAdapter transport_feedback_adapter_;
  mutable std::mutex transport_feedback_adapter_mutex_;
  std::unique_ptr<CongestionControl> controller_;
  BitrateProber prober_;
  std::shared_ptr<TaskQueue> task_queue_cc_;
  std::shared_ptr<TaskQueue> task_queue_pacer_;
  std::shared_ptr<TaskQueueLockFree> task_queue_encode_;
  std::shared_ptr<TaskQueueLockFree> task_queue_decode_;
  std::shared_ptr<TaskQueueLockFree> task_queue_trans_fb_;
  webrtc::DataSize congestion_window_size_;
  bool is_congested_ = false;
  std::string last_active_stream_;

 private:
  std::unique_ptr<ResolutionAdapter> resolution_adapter_;
  std::atomic<bool> b_force_i_frame_;
  std::mutex force_i_frame_streams_mutex_;
  std::unordered_set<std::string> force_i_frame_streams_;
  bool video_codec_inited_;
  bool hardware_acceleration_;
  bool native_video_output_;

 private:
  std::unique_ptr<AudioEncoder> audio_encoder_ = nullptr;
  std::unique_ptr<AudioDecoder> audio_decoder_ = nullptr;
  std::map<std::string, std::unique_ptr<AudioEncoder>> audio_encoders_;
  std::map<std::string, std::unique_ptr<AudioDecoder>> audio_decoders_;

  bool audio_codec_inited_ = false;

 private:
  int64_t target_bitrate_ = 0;
  int64_t available_transport_bitrate_ = 0;

  struct LossReport {
    uint32_t extended_highest_sequence_number = 0;
    int cumulative_lost = 0;
  };
  std::map<uint32_t, LossReport> last_report_blocks_;
  webrtc::Timestamp last_report_block_time_;
};
}  // namespace minirtc

#endif
