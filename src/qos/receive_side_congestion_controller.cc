/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "receive_side_congestion_controller.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

#include "api/media_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "log.h"
// #include "remote_bitrate_estimator_single_stream.h"
#include "remote_bitrate_estimator_abs_send_time.h"
#include "rtp_packet_received.h"

namespace minirtc {
namespace webrtc {

namespace {
static const uint32_t kTimeOffsetSwitchThreshold = 30;
}  // namespace

void ReceiveSideCongestionController::OnRttUpdate(int64_t avg_rtt_ms,
                                                  int64_t max_rtt_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  rbe_->OnRttUpdate(avg_rtt_ms, max_rtt_ms);
}

void ReceiveSideCongestionController::RemoveStream(uint32_t ssrc) {
  std::lock_guard<std::mutex> lock(mutex_);
  rbe_->RemoveStream(ssrc);
}

DataRate ReceiveSideCongestionController::LatestReceiveSideEstimate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rbe_->LatestEstimate();
}

void ReceiveSideCongestionController::PickEstimator() {
  // When we don't see AST, wait for a few packets before going back to TOF.
  if (using_absolute_send_time_) {
    ++packets_since_absolute_send_time_;
    if (packets_since_absolute_send_time_ >= kTimeOffsetSwitchThreshold) {
      LOG_INFO(
          "WrappingBitrateEstimator: Switching to transmission "
          "time offset RBE.");
      using_absolute_send_time_ = false;
      // rbe_ = std::make_unique<RemoteBitrateEstimatorSingleStream>(
      //     clock_, &remb_throttler_);
      rbe_ = std::make_unique<RemoteBitrateEstimatorAbsSendTime>(
          clock_, &remb_throttler_);
    }
  }
}

ReceiveSideCongestionController::ReceiveSideCongestionController(
    std::shared_ptr<Clock> clock,
    RtpTransportFeedbackGenerator::RtcpSender feedback_sender,
    RembThrottler::RembSender remb_sender)
    : clock_(clock),
      remb_throttler_(std::move(remb_sender), clock.get()),
      feedback_sender_(std::move(feedback_sender)),
      congestion_control_feedback_generator_(
          clock,
          [this](std::vector<std::unique_ptr<RtcpPacket>> packets) {
            for (auto& packet : packets) {
              pending_feedback_.push_back(std::move(packet));
            }
          }),
      // rbe_(std::make_unique<RemoteBitrateEstimatorSingleStream>(
      //     clock, &remb_throttler_)),
      rbe_(std::make_unique<RemoteBitrateEstimatorAbsSendTime>(
          clock, &remb_throttler_)),
      using_absolute_send_time_(false),
      packets_since_absolute_send_time_(0) {}

void ReceiveSideCongestionController::OnReceivedPacket(
    const RtpPacketReceived& packet, MediaType media_type) {
  {
    std::lock_guard<std::mutex> lock(feedback_mutex_);
    congestion_control_feedback_generator_.OnReceivedPacket(packet);
  }
  SendPendingFeedback();
}

void ReceiveSideCongestionController::OnBitrateChanged(int bitrate_bps) {
  std::lock_guard<std::mutex> lock(feedback_mutex_);
  DataRate send_bandwidth_estimate = DataRate::BitsPerSec(bitrate_bps);
  congestion_control_feedback_generator_.OnSendBandwidthEstimateChanged(
      send_bandwidth_estimate);
}

TimeDelta ReceiveSideCongestionController::MaybeProcess() {
  TimeDelta time_until;
  {
    std::lock_guard<std::mutex> lock(feedback_mutex_);
    time_until =
        congestion_control_feedback_generator_.Process(clock_->CurrentTime());
  }
  SendPendingFeedback();
  return std::max(time_until, TimeDelta::Zero());
}

void ReceiveSideCongestionController::SendPendingFeedback() {
  std::unique_lock<std::mutex> lock(feedback_mutex_);
  if (sending_feedback_) {
    return;
  }
  sending_feedback_ = true;
  while (!pending_feedback_.empty()) {
    std::vector<std::unique_ptr<RtcpPacket>> feedback;
    feedback.swap(pending_feedback_);
    // One drainer preserves report timestamp order. Other callers can keep
    // recording arrivals while the transport runs, including reentrant calls.
    lock.unlock();
    feedback_sender_(std::move(feedback));
    lock.lock();
  }
  sending_feedback_ = false;
}

void ReceiveSideCongestionController::SetMaxDesiredReceiveBitrate(
    DataRate bitrate) {
  remb_throttler_.SetMaxDesiredReceiveBitrate(bitrate);
}

void ReceiveSideCongestionController::SetTransportOverhead(
    DataSize overhead_per_packet) {
  std::lock_guard<std::mutex> lock(feedback_mutex_);
  congestion_control_feedback_generator_.SetTransportOverhead(
      overhead_per_packet);
}

}  // namespace webrtc
}  // namespace minirtc
