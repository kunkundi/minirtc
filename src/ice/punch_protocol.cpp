#include "punch_protocol.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace minirtc::punch {
namespace {
constexpr uint8_t magic[] = {0xf3, 0x4d, 0x50, 0x31};
void Put(Bytes& out, uint64_t n, size_t count) {
  for (size_t i = count; i; --i)
    out.push_back(static_cast<uint8_t>(n >> ((i - 1) * 8)));
}
uint64_t Get(const uint8_t* p, size_t count) {
  uint64_t n = 0;
  for (size_t i = 0; i < count; ++i) n = (n << 8) | p[i];
  return n;
}
Key Sha(const Bytes& b) {
  Key out{};
  SHA256(b.data(), b.size(), out.data());
  return out;
}
Key Tag(const uint8_t* data, size_t size, const Key& key) {
  Key out{};
  unsigned n = 0;
  if (!HMAC(EVP_sha256(), key.data(), key.size(), data, size, out.data(), &n) ||
      n != out.size())
    throw std::runtime_error("UDP punch HMAC failed");
  return out;
}
bool Fields(const Json& j, std::initializer_list<const char*> fields) {
  if (!j.is_object() || j.size() != fields.size()) return false;
  for (auto f : fields)
    if (!j.contains(f)) return false;
  return true;
}
bool Number(const Json& j, uint64_t low, uint64_t high) {
  if (!j.is_number_integer() ||
      (!j.is_number_unsigned() && j.get<int64_t>() < 0))
    return false;
  const auto n = j.get<uint64_t>();
  return n >= low && n <= high;
}
bool String(const Json& j, size_t low, size_t high) {
  return j.is_string() && j.get_ref<const std::string&>().size() >= low &&
         j.get_ref<const std::string&>().size() <= high &&
         j.get_ref<const std::string&>().find('\0') == std::string::npos;
}
bool HexDigest(const Json& j) {
  if (!String(j, 64, 64)) return false;
  const auto& s = j.get_ref<const std::string&>();
  return std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool Ip(const Json& j) {
  return String(j, 7, 15) && ValidIpv4(j.get<std::string>());
}
bool Endpoint(const Json& j) {
  return Fields(j, {"ip", "port"}) && Ip(j["ip"]) &&
         Number(j["port"], 1, 65535);
}
bool Budget(const Json& j) {
  return Fields(j, {"pool_size", "target_port_count", "probe_pps",
                    "burst_packets", "pool_probe_rounds", "round_duration_ms",
                    "ice_finish_reserve_ms", "probe_packet_budget",
                    "control_packet_budget", "promoted_endpoint_limit"}) &&
         Number(j["pool_size"], 1, 128) &&
         Number(j["target_port_count"], 1, 1024) &&
         Number(j["probe_pps"], 1, 200) && Number(j["burst_packets"], 1, 8) &&
         Number(j["pool_probe_rounds"], 1, 3) &&
         Number(j["round_duration_ms"], 1, 15000) &&
         Number(j["ice_finish_reserve_ms"], 3000, 15000) &&
         Number(j["probe_packet_budget"], 1, 1024) &&
         Number(j["control_packet_budget"], 1, 48) &&
         Number(j["promoted_endpoint_limit"], 1, 8);
}
bool Schema(Kind kind, const Json& j) {
  switch (kind) {
    case Kind::Hello: {
      if (!Fields(j, {"base", "samples", "budget", "remaining_ms", "mode"}) ||
          !Budget(j["budget"]) || !Number(j["remaining_ms"], 1, 30000) ||
          j["mode"] != "pool-wide")
        return false;
      const auto& base = j["base"];
      if (!Fields(base, {"ip", "port", "socket", "foundation", "component"}) ||
          !Ip(base["ip"]) || !Number(base["port"], 1, 65535) ||
          !Number(base["socket"], 1, std::numeric_limits<uint64_t>::max()) ||
          !String(base["foundation"], 1, 32) ||
          !Number(base["component"], 1, 1))
        return false;
      const auto& samples = j["samples"];
      if (!samples.is_array() || samples.size() > 3) return false;
      for (const auto& s : samples)
        if (!Fields(s, {"server", "server_port", "ip", "port", "age_ms",
                        "sequence"}) ||
            !Ip(s["server"]) || !Number(s["server_port"], 1, 65535) ||
            !Ip(s["ip"]) || !Number(s["port"], 1, 65535) ||
            !Number(s["age_ms"], 0, 15000) ||
            !Number(s["sequence"], 0, UINT32_MAX))
          return false;
      return true;
    }
    case Kind::Prepare:
      return Fields(j, {"anchor", "anchor_endpoint", "peer_ip", "budget",
                        "duration_ms", "offer_hello", "answer_hello"}) &&
             (j["anchor"] == "offer" || j["anchor"] == "answer") &&
             Endpoint(j["anchor_endpoint"]) && Ip(j["peer_ip"]) &&
             Budget(j["budget"]) && Number(j["duration_ms"], 1, 15000) &&
             HexDigest(j["offer_hello"]) && HexDigest(j["answer_hello"]);
    case Kind::Ready:
      return Fields(j, {"digest", "budget"}) && HexDigest(j["digest"]) &&
             Budget(j["budget"]);
    case Kind::Start:
    case Kind::Started:
      return Fields(j, {"digest"}) && HexDigest(j["digest"]);
    case Kind::Cancel:
      return Fields(j, {"reason"}) && String(j["reason"], 1, 32) &&
             std::set<std::string>{
                 "timeout",          "state-change",      "ineligible",
                 "resources",        "conflict",          "disconnect",
                 "interface-change", "generation-change", "budget"}
                 .count(j["reason"].get<std::string>());
    default:
      return false;
  }
}
bool Known(Kind kind, Domain domain) {
  const auto n = static_cast<uint8_t>(kind);
  return domain == Domain::Probe ? (kind == Kind::Probe || kind == Kind::Ack)
                                 : (n >= 1 && n <= 5) || kind == Kind::Cancel;
}
}  // namespace

void Keys::Clear() { OPENSSL_cleanse(this, sizeof(*this)); }
bool IsMagic(const uint8_t* data, size_t size) {
  return data && size >= sizeof(magic) &&
         std::equal(std::begin(magic), std::end(magic), data);
}
bool IsZero(const Id& id) {
  return std::all_of(id.begin(), id.end(), [](uint8_t n) { return n == 0; });
}
bool ValidIpv4(const std::string& ip) {
  size_t pos = 0;
  for (int i = 0; i < 4; ++i) {
    size_t start = pos;
    unsigned n = 0;
    while (pos < ip.size() && ip[pos] >= '0' && ip[pos] <= '9') {
      n = n * 10 + unsigned(ip[pos++] - '0');
      if (n > 255 || pos - start > 3) return false;
    }
    if (pos == start || (pos - start > 1 && ip[start] == '0')) return false;
    if (i < 3 && (pos == ip.size() || ip[pos++] != '.')) return false;
  }
  return pos == ip.size();
}
Bytes GenerationContext(const std::string& offer, const std::string& answer) {
  Bytes out;
  for (const auto* u : {&offer, &answer}) {
    if (u->empty() || u->size() > 256 ||
        u->find_first_of(" \t\r\n") != std::string::npos ||
        u->find('\0') != std::string::npos)
      return {};
    Put(out, u->size(), 2);
    out.insert(out.end(), u->begin(), u->end());
  }
  return out;
}
Id GenerationId(const Bytes& context) {
  Id id{};
  if (!context.empty()) {
    const auto hash = Sha(context);
    std::copy_n(hash.begin(), id.size(), id.begin());
  }
  return id;
}
std::string Digest(const Bytes& bytes) {
  const auto hash = Sha(bytes);
  std::string s;
  for (auto c : hash) {
    s += "0123456789abcdef"[c >> 4];
    s += "0123456789abcdef"[c & 15];
  }
  return s;
}
std::optional<Json> ParseControl(Kind kind, const Bytes& payload) {
  if (!Known(kind, Domain::Control) ||
      payload.size() > kControlMax - kHeaderSize - kTagSize)
    return {};
  bool valid = true;
  std::vector<std::set<std::string>> keys;
  auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
    if (depth > 8) valid = false;
    if (event == Json::parse_event_t::object_start) keys.emplace_back();
    if (event == Json::parse_event_t::key &&
        (!keys.size() || !keys.back().insert(value.get<std::string>()).second))
      valid = false;
    if (event == Json::parse_event_t::object_end && keys.size())
      keys.pop_back();
    return true;
  };
  try {
    auto j = Json::parse(payload.begin(), payload.end(), callback, true, false);
    if (valid && Schema(kind, j)) return j;
  } catch (const Json::exception&) {
  }
  return {};
}
Bytes ControlPayload(Kind kind, const Json& j) {
  if (!Schema(kind, j)) return {};
  try {
    const auto s = j.dump();
    if (s.size() <= kControlMax - kHeaderSize - kTagSize)
      return {s.begin(), s.end()};
  } catch (const Json::exception&) {
  }
  return {};
}
Bytes ProbePayload(const Probe& p) {
  if (!p.socket) return {};
  Bytes out;
  Put(out, p.socket, 8);
  out.insert(out.end(), p.nonce.begin(), p.nonce.end());
  return out;
}
std::optional<Probe> ParseProbe(const Bytes& p) {
  if (p.size() != 20) return {};
  Probe result;
  result.socket = Get(p.data(), 8);
  std::copy_n(p.begin() + 8, 12, result.nonce.begin());
  return result.socket ? std::optional<Probe>(result) : std::nullopt;
}
Bytes Encode(const Packet& p, const Key& key) {
  const auto domain = (p.kind == Kind::Probe || p.kind == Kind::Ack)
                          ? Domain::Probe
                          : Domain::Control;
  if (!p.sequence || IsZero(p.generation) ||
      (p.kind == Kind::Hello ? !IsZero(p.round) : IsZero(p.round)) ||
      (domain == Domain::Control ? !ParseControl(p.kind, p.payload).has_value()
                                 : !ParseProbe(p.payload).has_value()))
    return {};
  Bytes out(std::begin(magic), std::end(magic));
  out.push_back(1);
  out.push_back(static_cast<uint8_t>(p.kind));
  Put(out, p.payload.size(), 2);
  out.insert(out.end(), p.generation.begin(), p.generation.end());
  out.insert(out.end(), p.round.begin(), p.round.end());
  Put(out, p.sequence, 4);
  out.insert(out.end(), p.payload.begin(), p.payload.end());
  const auto tag = Tag(out.data(), out.size(), key);
  out.insert(out.end(), tag.begin(), tag.begin() + kTagSize);
  return out;
}
std::optional<Packet> Decode(const uint8_t* data, size_t size, const Key& key,
                             Domain domain, const Id& generation) {
  if (!IsMagic(data, size) || size < kHeaderSize + kTagSize ||
      size > kControlMax || data[4] != 1 ||
      !Known(static_cast<Kind>(data[5]), domain) ||
      size != kHeaderSize + Get(data + 6, 2) + kTagSize ||
      (domain == Domain::Probe && size != kProbeSize) || IsZero(generation) ||
      !std::equal(generation.begin(), generation.end(), data + 8))
    return {};
  const auto tag = Tag(data, size - kTagSize, key);
  if (CRYPTO_memcmp(tag.data(), data + size - kTagSize, kTagSize)) return {};
  Packet p{static_cast<Kind>(data[5])};
  p.generation = generation;
  std::copy_n(data + 24, 16, p.round.begin());
  p.sequence = static_cast<uint32_t>(Get(data + 40, 4));
  if (!p.sequence ||
      (p.kind == Kind::Hello ? !IsZero(p.round) : IsZero(p.round)))
    return {};
  p.payload.assign(data + kHeaderSize, data + size - kTagSize);
  if (domain == Domain::Control ? !ParseControl(p.kind, p.payload).has_value()
                                : !ParseProbe(p.payload).has_value())
    return {};
  return p;
}
ReplayWindow::Result ReplayWindow::Accept(uint32_t sequence,
                                          const Bytes& bytes) {
  if (!sequence || (sequence <= high_ && high_ - sequence >= 256))
    return Result::TooOld;
  if (sequence > high_) {
    const uint32_t shift = sequence - high_;
    if (shift >= 256)
      seen_.reset();
    else
      seen_ <<= shift;
    high_ = sequence;
  }
  const auto offset = high_ - sequence;
  const auto hash = Sha(bytes);
  if (seen_[offset])
    return CRYPTO_memcmp(hash.data(), digests_[sequence % 256].data(),
                         hash.size()) == 0
               ? Result::Duplicate
               : Result::Conflict;
  seen_.set(offset);
  digests_[sequence % 256] = hash;
  return Result::Fresh;
}
}  // namespace minirtc::punch
