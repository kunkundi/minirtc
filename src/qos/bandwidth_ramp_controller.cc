/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "bandwidth_ramp_controller.h"

#include <algorithm>

#include "log.h"

namespace minirtc {
namespace webrtc {

void BandwidthRampController::Reset(DataRate rate, bool relay, Timestamp now,
                                    DataRate minimum) {
  *this = BandwidthRampController();
  relay_ = relay;
  target_ = rate;
  minimum_ = minimum;
  changed_at_ = now;
  loss_tracker_.Reset(now);
  ResetWindow(now);
}

void BandwidthRampController::ResetWindow(Timestamp now) {
  epoch_ = now;
  first_send_ = first_receive_ = Timestamp::PlusInfinity();
  last_send_ = last_receive_ = last_feedback_ = Timestamp::MinusInfinity();
  received_ = first_received_size_ = DataSize::Zero();
  packets_ = lost_ = 0;
  feedback_packets_.clear();
}

void BandwidthRampController::OnFeedback(
    const TransportPacketsFeedback& report) {
  if (!report.packet_feedbacks.empty())
    network_feedback_at_ = report.feedback_time;
  loss_tracker_.OnFeedback(report, false);
  for (const auto& packet : report.packet_feedbacks) {
    const auto& sent = packet.sent_packet;
    if (!sent.send_time.IsFinite() || sent.send_time < epoch_ ||
        sent.pacing_info.probe_cluster_id != PacedPacketInfo::kNotAProbe)
      continue;
    const auto existing = feedback_packets_.find(sent.sequence_number);
    if (existing != feedback_packets_.end()) {
      // Overlapping RFC 8888 reports may repeat loss and later correct it.
      // Count each packet once, and turn a corrected loss into one receipt.
      if (existing->second || !packet.IsReceived()) continue;
      existing->second = true;
      --lost_;
    } else {
      if (feedback_packets_.size() >= 32768) continue;
      feedback_packets_.emplace(sent.sequence_number, packet.IsReceived());
      ++packets_;
      if (!packet.IsReceived()) ++lost_;
    }
    first_send_ = std::min(first_send_, sent.send_time);
    last_send_ = std::max(last_send_, sent.send_time);
    last_feedback_ = report.feedback_time;
    if (!packet.IsReceived()) continue;
    if (packet.receive_time < first_receive_) {
      first_receive_ = packet.receive_time;
      first_received_size_ = sent.size;
    }
    last_receive_ = std::max(last_receive_, packet.receive_time);
    received_ += sent.size;
  }
}

void BandwidthRampController::OnProbeSent(int id, Timestamp now) {
  if (id <= sent_probe_id_) return;
  sent_probe_id_ = id;
  candidate_id_ = id;
  candidate_ = DataRate::Zero();
  // Confirmation must cover normal traffic after this burst, not an earlier
  // low-rate interval or compressed acknowledgments of the probe itself.
  ResetWindow(now);
}

void BandwidthRampController::OnProbeResult(int id, DataRate rate,
                                            Timestamp now) {
  if (id < candidate_id_) return;
  candidate_id_ = id;
  candidate_ = rate;
  candidate_at_ = now;
}

void BandwidthRampController::InvalidateProbe(int id) {
  if (id == candidate_id_) candidate_ = DataRate::Zero();
}

void BandwidthRampController::Pause(Timestamp now, Timestamp until) {
  // A local queue, keyframe request or video pause is not a capacity failure.
  // Keep the media/pacing budget and recovery history while suspending growth.
  hold_until_ = std::max(hold_until_, until);
  candidate_ = DataRate::Zero();
  confirmed_at_ = Timestamp::MinusInfinity();
  ResetWindow(now);
}

void BandwidthRampController::OnVideoSendPaused(bool paused, Timestamp now) {
  if (video_paused_ == paused) return;
  video_paused_ = paused;
  Pause(now, now);
}

void BandwidthRampController::Hold(Timestamp now, Timestamp until,
                                   const char* reason, TimeDelta rtt) {
  if (until > now && target_ > minimum_ &&
      now - last_backoff_ >= TimeDelta::Seconds(1)) {
    const DataRate previous = target_;
    failed_rate_ = now - failed_at_ < TimeDelta::Seconds(30)
                       ? std::min(failed_rate_, target_)
                       : target_;
    failed_at_ = now;
    // Raw BWE may still contain an optimistic probe result. Back off the
    // published budget before that estimate catches up. Continued congestion
    // may back off again once a second, never once per feedback callback.
    DataRate backed_off = target_ * 0.85;
    if (!confirmed_rate_.IsZero())
      backed_off = std::min(backed_off, confirmed_rate_ * 0.95);
    target_ = std::max(minimum_, backed_off);
    caution_rate_ = target_;
    confirmed_rate_ = std::min(confirmed_rate_, target_);
    changed_at_ = now;
    last_backoff_ = now;
    const auto loss = loss_tracker_.recent();
    LOG_DEBUG(
        "Bandwidth ramp hold: reason={} previous_bps={} target_bps={} "
        "failed_bps={} "
        "confirmed_bps={} settled_lost={} settled_packets={} "
        "loss_grace_ms={} rtt_ms={} hold_ms={}",
        reason, previous.bps(), target_.bps(), failed_rate_.bps(),
        confirmed_rate_.bps(), loss.lost, loss.packets,
        PacketLossTracker::GracePeriod(rtt).ms(),
        rtt.IsFinite() ? rtt.ms() : -1, (until - now).ms());
    // Judge the lower budget using packets sent after this backoff. Delayed
    // reports from the previous rate must not cause another cut; local pauses
    // deliberately keep the tracker so pending corrections can still arrive.
    loss_tracker_.Reset(now);
    unhealthy_since_ = now;
  }
  hold_until_ = std::max(hold_until_, until);
  recovery_until_ = std::max(recovery_until_, until + TimeDelta::Seconds(10));
  candidate_ = DataRate::Zero();
  confirmed_at_ = Timestamp::MinusInfinity();
  ResetWindow(now);
}

TimeDelta BandwidthRampController::ConfirmationDuration(TimeDelta rtt,
                                                        Timestamp now) const {
  if (!rtt.IsFinite()) rtt = TimeDelta::Millis(100);
  const bool recovering =
      now < recovery_until_ ||
      (failed_rate_.IsFinite() && target_ >= caution_rate_ &&
       now - failed_at_ < TimeDelta::Seconds(30));
  return std::clamp(rtt * (recovering ? 4 : 2),
                    TimeDelta::Millis(recovering ? 1500 : 500),
                    TimeDelta::Millis(recovering ? 3000 : 1500));
}

double BandwidthRampController::GrowthScale(Timestamp now) const {
  if (failed_rate_.IsFinite() && target_ >= caution_rate_)
    return now - failed_at_ < TimeDelta::Seconds(30) ? 1.05 : 1.10;
  return now < recovery_until_ ? 1.10 : 1.5;
}

DataRate BandwidthRampController::probe_limit(Timestamp now) const {
  // Direct probes can measure extra headroom, but the media budget still
  // requires normal traffic confirmation before taking another large step.
  const double scale =
      !relay_ && now >= recovery_until_ && !failed_rate_.IsFinite()
          ? 2.0
          : GrowthScale(now);
  return target_ * std::max(1.10, scale);
}

void BandwidthRampController::LogWaiting(const char* reason, DataRate estimate,
                                         DataRate delivered, Timestamp now) {
  if (estimate <= target_ * 1.10 ||
      now - last_status_log_ < TimeDelta::Seconds(2))
    return;
  last_status_log_ = now;
  LOG_DEBUG(
      "Bandwidth ramp waiting: reason={} estimate_bps={} target_bps={} "
      "confirmed_bps={} delivered_bps={} candidate_bps={} packets={} "
      "loss={} hold_ms={}",
      reason, estimate.bps(), target_.bps(), confirmed_rate_.bps(),
      delivered.bps(), candidate_.bps(), packets_,
      packets_ ? double(lost_) / packets_ : 0.0,
      now < hold_until_ ? (hold_until_ - now).ms() : 0);
}

DataRate BandwidthRampController::Update(DataRate estimate, bool healthy,
                                         TimeDelta rtt, Timestamp now,
                                         const char* unhealthy_reason) {
  if (target_.IsZero()) {
    target_ = estimate;
    changed_at_ = now;
    ResetWindow(now);
    return target_;
  }
  loss_tracker_.Advance(now, rtt);
  const auto loss = loss_tracker_.recent();
  const bool settled_loss =
      loss.packets >= 40 && loss.lost >= loss.packets * 0.03;
  if (healthy) {
    unhealthy_since_ = Timestamp::MinusInfinity();
  } else if (!unhealthy_since_.IsFinite()) {
    unhealthy_since_ = now;
  }
  // A transient delay/RTT alarm freezes growth. The underlying delay BWE can
  // still lower its estimate immediately; an additional ramp backoff needs
  // a sustained alarm or loss that has survived the correction interval.
  if (settled_loss ||
      (!healthy && network_feedback_at_.IsFinite() &&
       network_feedback_at_ - unhealthy_since_ >= TimeDelta::Millis(200))) {
    Hold(now, now + TimeDelta::Seconds(2),
         settled_loss ? "settled_packet_loss" : unhealthy_reason, rtt);
  } else if (!healthy) {
    Pause(now, now + TimeDelta::Millis(200));
  }
  if (estimate < target_) {
    if (estimate < target_ * 0.95) {
      LOG_DEBUG(
          "Bandwidth ramp decrease: reason=bandwidth_estimate "
          "previous_bps={} target_bps={} settled_lost={} settled_packets={} "
          "rtt_ms={}",
          target_.bps(), estimate.bps(), loss.lost, loss.packets,
          rtt.IsFinite() ? rtt.ms() : -1);
    }
    if (estimate < target_ * 0.85)
      recovery_until_ = std::max(recovery_until_, now + TimeDelta::Seconds(10));
    // Congestion reductions always pass through immediately.
    target_ = estimate;
    confirmed_rate_ = std::min(confirmed_rate_, target_);
    changed_at_ = now;
    ResetWindow(now);
    return target_;
  }
  if (!healthy || settled_loss || video_paused_ || now < hold_until_) {
    LogWaiting(video_paused_ ? "video_paused" : "growth_hold", estimate,
               DataRate::Zero(), now);
    return target_;
  }
  if (packets_ >= 40 && lost_ >= packets_ * 0.03) {
    // Unsettled missing packets may still arrive. Do not grow into that
    // uncertainty, and do not record it as a failed bandwidth either.
    LogWaiting("pending_packet_loss", estimate, DataRate::Zero(), now);
    if (now - epoch_ > TimeDelta::Seconds(2)) ResetWindow(now);
    return target_;
  }

  const TimeDelta duration = ConfirmationDuration(rtt, now);
  const bool fresh = last_feedback_.IsFinite() &&
                     now - last_feedback_ <= duration &&
                     now - last_send_ <= duration + TimeDelta::Millis(500);
  bool sustained = false;
  DataRate delivered = DataRate::Zero();
  if (fresh && packets_ >= 20 && lost_ < packets_ * 0.02 &&
      first_receive_.IsFinite() && last_receive_ > first_receive_ &&
      last_send_ - first_send_ >= duration) {
    // Both clocks contribute only durations. Use the longer span to avoid
    // treating receiver batching/ACK compression as sustainable throughput.
    const TimeDelta span =
        std::max(last_send_ - first_send_, last_receive_ - first_receive_);
    delivered = (received_ - first_received_size_) / span;
    sustained = delivered >= target_ * 0.80;
  }
  const bool probe_step = candidate_id_ > consumed_id_ &&
                          candidate_ > target_ &&
                          now - candidate_at_ <= TimeDelta::Seconds(2) &&
                          now - changed_at_ >= duration;
  if (!sustained && !probe_step) {
    // A quiet desktop is not a low capacity measurement. Keep the target,
    // discard old windows, and leave later bounded ALR probes available.
    LogWaiting(fresh ? "unconfirmed_traffic" : "no_fresh_feedback", estimate,
               delivered, now);
    if (now - epoch_ > duration * 2) ResetWindow(now);
    return target_;
  }
  if (sustained) {
    confirmed_rate_ = std::min(target_, delivered);
    confirmed_at_ = now;
    if (failed_rate_.IsFinite() && confirmed_rate_ >= failed_rate_ * 1.05) {
      // Clear negative evidence only after real traffic exceeds that rate.
      failed_rate_ = DataRate::PlusInfinity();
      caution_rate_ = DataRate::PlusInfinity();
      recovery_until_ = now;
    }
  }
  DataRate next = std::min(estimate, target_ * GrowthScale(now));
  const bool near_failure = failed_rate_.IsFinite() && target_ >= caution_rate_;
  if (near_failure && !probe_step) next = target_;
  if (near_failure && probe_step) next = std::min(next, candidate_ * 0.95);
  if (!sustained) {
    next = std::min({next, candidate_ * 0.95, target_ * 1.25});
  }
  if (!confirmed_rate_.IsZero())
    next = std::min(next, std::max(target_, confirmed_rate_ * 1.5));
  // One burst may authorize one provisional step, never an entire ramp.
  if (candidate_id_ >= 0) consumed_id_ = candidate_id_;
  if (next > target_) {
    LOG_DEBUG(
        "Bandwidth ramp: path={} phase={} evidence={} estimate_bps={} "
        "target_bps={} next_bps={} confirmed_bps={} delivered_bps={} "
        "window_ms={}",
        relay_ ? "relay" : "direct",
        now < recovery_until_ || near_failure ? "recovery" : "growth",
        sustained ? "traffic" : "probe", estimate.bps(), target_.bps(),
        next.bps(), confirmed_rate_.bps(), delivered.bps(), duration.ms());
    target_ = next;
    changed_at_ = now;
  }
  ResetWindow(now);
  return target_;
}

}  // namespace webrtc
}  // namespace minirtc
