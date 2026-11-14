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
  int probe_duration_ms = 50;
  int probe_delta_ms = 5;
  double first_probe_scale = 2;
  double second_probe_scale = 0;
  double alr_probe_scale = 1.5;
  double loss_recovery_max_increase = 1.05;

  static RelayQosConfig Upstream() { return {15, 2, 3, 6, 2, 1.3}; }
  static RelayQosConfig FromEnvironment() {
    const char* profile = std::getenv("MINIRTC_RELAY_QOS");
    return profile && std::strcmp(profile, "upstream") == 0 ? Upstream()
                                                            : RelayQosConfig{};
  }
};

}  // namespace minirtc

#endif
