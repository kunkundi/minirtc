/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-15
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_VIDEO_RECEIVER_H_
#define _RTP_VIDEO_RECEIVER_H_

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "api/clock/clock.h"
#include "clock/system_clock.h"
#include "h264_frame_assember.h"
#include "io_statistics.h"
#include "nack_requester.h"
#include "receive_side_congestion_controller.h"
#include "received_frame.h"
#include "receiver_report.h"
#include "remote_capture_time_estimator.h"
#include "ringbuffer.h"
#include "rtc_base/numerics/sequence_number_util.h"
#include "rtcp_sender.h"
#include "rtp_fec.h"
#include "rtp_packet_av1.h"
#include "rtp_packet_h264.h"
#include "rtp_rtcp_defines.h"
#include "rtp_timestamp.h"
#include "sender_report.h"
#include "thread_base.h"

namespace minirtc {
using namespace webrtc;
class RtpVideoReceiver : public ThreadBase {
 public:
  RtpVideoReceiver(std::shared_ptr<SystemClock> clock);
  RtpVideoReceiver(std::shared_ptr<SystemClock> clock,
                   std::shared_ptr<IOStatistics> io_statistics);
  virtual ~RtpVideoReceiver();

 public:
  void InsertRtpPacket(RtpPacket& rtp_packet);
  void SetFecEnabled(bool enabled);

  void SetSendDataFunc(std::function<int(const char*, size_t)> data_send_func);

  void SetAbsoluteSendTimeExtensionId(
      std::optional<uint8_t> extension_id) {
    abs_send_time_ext_id_ = extension_id;
  }

  void SetLogContext(std::string log_context) {
    log_context_ = std::move(log_context);
  }

  void SetMediaConfig(uint32_t remote_ssrc, uint32_t rtx_ssrc,
                      rtp::PAYLOAD_TYPE media_payload_type) {
    if (remote_ssrc_.load() != remote_ssrc) {
      rtp_timestamp_mapper_.Reset();
    }
    remote_ssrc_.store(remote_ssrc);
    rtx_ssrc_.store(rtx_ssrc != remote_ssrc ? rtx_ssrc : 0);
    media_payload_type_ = media_payload_type;
  }

  void SetOnReceiveCompleteFrame(
      std::function<void(std::unique_ptr<ReceivedFrame>)>
          on_receive_complete_frame) {
    on_receive_complete_frame_ = on_receive_complete_frame;
  }
  // Called only for a complete, decodable frame. Recovery and expiry keep
  // running while the consumer is full.
  void SetFrameAdmissionFunc(std::function<bool(bool is_keyframe)> frame_admission) {
    std::lock_guard<std::mutex> lock(pending_frames_mtx_);
    frame_admission_ = std::move(frame_admission);
  }
  uint32_t GetSsrc() { return ssrc_; }
  uint32_t GetRemoteSsrc() { return remote_ssrc_.load(); }

  void StopRtcp();

  void OnSenderReport(const SenderReport& sender_report);
  void OnRttUpdate(int64_t rtt_ms);
  void OnClockOffset(int64_t offset_us, int64_t rtt_us) {
    rtp_timestamp_mapper_.UpdateClockOffset(
        offset_us, rtt_us, system_clock_->CurrentTimeUs());
  }
  void ResetClockOffset() { rtp_timestamp_mapper_.ResetClockOffset(); }

  void RequestKeyFrame();

 private:
  void InsertMediaPacket(RtpPacket& packet, bool fec_recovered = false);
  void ProcessH265RtpPacket(RtpPacket& packet);
  void ProcessAv1RtpPacket(RtpPacketAv1& rtp_packet_av1);
  bool CheckIsAv1FrameCompleted(RtpPacketAv1& rtp_packet_av1);

 private:
  void ProcessH264RtpPacket(RtpPacketH264& rtp_packet_h264);
  bool CheckIsH264FrameCompleted(RtpPacketH264& rtp_packet_h264, bool is_start,
                                 bool is_end);
  bool PopCompleteFrame(uint16_t start_seq, uint16_t end_seq,
                        uint32_t packet_count, uint32_t timestamp);
  bool RestoreMediaPacketFromRtx(RtpPacket& rtx_packet,
                                 RtpPacket* media_packet);
  void TrackPendingFramePacket(uint32_t timestamp, uint16_t sequence_number,
                               bool is_keyframe);
  bool GetFrameSequenceRange(uint32_t timestamp, const char* codec_name,
                             uint16_t* start_sequence_number,
                             uint16_t* end_sequence_number,
                             uint32_t* packet_count);
  void CommitPendingFrame(uint32_t timestamp,
                          std::unique_ptr<ReceivedFrame> frame,
                          bool is_keyframe, uint16_t end_sequence_number,
                          bool require_existing_entry);
  void ClearFrameMarkers(uint32_t timestamp);
  void EnsureFrameBufferCapacity(size_t required_capacity);
  std::unique_ptr<ReceivedFrame> CreateReceivedFrame(const uint8_t* data,
                                                     size_t size,
                                                     uint32_t timestamp);
  void DropFrameAssembly(uint32_t timestamp);
  bool IsFrameTimestampObsolete(uint32_t timestamp);
  void MarkFrameDiscardedLocked(uint32_t timestamp);
  struct FrameRecoveryTiming {
    int64_t soft_deadline_ms;
    int64_t soft_fir_confirmation_delay_ms;
    int64_t hard_deadline_ms;
  };
  FrameRecoveryTiming GetFrameRecoveryTiming();
  void MaybeLogRecoveryStats(bool force = false);
  void MaybeRetryKeyFrameRequest();
  bool IsReceivingKeyFrame();
  void OnCompleteRecoveryFrame(uint32_t timestamp, bool is_keyframe,
                               int64_t now_ms, uint64_t request_generation);
  struct FailedKeyFrame {
    int64_t arrival_ms;
    uint64_t request_generation;
    uint32_t timestamp;
  };
  bool SendKeyFrameRequest(
      bool enter_awaiting_state,
      std::optional<FailedKeyFrame> failed_keyframe = std::nullopt,
      std::optional<uint32_t> soft_frame_timestamp = std::nullopt);
  bool RtxEnabled() const { return rtx_ssrc_.load() != 0; }

