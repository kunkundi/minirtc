/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-16
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WEB_QOS_CONTROLLER_H_
#define _WEB_QOS_CONTROLLER_H_

#include <cstddef>
#include <cstdint>

#include "send_side_bandwidth_estimation.h"

namespace minirtc {

// Receiver-side bandwidth estimation (REMB/RR) uses the same QoS rate engine
// as the native transport, without fabricating per-packet arrival times.
// All methods are serialized by the owning media handler.
class WebQosController {
 public:
  static constexpr int kMaxFrameRate = 60;
  static constexpr int kInitialNetworkBps = 3000000;

  struct Budget {
    // Start near the native desktop encoder's 2.5 Mbit/s media budget. A very
    // small startup rate can reject every full-resolution keyframe before the
    // browser receives enough media to estimate the path.
    int network_bps = kInitialNetworkBps;
    int receiver_bps = 0;
    int video_bps = 2454000;
    int audio_bps = 96000;
    int max_fps = 30;
    double decode_scale = 1.0;
    double keyframe_scale = 1.0;
    double loss_ratio = 0;
    int rtt_ms = 0;
    int64_t feedback_age_ms = 0;
  };

  explicit WebQosController(int64_t now_ms);
  void Start(int64_t now_ms);
  void OnPacketSent(size_t bytes);
  void OnKeyFrameBlocked(int64_t now_ms);
  void OnReceiverEstimate(int bitrate_bps, int64_t now_ms);
  void OnReceiverReport(int64_t lost, int64_t expected, int rtt_ms,
                        int64_t now_ms);
  bool OnTelemetry(int64_t lost, int64_t expected, int rtt_ms,
                   double jitter_buffer_ms, double decode_ms, double fps,
                   bool frozen, int64_t now_ms, int received_bps = 0);
  Budget Update(int64_t now_ms, int queue_ms);

 private:
  void UpdateNetworkFeedback(int64_t lost, int64_t expected, int rtt_ms,
                             int64_t now_ms);
  void ResetProbe();
  void BackOff(double factor, int64_t now_ms);
  void ApplyReceiverLimit(int64_t now_ms);
  webrtc::SendSideBandwidthEstimation estimator_;
  Budget budget_;
  int64_t started_ms_;
  int64_t last_feedback_ms_;
  int64_t last_report_ms_ = -1;
  int64_t last_remb_ms_ = -1;
  int64_t last_update_ms_;
  int64_t last_backoff_ms_ = -1;
  int64_t last_pressure_ms_ = -1;
  int64_t last_telemetry_ms_ = -1;
  double previous_jitter_ms_ = 0;
  int64_t jitter_pressure_since_ms_ = -1;
  int64_t rtt_pressure_since_ms_ = -1;
  int min_rtt_ms_ = 0;
  int probe_floor_bps_ = 0;
  int64_t healthy_since_ms_ = -1;
  int64_t last_probe_ms_ = -1;
  int64_t last_decode_progress_ms_ = -1;
  int64_t last_loss_ms_ = -1;
  int64_t queue_pressure_since_ms_ = -1;
  int64_t send_window_ms_ = 0;
  uint64_t sent_bytes_ = 0;
  bool application_limited_ = false;
  int acknowledged_bps_ = 0;
  int64_t last_received_rate_ms_ = -1;
  int64_t last_packet_feedback_ms_ = -1;
  int64_t last_recovery_probe_ms_ = -1;
  int64_t recovery_probe_until_ms_ = -1;
};

}  // namespace minirtc

#endif
