#include "fec_adaptation_controller.h"

#include <algorithm>
#include <cmath>

namespace minirtc {
const char *FecDecisionName(FecDecision reason) {
  switch (reason) {
  case FecDecision::kOff:
    return "off";
  case FecDecision::kFixed:
    return "fixed";
  case FecDecision::kStartup:
    return "startup";
  case FecDecision::kHealthy:
    return "healthy";
  case FecDecision::kLoss:
    return "loss";
  case FecDecision::kCongested:
    return "congested";
  case FecDecision::kStale:
    return "stale";
  case FecDecision::kInsufficientSamples:
    return "insufficient-samples";
  }
  return "unknown";
}
void FecFeedbackTracker::Prune(int64_t now_ms) {
  while (!packets_.empty() &&
         (packets_.size() > kMaxPackets ||
          now_ms - packets_.begin()->second.sent_ms > 5000 ||
          now_ms < packets_.begin()->second.sent_ms))
    packets_.erase(packets_.begin());
}
void FecFeedbackTracker::Register(int64_t id, uint32_t ssrc, FecPacketKind kind,
                                  size_t bytes, size_t payload_bytes,
                                  int64_t now_ms) {
  packets_.insert_or_assign(
      id, Packet{ssrc, kind, bytes, std::min(bytes, payload_bytes), now_ms});
  Prune(now_ms);
}
void FecFeedbackTracker::Commit(int64_t id) {
  auto it = packets_.find(id);
  if (it != packets_.end())
    it->second.committed = true;
}
void FecFeedbackTracker::Remove(int64_t id) { packets_.erase(id); }
void FecFeedbackTracker::Feedback(int64_t id, bool received, int64_t now_ms) {
  auto it = packets_.find(id);
  if (it == packets_.end())
    return;
  auto &p = it->second;
  // Repeated loss reports cannot refresh freshness or grow the denominator.
  if (p.first_feedback_ms >= 0 && (!received || p.received))
    return;
  if (p.first_feedback_ms < 0)
    p.first_feedback_ms = now_ms;
  p.received |= received;
  p.generation = ++generation_;
}
std::map<uint32_t, FecFeedbackSnapshot>
FecFeedbackTracker::Snapshot(int64_t now_ms, int64_t rtt_ms) {
  Prune(now_ms);
  std::map<uint32_t, FecFeedbackSnapshot> result;
  std::map<uint32_t, size_t> runs;
  std::map<uint32_t, size_t> payloads, media_bytes;
  // Wait after a negative report for reordering, but do not wait a full RTT.
  const int64_t grace = std::clamp<int64_t>(rtt_ms / 4, 20, 100);
  const int64_t end = now_ms - grace;
  for (const auto &entry : packets_) {
    const auto &p = entry.second;
    if (!p.committed)
      continue;
    auto &s = result[p.ssrc];
    if (now_ms - p.sent_ms < 1000 && now_ms >= p.sent_ms) {
      s.sent_bps[static_cast<size_t>(p.kind)] += int64_t(p.bytes) * 8;
      if (p.kind == FecPacketKind::kMedia) {
        payloads[p.ssrc] += p.payload;
        media_bytes[p.ssrc] += p.bytes;
      }
    }
    if (p.kind != FecPacketKind::kMedia)
      continue;
    // Original observation time stays unchanged on a late positive report.
    s.last_feedback_ms = std::max(s.last_feedback_ms, p.first_feedback_ms);
    s.feedback_generation = std::max(s.feedback_generation, p.generation);
    if (p.first_feedback_ms < end - 1000 || p.first_feedback_ms > end ||
        p.first_feedback_ms < 0) {
      runs[p.ssrc] = 0;
      continue;
    }
    ++s.samples;
    if (!p.received) {
      ++s.lost;
      s.max_loss_run = std::max(s.max_loss_run, ++runs[p.ssrc]);
    } else {
      runs[p.ssrc] = 0;
    }
  }
  for (auto &item : result) {
    if (media_bytes[item.first])
      item.second.payload_fraction =
          double(payloads[item.first]) / media_bytes[item.first];
  }
  return result;
}
void FecFeedbackTracker::Reset() {
  packets_.clear();
  ++generation_;
}

void FecAdaptationController::Reset() { *this = FecAdaptationController(); }
FecProtectionConfig
FecAdaptationController::Update(FecMode mode, const FecNetworkSnapshot &n) {
  if (mode != mode_ || (last_update_ms_ >= 0 && n.now_ms < last_update_ms_)) {
    const auto version = version_;
    Reset();
    version_ = version;
    mode_ = mode;
  }
  // Urgent congestion/zero budget bypasses the normal control cadence.
  const bool urgent = n.congested || n.queue_ms >= 100 || n.transport_bps <= 0;
  if (!urgent && last_update_ms_ >= 0 && n.now_ms - last_update_ms_ < 200)
    return current_;
  const bool falling =
      previous_bitrate_ > 0 && n.transport_bps < previous_bitrate_ * 8 / 10;
  const bool growing_queue =
      n.queue_ms >= 40 && n.queue_ms > previous_queue_ms_ + 10;
  if (urgent || falling || growing_queue)
    congestion_until_ms_ = n.now_ms + 1000;
  previous_bitrate_ = n.transport_bps;
  previous_queue_ms_ = n.queue_ms;
  last_update_ms_ = n.now_ms;
  FecDecision reason = FecDecision::kStartup;
  double desired = ratio_;
  const auto &f = n.feedback;
  const int64_t stale_ms =
      std::max<int64_t>(2000, 3 * std::clamp<int64_t>(n.rtt_ms, 0, 2000));
  const bool fresh = f.last_feedback_ms >= 0 &&
                     n.now_ms >= f.last_feedback_ms &&
                     n.now_ms - f.last_feedback_ms <= stale_ms;
  if (mode == FecMode::kOff) {
    desired = 0;
    reason = FecDecision::kOff;
  } else if (urgent || n.now_ms < congestion_until_ms_) {
    desired = urgent ? 0 : std::min(ratio_, 0.05);
    reason = FecDecision::kCongested;
  } else if (mode == FecMode::kFixed) {
    desired = 0.25;
    reason = FecDecision::kFixed;
  } else if (!fresh || f.samples < 40) {
    desired = std::min(ratio_, 0.10);
    reason = !fresh ? FecDecision::kStale : FecDecision::kInsufficientSamples;
  } else {
    // Conservative initial bands, exercised by erasure/trace tests. These are
    // deployment tuning parameters, not a guarantee that r/k == loss rate.
    const double p = f.loss_rate();
    desired = p < 0.005   ? 0
              : p < 0.015 ? 0.05
              : p < 0.03  ? 0.10
              : p < 0.05  ? 0.15
              : p < 0.08  ? 0.20
                          : 0.25;
    if (p >= 0.005 && (n.rtt_ms >= 100 || f.max_loss_run >= 3))
      desired = std::min(0.25, desired + 0.05);
    // A short RTT gives retransmission a chance, but never disables all
    // protection solely on RTT. There is no remote playout deadline feedback.
    reason = desired == 0 ? FecDecision::kHealthy : FecDecision::kLoss;
  }
  const bool immediate = mode != FecMode::kAdaptive || !fresh ||
                         f.samples < 40 || reason == FecDecision::kCongested;
  if (immediate) {
    ratio_ = desired;
    candidate_ = -1;
    confirmation_count_ = 0;
  } else if (desired != ratio_) {
    if (candidate_ != desired) {
      candidate_ = desired;
      candidate_since_ms_ = n.now_ms;
      confirmation_count_ = 0;
    }
    if (f.feedback_generation != last_feedback_generation_)
      ++confirmation_count_;
    if ((desired > ratio_ && confirmation_count_ >= 2) ||
        (desired < ratio_ && n.now_ms - candidate_since_ms_ >= 3000 &&
         confirmation_count_ >= 2)) {
      ratio_ = desired;
      candidate_ = -1;
    }
  } else {
    candidate_ = -1;
    confirmation_count_ = 0;
  }
  last_feedback_generation_ = f.feedback_generation;
  current_.version = ++version_;
  current_.source_ratio = ratio_;
  current_.keyframe_priority = mode == FecMode::kAdaptive;
  current_.reason = reason;
  return current_;
}

FecProtectionConfig AllocateFecBudget(FecProtectionConfig c, int64_t bps,
                                      double fraction) {
  bps = std::clamp<int64_t>(bps, 0, 1000000000);
  c.source_ratio =
      std::isfinite(c.source_ratio) ? std::clamp(c.source_ratio, 0.0, 0.25) : 0;
  fraction = std::isfinite(fraction) ? std::clamp(fraction, 0.05, 1.0) : 0.94;
  const auto media = static_cast<int64_t>(bps / (1 + c.source_ratio));
  c.fec_bitrate_bps = bps - media;
  c.media_bitrate_bps = static_cast<int64_t>(media * fraction);
  return c;
}
void FecRateBudget::Refill(int64_t now_ms) {
  if (last_ms_ >= 0 && now_ms >= last_ms_ && rate_bps_ > 0)
    credit_ = std::min(
        4800.0, credit_ + double(std::min<int64_t>(now_ms - last_ms_, 1000)) *
                              rate_bps_ / 8000);
  if (now_ms < last_ms_)
    credit_ = 0;
  last_ms_ = now_ms;
}
void FecRateBudget::SetRate(int64_t bps, int64_t now_ms) {
  Refill(now_ms);
  bps = std::clamp<int64_t>(bps, 0, 1000000000);
  if (rate_bps_ > 0 && bps < rate_bps_)
    credit_ *= double(bps) / rate_bps_;
  if (!bps)
    credit_ = 0;
  rate_bps_ = bps;
}
bool FecRateBudget::Consume(size_t bytes, int64_t now_ms) {
  if (rate_bps_ < 0)
    return true;
  Refill(now_ms);
  if (bytes > credit_)
    return false;
  credit_ -= bytes;
  return true;
}
} // namespace minirtc