 private:
  void ProcessPendingNacks();
  int SendRtcpRR(ReceiverReport& rtcp_rr);

  void SendCombinedRtcpPacket(
      std::vector<std::unique_ptr<RtcpPacket>> rtcp_packets);

  void SendRemb(int64_t bitrate_bps, std::vector<uint32_t> ssrcs);

 private:
  bool Process() override;
  void RtcpThread();

 private:
  bool SendNack(const std::vector<uint16_t>& nack_list);
  void SendPreparedNackBatch(std::vector<uint16_t> nack_batch);

  void SendRR();

  void ReviseFrequencyAndJitter(int payload_type_frequency);

 private:
  std::map<uint16_t, RtpPacketH264> incomplete_h264_frame_list_;
  std::map<uint16_t, RtpPacket> incomplete_h265_packets_;
  std::map<uint16_t, RtpPacketAv1> incomplete_av1_frame_list_;
  std::map<uint16_t, RtpPacket> incomplete_frame_list_;
  uint8_t* nv12_data_ = nullptr;
  size_t frame_buffer_capacity_ = 0;
  std::function<void(std::unique_ptr<ReceivedFrame>)>
      on_receive_complete_frame_ = nullptr;
  std::optional<uint32_t> last_complete_frame_ts_;
  RingBuffer<ReceivedFrame> compelete_video_frame_queue_;
  std::mutex frame_assembly_mtx_;

 private:
  struct PendingFrame {
    std::unique_ptr<ReceivedFrame> frame;
    bool is_complete = false;
    int64_t arrival_time = 0;
    bool is_keyframe = false;
    std::optional<uint16_t> last_sequence_number;
    bool recovery_escalated = false;
    bool soft_fir_requested = false;
    int64_t last_progress_time = 0;
    uint64_t request_generation = 0;
    std::optional<int64_t> admission_blocked_since_ms;
  };
  struct RtpTimestampLess {
    bool operator()(uint32_t lhs, uint32_t rhs) const {
      return lhs != rhs && webrtc::AheadOf(rhs, lhs);
    }
  };
  std::map<uint32_t, PendingFrame, RtpTimestampLess> pending_frames_;
  std::mutex pending_frames_mtx_;
  std::function<bool(bool is_keyframe)> frame_admission_;
  bool awaiting_keyframe_ = false;
  std::optional<uint32_t> last_discarded_frame_ts_;

 private:
  std::shared_ptr<IOStatistics> io_statistics_ = nullptr;
  uint32_t last_recv_bytes_ = 0;
  uint32_t total_rtp_packets_recv_ = 0;
  uint32_t total_rtp_payload_recv_ = 0;

  std::mutex nack_mtx_;
  std::function<int(const char*, size_t)> data_send_func_ = nullptr;

 private:
  std::mutex fec_mtx_;
  std::unique_ptr<RtpFecReceiver> fec_receiver_;
  std::unordered_map<uint64_t, uint16_t> fua_end_sequence_numbers_;
  std::unordered_map<uint64_t, uint16_t> fua_start_sequence_numbers_;
  H264FrameAssembler h264_frame_assembler_;

