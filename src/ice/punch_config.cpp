#include "punch_config.h"

#include <charconv>
#include <cstdlib>

namespace minirtc {
bool PunchConfig::Validate() const {
  if (mode != PunchMode::Off && mode != PunchMode::PoolWide) return false;
  if (trigger_delay_ms > 15000 || !mapping_sample_max_age_ms ||
      mapping_sample_max_age_ms > 15000 || !pool_size || pool_size > 128 ||
      !target_port_count || target_port_count > 1024 || !probe_pps ||
      probe_pps > 200 || !burst_packets || burst_packets > 8 ||
      !pool_probe_rounds || pool_probe_rounds > 3 || !round_duration_ms ||
      round_duration_ms > 15000 || ice_finish_reserve_ms < 3000 ||
      ice_finish_reserve_ms > 15000 || !probe_packet_budget ||
      probe_packet_budget > 1024 || !control_packet_budget ||
      control_packet_budget > 48 || !promoted_endpoint_limit ||
      promoted_endpoint_limit > 8)
    return false;
  return true;
}

PunchConfig ReadPunchConfig(const std::function<std::string(const char*)>& get,
                            bool platform_supported) {
  PunchConfig c;
  if (!platform_supported) return c;
  c.mode = PunchMode::PoolWide;
  struct Field {
    const char* name;
    uint32_t PunchConfig::* member;
  };
  const Field fields[] = {
      {"TRIGGER_DELAY_MS", &PunchConfig::trigger_delay_ms},
      {"SAMPLE_MAX_AGE_MS", &PunchConfig::mapping_sample_max_age_ms},
      {"POOL_SIZE", &PunchConfig::pool_size},
      {"TARGET_PORT_COUNT", &PunchConfig::target_port_count},
      {"PROBE_PPS", &PunchConfig::probe_pps},
      {"BURST_PACKETS", &PunchConfig::burst_packets},
      {"POOL_PROBE_ROUNDS", &PunchConfig::pool_probe_rounds},
      {"ROUND_DURATION_MS", &PunchConfig::round_duration_ms},
      {"ICE_FINISH_RESERVE_MS", &PunchConfig::ice_finish_reserve_ms},
      {"PROBE_PACKET_BUDGET", &PunchConfig::probe_packet_budget},
      {"CONTROL_PACKET_BUDGET", &PunchConfig::control_packet_budget},
      {"PROMOTED_ENDPOINT_LIMIT", &PunchConfig::promoted_endpoint_limit}};
  for (const auto& f : fields) {
    const auto name = std::string("MINIRTC_UDP_PUNCH_") + f.name;
    const auto value = get(name.c_str());
    if (value.empty()) continue;
    uint32_t n = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), n);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
      return {};
    }
    c.*(f.member) = n;
  }
  if (!c.Validate()) return {};
  return c;
}

bool PunchPlatformSupported() {
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
  return true;
#else
  return false;
#endif
}

const PunchConfig& ProcessPunchConfig() {
  static const PunchConfig value = [] {
    auto c = ReadPunchConfig(
        [](const char* name) {
          const char* v = std::getenv(name);
          return v ? std::string(v) : std::string{};
        },
        PunchPlatformSupported());
    return c;
  }();
  return value;
}
}  // namespace minirtc
