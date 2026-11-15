/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-15
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RELAY_QOS_CONFIG_H_
#define _RELAY_QOS_CONFIG_H_

#include <cstdlib>
#include <cstring>

namespace minirtc {

// Route policy stays outside the GCC loss model. Set MINIRTC_RELAY_QOS=upstream
// before startup to compare upstream probing/growth defaults on the same path.
// Direct connections always use upstream defaults.
struct RelayQosConfig {
  // Sample beyond a short relay/token-bucket burst, rather than treating its
  // briefly available credit as sustainable link capacity.
  int probe_duration_ms = 100;
  int probe_delta_ms = 5;
  double first_probe_scale = 2;
  double second_probe_scale = 0;
  double alr_probe_scale = 1.5;
  double loss_recovery_max_increase = 1.1;
  // WebRTC supports repeated initial probes in a finite startup window.
  // MiniRTC restricts repeats to failed startup measurements, and additionally
  // lets relay feedback/loss settle before continuing an exponential chain.
  // Congestion may block all retry attempts during the first few seconds.
  // Keep the unused attempts available briefly after that congestion clears,
  // without turning startup recovery into unlimited periodic probing.
  int initial_retry_window_ms = 30000;
  int initial_retry_limit = 2;
  int probe_settle_ms = 150;
  double further_probe_scale = 1.5;

  // A short probe receipt is not evidence of sustained capacity. Observe
  // ordinary traffic before another increase. Remember congested probes while
  // retaining bounded recovery, without capping the media bandwidth estimator.
  int continuation_observation_ms = 500;
  int failed_probe_backoff_ms = 15000;
  // Probe receipts can precede the congestion caused by a short burst. Keep
  // that evidence separate from the one-second estimate acceptance deadline.
  int post_probe_observation_ms = 2000;
  int post_probe_overuse_ms = 200;
  double post_probe_drop_ratio = 0.75;
  int failed_probe_max_backoff_ms = 60000;
  double recovery_probe_scale = 1.2;
  int recovery_probe_interval_ms = 5000;

  static RelayQosConfig Upstream() {
    return {15, 2, 3, 6, 2, 1.3, 0, 0, 0, 2, 0, 0,
            0, 0, 0.75, 0, 1.2, 0};
  }
  static RelayQosConfig FromEnvironment() {
    const char* profile = std::getenv("MINIRTC_RELAY_QOS");
    return profile && std::strcmp(profile, "upstream") == 0 ? Upstream()
                                                            : RelayQosConfig{};
  }
};

}  // namespace minirtc

#endif
