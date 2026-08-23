#ifndef _RTP_VIDEO_RECEIVER_H_
#define _RTP_VIDEO_RECEIVER_H_

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

#include "api/clock/clock.h"
#include "clock/system_clock.h"
#include "fec_decoder.h"
#include "h264_frame_assember.h"
#include "io_statistics.h"
#include "nack_requester.h"
#include "receive_side_congestion_controller.h"
#include "received_frame.h"
#include "receiver_report.h"
#include "ringbuffer.h"
#include "rtc_base/numerics/sequence_number_util.h"
#include "rtcp_sender.h"
#include "rtp_packet_av1.h"
#include "rtp_packet_h264.h"
#include "rtp_rtcp_defines.h"
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

  void SetSendDataFunc(std::function<int(const char*, size_t)> data_send_func);

  void SetMediaConfig(uint32_t remote_ssrc, uint32_t rtx_ssrc,
                      rtp::PAYLOAD_TYPE media_payload_type) {
    remote_ssrc_.store(remote_ssrc);
    rtx_ssrc_.store(rtx_ssrc != remote_ssrc ? rtx_ssrc : 0);
    media_payload_type_ = media_payload_type;
  }

  void SetOnReceiveCompleteFrame(
      std::function<void(std::unique_ptr<ReceivedFrame>)>
          on_receive_complete_frame) {
    on_receive_complete_frame_ = on_receive_complete_frame;
  }
  uint32_t GetSsrc() { return ssrc_; }
  uint32_t GetRemoteSsrc() { return remote_ssrc_.load(); }

  void StopRtcp();

  void OnSenderReport(const SenderReport& sender_report);
  void OnRttUpdate(int64_t rtt_ms);

  void RequestKeyFrame();

 private:
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
  void TrackPendingFramePacket(uint32_t timestamp, uint16_t sequence_number);
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
  std::pair<int64_t, int64_t> FrameRecoveryDeadlinesMs();
  void SendKeyFrameRequest(bool enter_awaiting_state);
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
  };
  struct RtpTimestampLess {
    bool operator()(uint32_t lhs, uint32_t rhs) const {
      return lhs != rhs && webrtc::AheadOf(rhs, lhs);
    }
  };
  std::map<uint32_t, PendingFrame, RtpTimestampLess> pending_frames_;
  std::mutex pending_frames_mtx_;
  bool awaiting_keyframe_ = false;

 private:
  std::shared_ptr<IOStatistics> io_statistics_ = nullptr;
  uint32_t last_recv_bytes_ = 0;
  uint32_t total_rtp_packets_recv_ = 0;
  uint32_t total_rtp_payload_recv_ = 0;

  std::mutex nack_mtx_;
  std::function<int(const char*, size_t)> data_send_func_ = nullptr;

 private:
  bool fec_enable_ = false;
  FecDecoder fec_decoder_;
  uint64_t last_packet_ts_ = 0;
  // std::map<uint16_t, RtpPacket> incomplete_fec_frame_list_;
  // std::map<uint32_t, std::map<uint16_t, RtpPacket>> fec_source_symbol_list_;
  // std::map<uint32_t, std::map<uint16_t, RtpPacket>> fec_repair_symbol_list_;
  std::set<uint64_t> incomplete_fec_frame_list_;
  std::map<uint64_t, std::map<uint16_t, RtpPacket>> incomplete_fec_packet_list_;
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
  int64_t last_keyframe_request_ms_ = 0;
  std::atomic<bool> is_running_;

 private:
  uint32_t ssrc_ = 0;
  std::atomic<uint32_t> remote_ssrc_{0};
  std::atomic<uint32_t> rtx_ssrc_{0};
  rtp::PAYLOAD_TYPE media_payload_type_ = rtp::PAYLOAD_TYPE::H264;
  std::shared_ptr<webrtc::Clock> clock_;
  ReceiveSideCongestionController receive_side_congestion_controller_;
  RtcpFeedbackSenderInterface* active_remb_module_ = nullptr;

  std::unique_ptr<RtcpSender> rtcp_sender_;
  std::unique_ptr<NackRequester> nack_;

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
  int64_t delta_ntp_internal_ms_;
};
}  // namespace minirtc

#endif
