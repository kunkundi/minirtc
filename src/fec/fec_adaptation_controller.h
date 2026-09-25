/*
 * @Author: DI JUNKUN
 * @Date: 2025-09-25
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _FEC_ADAPTATION_CONTROLLER_H_
#define _FEC_ADAPTATION_CONTROLLER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

namespace minirtc {
enum class FecMode { kOff, kFixed, kAdaptive };
enum class FecPacketKind { kMedia, kRepair, kRtx, kOther };
enum class FecDecision {
  kOff,
  kFixed,
  kStartup,
  kHealthy,
  kLoss,
  kCongested,
  kStale,
  kInsufficientSamples
};
const char *FecDecisionName(FecDecision reason);

struct FecFeedbackSnapshot {
  size_t samples = 0, lost = 0, max_loss_run = 0;
  int64_t last_feedback_ms = -1;
  uint64_t feedback_generation = 0;
  // Actual successful sends in the last second, in the caller's byte domain.
  std::array<int64_t, 4> sent_bps{};
  double payload_fraction = 0.94;
  double loss_rate() const { return samples ? double(lost) / samples : 0; }
};

// Serialized by the feedback adapter's existing lock. Registered sends only
// become samples after Commit; a fast feedback may arrive before Commit.
class FecFeedbackTracker {
public:
  static constexpr size_t kMaxPackets = 16384;
  void Register(int64_t id, uint32_t ssrc, FecPacketKind kind, size_t bytes,
                size_t payload_bytes, int64_t now_ms);
  void Commit(int64_t id);
  void Remove(int64_t id);
  void Feedback(int64_t id, bool received, int64_t now_ms);
  std::map<uint32_t, FecFeedbackSnapshot> Snapshot(int64_t now_ms,
                                                   int64_t rtt_ms);
  void Reset();
  size_t size() const { return packets_.size(); }

private:
  struct Packet {
    uint32_t ssrc;
    FecPacketKind kind;
    size_t bytes, payload;
    int64_t sent_ms, first_feedback_ms = -1;
    uint64_t generation = 0;
    bool committed = false, received = false;
  };
  void Prune(int64_t now_ms);
  std::map<int64_t, Packet> packets_;
  uint64_t generation_ = 0;
};

struct FecNetworkSnapshot {
  FecFeedbackSnapshot feedback;
  int64_t now_ms = 0, rtt_ms = 0, queue_ms = 0;
  int64_t transport_bps = 0;
  bool congested = false;
};

struct FecProtectionConfig {
  uint64_t version = 0;
  double source_ratio = 0.25;
  // -1 is the legacy standalone sender (ratio budget only); 0 disables sends.
  int64_t fec_bitrate_bps = -1;
  int64_t media_bitrate_bps = 0;
  int64_t recovery_window_ms = 150;
  bool keyframe_priority = false;
  FecDecision reason = FecDecision::kFixed;
};

class FecAdaptationController {
public:
  FecProtectionConfig Update(FecMode mode, const FecNetworkSnapshot &network);
  void Reset();

private:
  double ratio_ = 0.10, candidate_ = -1;
  int64_t candidate_since_ms_ = -1, last_update_ms_ = -1;
  int64_t previous_bitrate_ = 0, previous_queue_ms_ = 0;
  int64_t congestion_until_ms_ = -1;
  uint64_t last_feedback_generation_ = 0, version_ = 0;
  int confirmation_count_ = 0;
  FecMode mode_ = FecMode::kAdaptive;
  FecProtectionConfig current_;
};

// Caller subtracts other traffic and RTX first. No encoder minimum can expand
// this allocation. Ratio counts RTP source bytes; using wire bytes here is a
// conservative reservation, corrected by the actual sender's byte budget.
FecProtectionConfig AllocateFecBudget(FecProtectionConfig config,
                                      int64_t available_bps,
                                      double payload_fraction);

// A bounded time budget shared by all repair streams in one pacer. No refund
// for failed/dropped sends: this avoids feedback-driven retransmission bursts.
class FecRateBudget {
public:
  void SetRate(int64_t bps, int64_t now_ms);
  bool Consume(size_t bytes, int64_t now_ms);

private:
  void Refill(int64_t now_ms);
  int64_t rate_bps_ = -1, last_ms_ = -1;
  double credit_ = 0;
};
} // namespace minirtc
#endif
