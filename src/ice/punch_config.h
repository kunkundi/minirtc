/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _PUNCH_CONFIG_H_
#define _PUNCH_CONFIG_H_

#include <cstdint>
#include <functional>
#include <string>

namespace minirtc {
// Unsupported platforms or invalid budgets disable punching.
enum class PunchMode { Off, PoolWide };

// Copied into each connection, then held const. No public Params ABI change.
struct PunchConfig {
  PunchMode mode = PunchMode::Off;
  uint32_t trigger_delay_ms = 1500;
  uint32_t mapping_sample_max_age_ms = 15000;
  uint32_t pool_size = 128;
  uint32_t target_port_count = 1024;
  uint32_t probe_pps = 128;
  uint32_t burst_packets = 8;
  uint32_t pool_probe_rounds = 3;
  uint32_t round_duration_ms = 10000;
  uint32_t ice_finish_reserve_ms = 3000;
  uint32_t probe_packet_budget = 1024;
  uint32_t control_packet_budget = 48;
  uint32_t promoted_endpoint_limit = 4;
  bool enabled() const { return mode != PunchMode::Off; }
  uint32_t sockets() const { return pool_size; }
  bool Validate() const;
};

// Supported native platforms use PoolWide; invalid budgets fail closed.
PunchConfig ReadPunchConfig(const std::function<std::string(const char*)>& get,
                            bool platform_supported);
const PunchConfig& ProcessPunchConfig();
bool PunchPlatformSupported();
inline bool PunchEligible(const PunchConfig& config, bool platform_supported,
                          uint32_t backend_abi, bool reliable, bool turn_auto,
                          uint32_t components) {
  return platform_supported && backend_abi == 2 && config.enabled() &&
         config.Validate() && !reliable && turn_auto && components == 1;
}
}  // namespace minirtc

#endif
