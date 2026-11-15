/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_CONGESTION_CONTROLLER_GOOG_CC_PROBE_CONTROLLER_H_
#define MODULES_CONGESTION_CONTROLLER_GOOG_CC_PROBE_CONTROLLER_H_

#include <stdint.h>

#include <optional>
#include <vector>

#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "relay_qos_config.h"

namespace minirtc {
namespace webrtc {

// Fixed GCC defaults from WebRTC 2e61fea6e37524aca68adb43ca304c41f65d4add.
// MiniRTC applies explicit application limits but has no independent network
// capacity estimator or field-trial interface.
struct ProbeControllerConfig {
  double first_exponential_probe_scale = 3.0;
  double second_exponential_probe_scale = 6.0;
  double further_exponential_probe_scale = 2.0;
  double further_probe_threshold = 0.7;
  TimeDelta alr_probing_interval = TimeDelta::Seconds(5);
  double alr_probe_scale = 2.0;
  int min_probe_packets_sent = 5;
  TimeDelta min_probe_duration = TimeDelta::Millis(15);
  TimeDelta min_probe_delta = TimeDelta::Millis(2);
  double loss_limited_probe_scale = 1.5;
};

// Reason that bandwidth estimate is limited. Bandwidth estimate can be limited
// by either delay based bwe, or loss based bwe when it increases/decreases the
// estimate.
enum class BandwidthLimitedCause : int {
  kLossLimitedBweIncreasing = 0,
  kLossLimitedBwe = 1,
  kDelayBasedLimited = 2,
  kDelayBasedLimitedDelayIncreased = 3,
  kRttBasedBackOffHighRtt = 4
};

// This class controls initiation of probing to estimate initial channel
// capacity. There is also support for probing during a session when max
// bitrate is adjusted by an application.
class ProbeController {
 public:
  explicit ProbeController(bool require_video = true,
                           RelayQosConfig relay_config = {});
  ~ProbeController();

  ProbeController(const ProbeController&) = delete;
  ProbeController& operator=(const ProbeController&) = delete;

  [[nodiscard]] std::vector<ProbeClusterConfig> SetBitrates(
      DataRate min_bitrate, DataRate start_bitrate, DataRate max_bitrate,
      Timestamp at_time);

  [[nodiscard]] std::vector<ProbeClusterConfig> OnNetworkAvailability(
      NetworkAvailability msg);

  [[nodiscard]] std::vector<ProbeClusterConfig> SetEstimatedBitrate(
      DataRate bitrate, BandwidthLimitedCause bandwidth_limited_cause,
      Timestamp at_time);

  void EnablePeriodicAlrProbing(bool enable);
  // Zero removes the application limit. This is demand configured by the
  // application, never throughput measured while the application is idle.
  void SetMaxTotalAllocatedBitrate(DataRate bitrate);
  void SetRelayPath(bool relay_path);
  void OnVideoPacketSent();
  void OnVideoSendPaused(bool paused);
  void OnProbeSent(int id, Timestamp at_time);
  void OnProbeResult(int id, double received_fraction = 1.0);
  // A relay candidate ends startup retries only after bandwidth adoption.
  void OnProbeResultAdopted(int id, Timestamp at_time,
                            DataRate bitrate = DataRate::Zero());
  // Called once per real transport report, after delay/loss BWE updates.
  // Timers and low application throughput alone cannot prove path health.
  Timestamp ProbeObservationStartTime() const;
  void OnProbeHealthFeedback(Timestamp at_time, Timestamp latest_send_time,
                             DataRate bitrate, bool delay_overuse, bool in_alr,
                             DataSize non_probe_received);
  void OnProbeResultRejected(int id);
  void OnProbeAborted(int id);
  bool IsProbeResultValid(int id) const;
  // Release a relay estimate and authorize its continuation from the same
  // completed-cluster feedback and observation window.
  bool IsProbeResultReady(int id, Timestamp at_time) const;
  void OnTransportFeedback(Timestamp at_time, TimeDelta rtt);
  void SetTransportLossSample(int settled_packets, int settled_lost);
  void LogState(Timestamp at_time) const;

  void SetAlrStartTime(std::optional<Timestamp> alr_start_time);
  void SetAlrEndedTime(Timestamp alr_end_time);

  [[nodiscard]] std::vector<ProbeClusterConfig> RequestProbe(Timestamp at_time);

  // Resets the ProbeController to a state equivalent to as if it was just
  // created EXCEPT for configuration settings like
  // `enable_periodic_alr_probing_`, `network_available_` and the path profile.
  void Reset(Timestamp at_time);

  [[nodiscard]] std::vector<ProbeClusterConfig> Process(Timestamp at_time);

 private:
  enum class State {
    // Initial state where no probing has been triggered yet.
    kInit,
    // Waiting for probing results to continue further probing.
    kWaitingForProbingResult,
    // Probing is complete.
    kProbingComplete,
  };

