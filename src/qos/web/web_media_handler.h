/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-16
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WEB_MEDIA_HANDLER_H_
#define _WEB_MEDIA_HANDLER_H_

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "rtc/mediahandler.hpp"
#include "rtc/rtcpsrreporter.hpp"
#include "web_qos_controller.h"

namespace minirtc {

// One instance per browser connection, shared by all of its video tracks.
// RTCP remains immediate; original media and bounded NACK repairs share the
// same aggregate pacing budget. No callbacks into the transport are retained.
class WebMediaHandler final : public ::rtc::MediaHandler {
 public:
  WebMediaHandler();
  ~WebMediaHandler() override;
  void Stop();
  void RegisterStream(
      uint32_t ssrc, std::shared_ptr<::rtc::RtcpSrReporter> reporter = nullptr);
  bool CanEncode(uint32_t ssrc);
  void SetFrameDeadline(int deadline_ms);
  bool AdmitAudio(size_t payload_size);
  bool TakeKeyFrameRequest(uint32_t ssrc);
  WebQosController::Budget Budget(uint32_t ssrc = 0);
  int QueueDelayMs();
  void ObserveSenderReport(const ::rtc::message_ptr& message);
  bool OnTelemetry(int64_t lost, int64_t expected, int rtt_ms, double jitter_ms,
                   double decode_ms, double fps, bool frozen,
                   int received_bps = 0);
  void incoming(::rtc::message_vector& messages,
                const ::rtc::message_callback& send) override;
  void outgoing(::rtc::message_vector& messages,
                const ::rtc::message_callback& send) override;

 private:
  struct StreamState {
    bool waiting_for_keyframe = true;
    bool request_keyframe = true;
    int64_t last_keyframe_request_ms = -1;
    int64_t last_freeze_request_ms = -1;
    int64_t active_ms = -1;
    bool has_report = false;
    uint32_t reported_sequence = 0;
    int32_t reported_loss = 0;
    double recovery_scale = 1;
    double keyframe_scale = 1;
    int blocked_keyframes = 0;
    int64_t last_keyframe_failure_ms = -1;
    int keyframe_failure_bps = 0;
    int64_t last_recovery_ms = -1;
    std::map<uint32_t, int64_t> sender_reports;
    std::shared_ptr<::rtc::RtcpSrReporter> reporter;
    bool sequence_started = false;
    uint16_t next_sequence = 0;
  };
  struct Packet {
    ::rtc::message_ptr message;
    ::rtc::message_callback send;
    uint32_t ssrc;
    int64_t enqueued_ms;
    int64_t deadline_ms;
    bool repair = false;
  };
  struct CachedPacket {
    Packet packet;
    int64_t sent_ms;
    int64_t last_repair_ms = -1;
    int repairs = 0;
  };
  static int64_t NowMs();
  int QueueDelayLocked(int64_t now_ms) const;
  void DiscardStreamPacketsLocked(uint32_t ssrc);
  void RecoverLocked(uint32_t ssrc, int64_t now_ms, bool oversized,
                     bool keyframe = false);
  void HandleRtcpLocked(const uint8_t* data, size_t size, int64_t now_ms);
  void RequestRepairLocked(uint32_t ssrc, uint16_t sequence, int64_t now_ms);
  void Run();

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopped_ = false;
  bool media_started_ = false;
  int frame_deadline_ms_ = 250;
  WebQosController qos_;
  WebQosController::Budget budget_;
  std::unordered_map<uint32_t, StreamState> streams_;
  std::deque<Packet> packets_;
  size_t queued_bytes_ = 0;
  std::map<uint64_t, CachedPacket> cache_;
  size_t cache_bytes_ = 0;
  double tokens_ = 0;
  double repair_tokens_ = 0;
  double audio_tokens_ = 0;
  std::thread worker_;
};

}  // namespace minirtc

#endif
