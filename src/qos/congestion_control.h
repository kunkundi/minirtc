#ifndef _CONGESTION_CONTROL_H_
#define _CONGESTION_CONTROL_H_

#include <cstddef>
#include <deque>
#include <memory>

#include "acknowledged_bitrate_estimator_interface.h"
#include "alr_detector.h"
#include "api/transport/network_control.h"
#include "api/transport/network_types.h"
#include "bitrate_limits.h"
#include "delay_based_bwe.h"
#include "packet_loss_tracker.h"
#include "probe_bitrate_estimator.h"
#include "probe_controller.h"
#include "send_side_bandwidth_estimation.h"

namespace minirtc {

using namespace webrtc;

class CongestionControl {
 public:
  explicit CongestionControl(StartingBitrates starting_bitrates = {},
                             bool disable_receiver_padding_probing = true,
                             RelayQosConfig relay_config =
                                 RelayQosConfig::FromEnvironment());
  ~CongestionControl();

 public:
  NetworkControlUpdate OnNetworkAvailability(NetworkAvailability msg);

  NetworkControlUpdate OnProcessInterval(ProcessInterval msg);

  NetworkControlUpdate OnTransportPacketsFeedback(
      TransportPacketsFeedback report);

  void OnSentPacket(size_t bytes_sent, Timestamp at_time,
                    int probe_cluster_id = PacedPacketInfo::kNotAProbe,
                    bool is_video = false,
                    DataSize data_in_flight = DataSize::Zero());

  void OnProbeCompleted(int cluster_id, int sent_packets, Timestamp at_time);
  void OnProbeAborted(int cluster_id, Timestamp at_time, const char* reason);
  void OnVideoSendPaused(bool paused, Timestamp at_time);
  void SetMaxTotalAllocatedBitrate(DataRate bitrate);

  NetworkControlUpdate SetRelayPath(bool relay_path, Timestamp at_time,
                                    bool route_changed = false);

  void MaybeTriggerOnNetworkChanged(NetworkControlUpdate* update,
                                    Timestamp at_time);

 private:
  void ClampConstraints();
  std::vector<ProbeClusterConfig> ResetConstraints(
      TargetRateConstraints new_constraints);
  PacerConfig GetPacingRates(Timestamp at_time) const;
  void UpdatePacketLoss(Timestamp at_time);

 private:
  const RelayQosConfig relay_config_;
  const std::unique_ptr<ProbeController> probe_controller_;

  std::unique_ptr<SendSideBandwidthEstimation> bandwidth_estimation_;
  std::unique_ptr<AlrDetector> alr_detector_;
  std::unique_ptr<ProbeBitrateEstimator> probe_bitrate_estimator_;
  std::unique_ptr<DelayBasedBwe> delay_based_bwe_;
  std::unique_ptr<AcknowledgedBitrateEstimatorInterface>
      acknowledged_bitrate_estimator_;

  std::optional<TargetRateConstraints> initial_constraints_;
  StartingBitrates starting_bitrates_;

  DataRate min_target_rate_ = DataRate::Zero();
  DataRate min_data_rate_ = DataRate::Zero();
  DataRate max_data_rate_ = DataRate::PlusInfinity();
  std::optional<DataRate> starting_rate_;

  Timestamp next_loss_update_ = Timestamp::MinusInfinity();
  int lost_packets_since_last_loss_update_ = 0;
  int expected_packets_since_last_loss_update_ = 0;
  PacketLossTracker loss_tracker_;

  struct PendingProbe {
    int id;
    DataRate bitrate;
  };
  std::optional<PendingProbe> pending_relay_probe_;

  std::deque<int64_t> feedback_max_rtts_;

  DataRate last_loss_based_target_rate_;
  DataRate last_pushback_target_rate_ = DataRate::Zero();
  DataRate last_stable_target_rate_ = DataRate::Zero();
  LossBasedState last_loss_base_state_ = LossBasedState::kDelayBasedEstimate;

  uint8_t last_estimated_fraction_loss_ = 0;
  TimeDelta last_estimated_round_trip_time_ = TimeDelta::PlusInfinity();

  bool previously_in_alr_ = false;
  bool last_reported_in_alr_ = false;
  bool relay_path_ = false;
  std::optional<BandwidthLimitedCause> last_probe_limited_cause_;
  bool first_feedback_received_ = false;
  Timestamp last_diagnostic_time_ = Timestamp::MinusInfinity();
  DataSize outstanding_data_ = DataSize::Zero();
  std::optional<DataSize> current_data_window_;
  double encoding_rate_ratio_ = 1.0;
};
}  // namespace minirtc

#endif