  void UpdateState(State new_state);
  [[nodiscard]] std::vector<ProbeClusterConfig> InitiateExponentialProbing(
      Timestamp at_time);
  [[nodiscard]] std::vector<ProbeClusterConfig> InitiateProbing(
      Timestamp now, const std::vector<DataRate>& bitrates_to_probe,
      bool probe_further);
  bool TimeForAlrProbe(Timestamp at_time) const;
  std::vector<ProbeClusterConfig> ContinueProbing(Timestamp at_time);
  const char* ProbeResultBlockReason(Timestamp at_time,
                                     bool require_settled) const;
  const char* ContinuationBlockReason(Timestamp at_time) const;
  bool HasSignificantSettledLoss(Timestamp at_time) const;
  TimeDelta ProbeResultWaitTime() const;
  TimeDelta ContinuationObservationTime() const;
  void RememberCongestedProbe(Timestamp at_time);
  void RecordProbeFailure(Timestamp at_time, const char* reason);
  bool HasProbeRiskPolicy() const;
  bool IsRecoveringProbeRisk() const;
  std::vector<ProbeClusterConfig> RecoverProbing(Timestamp at_time);
  ProbeClusterConfig CreateProbeClusterConfig(Timestamp at_time,
                                              DataRate bitrate);

  bool network_available_;
  BandwidthLimitedCause bandwidth_limited_cause_ =
      BandwidthLimitedCause::kDelayBasedLimited;
  State state_;
  DataRate min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
  Timestamp time_last_probing_initiated_ = Timestamp::MinusInfinity();
  DataRate estimated_bitrate_ = DataRate::Zero();
  DataRate start_bitrate_ = DataRate::Zero();
  DataRate max_bitrate_ = DataRate::PlusInfinity();
  DataRate max_total_allocated_bitrate_ = DataRate::Zero();
  Timestamp last_bwe_drop_probing_time_ = Timestamp::Zero();
  std::optional<Timestamp> alr_start_time_;
  std::optional<Timestamp> alr_end_time_;
  bool enable_periodic_alr_probing_;
  Timestamp time_of_last_large_drop_ = Timestamp::MinusInfinity();
  DataRate bitrate_before_last_large_drop_ = DataRate::Zero();
  bool last_drop_in_alr_ = false;

  const bool require_video_;
  const RelayQosConfig relay_config_;
  bool video_started_ = false;
  bool video_paused_ = false;
  int min_valid_probe_id_ = 1;
  int current_probe_start_id_ = 1;
  int last_probe_result_id_ = -1;
  double last_probe_received_fraction_ = 0;
  bool relay_path_ = false;
  Timestamp initial_retry_deadline_ = Timestamp::MinusInfinity();
  DataRate initial_probe_rate_ = DataRate::Zero();
  int initial_retry_count_ = 0;
  Timestamp last_feedback_time_ = Timestamp::MinusInfinity();
  TimeDelta feedback_rtt_ = TimeDelta::Millis(100);
  int settled_packets_ = 0;
  int settled_lost_ = 0;
  DataRate last_probe_target_ = DataRate::Zero();
  int last_adopted_probe_id_ = -1;
  int last_failed_probe_id_ = -1;
  int last_timed_out_probe_id_ = -1;
  DataRate failed_probe_ceiling_ = DataRate::PlusInfinity();
  Timestamp failed_probe_ceiling_expires_ = Timestamp::MinusInfinity();

  // One exponential chain is one experiment. Its adopted peak remains
  // observable after estimate acceptance or the result deadline expires.
  struct ProbeEpisode {
    int first_id = -1;
    int last_id = -1;
    DataRate highest_target = DataRate::Zero();
    DataRate adopted_rate = DataRate::Zero();
    Timestamp adopted_at = Timestamp::MinusInfinity();
    Timestamp last_feedback = Timestamp::MinusInfinity();
    Timestamp overuse_since = Timestamp::MinusInfinity();
    DataSize non_probe_received = DataSize::Zero();
    bool failed = false;
  };
  std::optional<ProbeEpisode> probe_episode_;
  int probe_failure_count_ = 0;
  // Backoff can relax after a healthy low-rate step, but that does not prove
  // the previously failed range is sustainable. Keep single-step discovery
  // until ordinary traffic has exercised that range successfully.
  DataRate recovery_risk_bitrate_ = DataRate::Zero();
  Timestamp next_recovery_probe_time_ = Timestamp::MinusInfinity();

  int32_t next_probe_cluster_id_ = 1;

  ProbeControllerConfig config_;
};

}  // namespace webrtc
}  // namespace minirtc

#endif  // MODULES_CONGESTION_CONTROLLER_GOOG_CC_PROBE_CONTROLLER_H_