 private:
  std::thread rtcp_thread_;
  std::mutex rtcp_mtx_;
  std::mutex rtcp_sender_mtx_;
  std::condition_variable rtcp_cv_;
  std::chrono::steady_clock::time_point last_send_rtcp_rr_ts_;
  std::atomic<bool> rtcp_stop_ = false;
  int rtcp_rr_interval_ms_ = 5000;
  int rtcp_scheduler_interval_ms_ = 200;
  std::atomic<bool> keyframe_request_pending_{false};
  std::atomic<int64_t> keyframe_request_started_ms_{0};
  std::atomic<int64_t> next_keyframe_request_ms_{0};
  // A repaired delta ends its recovery event, but the FIR it already caused
  // can still produce a keyframe. Track that attempt independently. Publish
  // generations under pending_frames_mtx_ before sending, including failures,
  // so frames first observed in the same millisecond cannot match an old FIR.
  std::atomic<uint64_t> keyframe_request_generation_{0};
  std::atomic<bool> keyframe_request_in_flight_{false};
  // Protected by rtcp_sender_mtx_. This is a local receive-order association,
  // not a sender acknowledgement of an FIR sequence number.
  std::optional<uint32_t> keyframe_request_timestamp_floor_;
  // Protected by rtcp_sender_mtx_. A failure must belong to the latest FIR
  // before it can shorten that request's retry interval.
  int64_t last_keyframe_request_ms_ = 0;
  // Also protected by rtcp_sender_mtx_. A soft FIR does not abandon the
  // decoder's reference chain; repairing this frame can end its event.
  std::optional<uint32_t> soft_keyframe_request_timestamp_;
  int64_t last_recovery_stats_log_ms_ = 0;
  // Latest measured RTT (ms) mirrored out of NackRequester so the FIR retry
  // interval can be computed without taking nack_mtx_ under rtcp_sender_mtx_.
  // 0 means no sample yet.
  std::atomic<int64_t> recovery_rtt_ms_{0};
  std::atomic<bool> is_running_;

 private:
  uint32_t ssrc_ = 0;
  std::atomic<uint32_t> remote_ssrc_{0};
  std::atomic<uint32_t> rtx_ssrc_{0};
  rtp::PAYLOAD_TYPE media_payload_type_ = rtp::PAYLOAD_TYPE::H264;
  std::string log_context_ = "video";
  std::optional<uint8_t> abs_send_time_ext_id_;
  RemoteCaptureTimeEstimator rtp_timestamp_mapper_;
  std::shared_ptr<SystemClock> system_clock_;
  std::shared_ptr<webrtc::Clock> clock_;
  ReceiveSideCongestionController receive_side_congestion_controller_;
  RtcpFeedbackSenderInterface* active_remb_module_ = nullptr;

  std::unique_ptr<RtcpSender> rtcp_sender_;
  std::unique_ptr<NackRequester> nack_;

  std::atomic<uint64_t> recovery_completed_frames_{0};
  std::atomic<uint64_t> recovery_discarded_frames_{0};
  std::atomic<uint64_t> recovery_soft_stalls_{0};
  std::atomic<uint64_t> recovery_hard_timeouts_{0};
  // Ticks where a completed frame was held because the decode consumer was
  // behind. Non-zero means the viewer is decode-bound, not network-bound.
  std::atomic<uint64_t> recovery_admission_stalls_{0};
  std::atomic<uint64_t> recovery_obsolete_packets_{0};
  std::atomic<uint64_t> recovery_accepted_rtx_packets_{0};
  std::atomic<uint64_t> recovery_late_rtx_packets_{0};
  std::atomic<uint64_t> recovery_fir_sent_{0};
  std::atomic<uint64_t> recovery_fir_suppressed_{0};
  std::atomic<uint64_t> recovery_fir_failed_{0};
  std::atomic<uint64_t> recovery_soft_fir_sent_{0};
  std::atomic<uint64_t> recovery_soft_fir_avoided_{0};
  std::atomic<uint64_t> recovery_soft_fir_repaired_{0};
  std::atomic<uint64_t> recovery_soft_fir_repair_total_ms_{0};
  std::atomic<uint64_t> recovery_soft_fir_repair_max_ms_{0};
  std::atomic<uint64_t> recovery_fir_responses_{0};
  std::atomic<uint64_t> recovery_fir_response_total_ms_{0};
  std::atomic<uint64_t> recovery_fir_response_max_ms_{0};

  uint8_t fraction_lost_ = 0;
  int32_t cumulative_lost_ = 0;
  uint32_t jitter_ = 0;
  uint16_t extended_high_seq_num_ = 0;
  uint32_t last_sr_ = 0;

  int32_t cumulative_loss_ = 0;
  int32_t last_report_cumulative_loss_ = 0;
  int32_t cumulative_loss_rtcp_offset_ = 0;
  std::optional<Timestamp> last_receive_time_;
  int last_payload_type_frequency_ = 0;
  uint16_t last_extended_high_seq_num_ = 0;
  uint32_t jitter_q4_ = 0;
  uint32_t last_received_timestamp_ = 0;
  std::mutex receiver_stats_mtx_;
  std::atomic<bool> has_received_media_packet_{false};

  uint32_t remote_ssrc = 0;
  uint32_t last_remote_ntp_timestamp = 0;
  uint32_t last_remote_rtp_timestamp = 0;
  uint32_t last_arrival_timestamp = 0;
  uint32_t last_arrival_ntp_timestamp = 0;
  uint32_t packets_sent = 0;
  uint32_t bytes_sent = 0;
  uint32_t reports_count = 0;

  uint8_t sequence_number_fir_ = 0;

 private:
  FILE* file_rtp_recv_ = nullptr;
};
}  // namespace minirtc

#endif
