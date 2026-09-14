/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-14
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _PUNCH_PROTOCOL_H_
#define _PUNCH_PROTOCOL_H_

#include <array>
#include <bitset>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace minirtc::punch {
using Id = std::array<uint8_t, 16>;
using Key = std::array<uint8_t, 32>;
using Bytes = std::vector<uint8_t>;
using Json = nlohmann::json;
constexpr size_t kHeaderSize = 44, kTagSize = 16, kControlMax = 1200,
                 kProbeSize = 80;
enum class Kind : uint8_t {
  Hello = 1,
  Prepare = 2,
  Ready = 3,
  Start = 4,
  Started = 5,
  Cancel = 7,
  Probe = 0x20,
  Ack = 0x21
};
enum class Domain { Control, Probe };
struct Keys {
  Key offer_control{}, answer_control{}, offer_probe{}, answer_probe{};
  void Clear();
  ~Keys() { Clear(); }
};
struct Packet {
  Kind kind;
  Id generation{}, round{};
  uint32_t sequence = 0;
  Bytes payload;
};
struct Probe {
  uint64_t socket = 0;
  std::array<uint8_t, 12> nonce{};
};
bool IsMagic(const uint8_t* data, size_t size);
bool IsZero(const Id& id);
Bytes GenerationContext(const std::string& offer, const std::string& answer);
Id GenerationId(const Bytes& context);
Bytes Encode(const Packet& packet, const Key& key);
std::optional<Packet> Decode(const uint8_t* data, size_t size, const Key& key,
                             Domain domain, const Id& generation);
std::optional<Json> ParseControl(Kind kind, const Bytes& payload);
Bytes ControlPayload(Kind kind, const Json& value);
Bytes ProbePayload(const Probe& probe);
std::optional<Probe> ParseProbe(const Bytes& payload);
std::string Digest(const Bytes& bytes);
bool ValidIpv4(const std::string& ip);

// Keep authenticated packet digests too: reuse of one sequence with different
// contents is a conflict, not an idempotent retransmission. No sequence wrap.
class ReplayWindow {
 public:
  enum class Result { Fresh, Duplicate, TooOld, Conflict };
  Result Accept(uint32_t sequence, const Bytes& authenticated_packet);

 private:
  uint32_t high_ = 0;
  std::bitset<256> seen_;
  std::array<Key, 256> digests_{};
};
}  // namespace minirtc::punch

#endif
