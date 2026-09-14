#include "punch_scheduler.h"

#include <openssl/rand.h>

#include <algorithm>
#include <tuple>

#include "nat_traversal.h"

namespace minirtc {
using namespace punch;
PunchMapping ClassifyPunchMapping(const PunchMappingSnapshot& s, uint64_t gen,
                                  int64_t now, uint32_t age) {
  if (!s.socket || !gen || s.generation != gen || !s.local_port ||
      !ValidIpv4(s.local_ip) || s.component != 1 || s.foundation.empty() ||
      s.samples.size() < 2 || s.samples.size() > 32)
    return PunchMapping::Unknown;
  std::set<std::string> servers;
  std::set<uint16_t> ports;
  std::string ip;
  for (const auto& sample : s.samples) {
    if (sample.socket != s.socket || sample.generation != gen ||
        sample.received_ms > now || now - sample.received_ms > age ||
        !IsPublicIpv4ForPrediction(sample.server_ip) || !sample.server_port ||
        !IsPublicIpv4ForPrediction(sample.mapped_ip) || !sample.mapped_port)
      return PunchMapping::Unknown;
    if (!ip.empty() && ip != sample.mapped_ip) return PunchMapping::Unknown;
    ip = sample.mapped_ip;
    servers.insert(sample.server_ip);
    ports.insert(sample.mapped_port);
  }
  if (servers.size() < 2) return PunchMapping::Unknown;
  return ports.size() == 1 ? PunchMapping::StableObserved
                           : PunchMapping::DependentObserved;
}
std::optional<PunchMappingSnapshot> SelectPunchMapping(
    const std::vector<PunchMappingSnapshot>& snapshots,
    const std::string& selected_ip, uint16_t selected_port, uint64_t gen,
    int64_t now, uint32_t age) {
  using Rank = std::tuple<int, int, int64_t, std::string, uint16_t, std::string,
                          uint64_t>;
  std::optional<Rank> best;
  std::optional<PunchMappingSnapshot> chosen;
  for (auto snapshot : snapshots) {
    snapshot.samples.erase(
        std::remove_if(
            snapshot.samples.begin(), snapshot.samples.end(),
            [&](const auto& sample) { return now - sample.received_ms > age; }),
        snapshot.samples.end());
    const auto mapping = ClassifyPunchMapping(snapshot, gen, now, age);
    if (mapping == PunchMapping::Unknown) continue;
    const bool same_ip = snapshot.local_ip == selected_ip;
    const int path =
        same_ip ? (snapshot.local_port == selected_port ? 0 : 1) : 2;
    const auto oldest =
        std::min_element(snapshot.samples.begin(), snapshot.samples.end(),
                         [](const auto& a, const auto& b) {
                           return a.received_ms < b.received_ms;
                         });
    // Route affinity wins over mapping class. Within that preference, a stable
    // mapping can anchor a dependent peer; fresher samples leave more time to
    // finish. Identity only breaks otherwise equivalent ties, never picks the
    // interface before route affinity as map iteration used to do.
    const Rank rank{path,
                    mapping == PunchMapping::StableObserved ? 0 : 1,
                    -oldest->received_ms,
                    snapshot.local_ip,
                    snapshot.local_port,
                    snapshot.foundation,
                    snapshot.socket};
    if (!best || rank < *best) {
      best = rank;
      chosen = std::move(snapshot);
    }
  }
  return chosen;
}
Json PunchBudgetJson(const PunchConfig& c) {
  return {{"pool_size", c.sockets()},
          {"target_port_count", c.target_port_count},
          {"probe_pps", c.probe_pps},
          {"burst_packets", c.burst_packets},
          {"pool_probe_rounds", c.pool_probe_rounds},
          {"round_duration_ms", c.round_duration_ms},
          {"ice_finish_reserve_ms", c.ice_finish_reserve_ms},
          {"probe_packet_budget", c.probe_packet_budget},
          {"control_packet_budget", c.control_packet_budget},
          {"promoted_endpoint_limit", c.promoted_endpoint_limit}};
}
std::optional<Json> PunchHello(const PunchMappingSnapshot& s,
                               const PunchConfig& c, uint64_t gen, int64_t now,
                               int64_t remaining) {
  if (!c.enabled() || !c.Validate() || remaining <= 0 || remaining > 30000 ||
      ClassifyPunchMapping(s, gen, now, c.mapping_sample_max_age_ms) ==
          PunchMapping::Unknown)
    return {};
  Json samples = Json::array();
  // At most three distinct destinations keep HELLO below the datagram cap.
  // Preserve contradictory ports in preference to duplicate stable samples.
  auto ordered = s.samples;
  std::stable_sort(
      ordered.begin(), ordered.end(),
      [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
  std::vector<PunchMappingSample> selected{ordered.front()};
  for (const auto& v : ordered)
    if (v.mapped_port != selected.front().mapped_port) {
      selected.push_back(v);
      break;
    }
  for (const auto& v : ordered) {
    if (selected.size() >= 3) break;
    if (std::none_of(selected.begin(), selected.end(),
                     [&](const auto& x) { return x.server_ip == v.server_ip; }))
      selected.push_back(v);
  }
  for (const auto& v : selected)
    samples.push_back({{"server", v.server_ip},
                       {"server_port", v.server_port},
                       {"ip", v.mapped_ip},
                       {"port", v.mapped_port},
                       {"age_ms", now - v.received_ms},
                       {"sequence", v.sequence}});
  Json hello = {{"base",
                 {{"ip", s.local_ip},
                  {"port", s.local_port},
                  {"socket", s.socket},
                  {"foundation", s.foundation},
                  {"component", s.component}}},
                {"samples", samples},
                {"budget", PunchBudgetJson(c)},
                {"remaining_ms", remaining},
                {"mode", "pool-wide"}};
  if (ControlPayload(Kind::Hello, hello).empty()) return {};
  return hello;
}
namespace {
PunchMapping ClassifyHello(const Json& h) {
  PunchMappingSnapshot s;
  s.socket = h["base"]["socket"].get<uint64_t>();
  s.generation = 1;
  s.local_ip = h["base"]["ip"];
  s.local_port = h["base"]["port"];
  s.foundation = h["base"]["foundation"];
  for (const auto& v : h["samples"])
    s.samples.push_back({s.socket, 1, v["server"], v["ip"], v["server_port"],
                         v["port"], 15000 - v["age_ms"].get<int64_t>(),
                         v["sequence"]});
  return ClassifyPunchMapping(s, 1, 15000);
}
}  // namespace
std::optional<Json> MakePunchPlan(const Json& offer, const Json& answer) {
  if (ControlPayload(Kind::Hello, offer).empty() ||
      ControlPayload(Kind::Hello, answer).empty() ||
      offer["mode"] != answer["mode"])
    return {};
  const auto a = ClassifyHello(offer), b = ClassifyHello(answer);
  bool anchor_offer;
  if (a == PunchMapping::StableObserved && b == PunchMapping::DependentObserved)
    anchor_offer = true;
  else if (b == PunchMapping::StableObserved &&
           a == PunchMapping::DependentObserved)
    anchor_offer = false;
  else
    return {};
  auto budget = offer["budget"];
  for (auto it = budget.begin(); it != budget.end(); ++it) {
    const auto x = it.value().get<uint32_t>(),
               y = answer["budget"][it.key()].get<uint32_t>();
    it.value() =
        it.key() == "ice_finish_reserve_ms" ? std::max(x, y) : std::min(x, y);
  }
  const auto remaining = std::min(offer["remaining_ms"].get<uint32_t>(),
                                  answer["remaining_ms"].get<uint32_t>());
  const auto reserve = budget["ice_finish_reserve_ms"].get<uint32_t>();
  // Leave room for bounded control retries/allocation; local clocks still own
  // the actual finish deadline, so this is not permission to extend a window.
  if (remaining <= reserve + 3000) return {};
  const auto duration = std::min(budget["round_duration_ms"].get<uint32_t>(),
                                 remaining - reserve - 3000);
  const auto& anchor = anchor_offer ? offer : answer;
  const auto& pool = anchor_offer ? answer : offer;
  Json plan = {{"anchor", anchor_offer ? "offer" : "answer"},
               {"anchor_endpoint",
                {{"ip", anchor["samples"][0]["ip"]},
                 {"port", anchor["samples"][0]["port"]}}},
               {"peer_ip", pool["samples"][0]["ip"]},
               {"budget", budget},
               {"duration_ms", duration},
               {"offer_hello", Digest(ControlPayload(Kind::Hello, offer))},
               {"answer_hello", Digest(ControlPayload(Kind::Hello, answer))}};
  return ControlPayload(Kind::Prepare, plan).empty()
             ? std::nullopt
             : std::optional<Json>(plan);
}
bool SecurePunchRandom(uint32_t& value) {
  return RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) ==
         1;
}
std::vector<uint16_t> SamplePunchPorts(size_t count,
                                       const std::set<uint16_t>& exclude,
                                       const PunchRandom& random) {
  if (!count || count > 1024 || !random) return {};
  std::vector<uint16_t> ports;
  for (uint32_t p = 1024; p <= 65535; ++p)
    if (!exclude.count(static_cast<uint16_t>(p)))
      ports.push_back(static_cast<uint16_t>(p));
  if (count > ports.size()) return {};
  for (size_t i = 0; i < count; ++i) {
    const uint64_t bound = ports.size() - i, range = uint64_t{1} << 32;
    const uint64_t limit = range - range % bound;
    uint32_t value = 0;
    bool accepted = false;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      if (!random(value)) return {};
      if (value < limit) {
        accepted = true;
        break;
      }
    }
    if (!accepted) return {};
    std::swap(ports[i], ports[i + value % bound]);
  }
  ports.resize(count);
  // Sort only the sampled subset: preserve random coverage of the full range
  // while probing its discrete ports in ascending order.
  std::sort(ports.begin(), ports.end());
  return ports;
}
PunchDeadlines MakePunchDeadlines(int64_t now, int64_t original,
                                  uint32_t duration, uint32_t reserve) {
  if (now < 0 || original <= now || original - now <= reserve || !duration ||
      duration > 15000 || reserve < 3000 || reserve > 15000)
    return {};
  const auto probe =
      now + std::min<int64_t>(duration, original - now - reserve);
  return {probe, std::min<int64_t>(original, probe + reserve)};
}
PunchTokenBucket::PunchTokenBucket(uint32_t rate, uint32_t burst, int64_t now)
    : rate_(rate), burst_(burst), tokens_(int64_t(burst) * 1000), last_(now) {}
bool PunchTokenBucket::Available(int64_t now) {
  if (now < last_) return false;
  const auto elapsed = std::min<int64_t>(now - last_, 1000 * int64_t(burst_));
  tokens_ =
      std::min<int64_t>(int64_t(burst_) * 1000, tokens_ + elapsed * rate_);
  last_ = now;
  return tokens_ >= 1000;
}
bool PunchTokenBucket::Take(int64_t now) {
  if (!Available(now)) return false;
  tokens_ -= 1000;
  return true;
}
std::optional<size_t> PunchRoundRobin::Peek(int64_t now) {
  if (done() || now < round_start_) return {};
  return index_;
}
void PunchRoundRobin::Commit(int64_t now) {
  if (!Peek(now)) return;
  if (index_ == 0) round_start_ = now;
  if (++index_ == count_) {
    index_ = 0;
    ++round_;
    round_start_ = std::max(round_start_ + 1000, now);
  }
}
}  // namespace minirtc
