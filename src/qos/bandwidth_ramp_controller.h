/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _BANDWIDTH_RAMP_CONTROLLER_H_
#define _BANDWIDTH_RAMP_CONTROLLER_H_

#include <unordered_map>

#include "api/transport/network_types.h"
#include "packet_loss_tracker.h"

namespace minirtc {
namespace webrtc {

// Separates a burst-capacity estimate from the sustained transport budget.
// All rates include media and repair traffic; probe bytes never confirm load.
class BandwidthRampController {
 public:
  void Reset(DataRate rate, bool relay, Timestamp now,
             DataRate minimum = DataRate::Zero());
  void OnFeedback(const TransportPacketsFeedback& report);
  void OnProbeSent(int id, Timestamp now);
  void OnProbeResult(int id, DataRate rate, Timestamp now);
  void InvalidateProbe(int id);
  void Pause(Timestamp now, Timestamp until);
  void OnVideoSendPaused(bool paused, Timestamp now);
  DataRate Update(DataRate estimate, bool healthy, TimeDelta rtt,
                  Timestamp now);

  DataRate target() const { return target_; }
  DataRate confirmed_rate() const { return confirmed_rate_; }
  Timestamp confirmed_at() const { return confirmed_at_; }
  Timestamp hold_until() const { return hold_until_; }
  DataRate probe_limit(Timestamp now) const;

 private:
  void ResetWindow(Timestamp now);
  TimeDelta ConfirmationDuration(TimeDelta rtt, Timestamp now) const;
  double GrowthScale(Timestamp now) const;
  void LogWaiting(const char* reason, DataRate estimate, DataRate delivered,
                  Timestamp now);

  bool relay_ = false;
  bool video_paused_ = false;
  PacketLossTracker loss_tracker_;
  DataRate target_ = DataRate::Zero();
  DataRate minimum_ = DataRate::Zero();
  DataRate failed_rate_ = DataRate::PlusInfinity();
  DataRate caution_rate_ = DataRate::PlusInfinity();
  Timestamp failed_at_ = Timestamp::MinusInfinity();
  DataRate confirmed_rate_ = DataRate::Zero();
  Timestamp confirmed_at_ = Timestamp::MinusInfinity();
  Timestamp changed_at_ = Timestamp::MinusInfinity();
  Timestamp hold_until_ = Timestamp::MinusInfinity();
  Timestamp recovery_until_ = Timestamp::MinusInfinity();
  Timestamp epoch_ = Timestamp::MinusInfinity();
  Timestamp first_send_ = Timestamp::PlusInfinity();
  Timestamp last_send_ = Timestamp::MinusInfinity();
  Timestamp first_receive_ = Timestamp::PlusInfinity();
  Timestamp last_receive_ = Timestamp::MinusInfinity();
  Timestamp last_feedback_ = Timestamp::MinusInfinity();
  DataSize received_ = DataSize::Zero();
  DataSize first_received_size_ = DataSize::Zero();
  int packets_ = 0;
  int lost_ = 0;
  std::unordered_map<int64_t, bool> feedback_packets_;
  int sent_probe_id_ = -1;
  int candidate_id_ = -1;
  int consumed_id_ = -1;
  DataRate candidate_ = DataRate::Zero();
  Timestamp candidate_at_ = Timestamp::MinusInfinity();
  Timestamp last_status_log_ = Timestamp::MinusInfinity();
};

}  // namespace webrtc
}  // namespace minirtc

#endif
