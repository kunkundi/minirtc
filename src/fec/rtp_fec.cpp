#include "rtp_fec.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <sstream>

namespace minirtc {
namespace {
uint16_t Read16(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
uint32_t Read32(const uint8_t* p) {
  return (uint32_t(Read16(p)) << 16) | Read16(p + 2);
}
void Write16(uint8_t* p, size_t value) {
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}
void Write32(uint8_t* p, uint32_t value) {
  Write16(p, value >> 16);
  Write16(p + 2, value);
}
// Validate every optional RTP length before indexing a network buffer.
size_t HeaderSize(const uint8_t* p, size_t size) {
  if (!p || size < 12 || (p[0] >> 6) != 2 || (p[0] & 0x20)) return 0;
  size_t header = 12 + 4 * (p[0] & 15);
  if (header > size) return 0;
  if (p[0] & 0x10) {
    if (size - header < 4) return 0;
    header += 4 + 4 * size_t(Read16(p + header + 2));
  }
  return header < size ? header : 0;
}
}  // namespace

bool SupportsRsFec(const std::string& section) {
  std::istringstream lines(section);
  std::string line;
  bool attribute = false, payload = false;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == kFecSdpAttribute) attribute = true;
    if (line.rfind("m=video ", 0) == 0) {
      std::istringstream media(line);
      std::string field;
      for (int i = 0; i < 3; ++i) media >> field;
      while (media >> field)
        if (field == "122") payload = true;
    }
  }
  return attribute && payload;
}

bool SupportsOpusFec(const std::string& section) {
  std::istringstream lines(section);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.rfind("a=fmtp:111 ", 0) != 0) continue;
    std::istringstream parameters(line.substr(11));
    std::string parameter;
    while (std::getline(parameters, parameter, ';')) {
      const auto begin = parameter.find_first_not_of(" \t\r");
      const auto end = parameter.find_last_not_of(" \t\r");
      if (begin != std::string::npos &&
          parameter.substr(begin, end - begin + 1) == "useinbandfec=1")
        return true;
    }
  }
  return false;
}

void RtpFecSender::Reset() {
  pending_.clear();
  source_scratch_.clear();
  repair_scratch_.clear();
  credit_bytes_ = 0;
  last_packet_ms_ = -1;
  rate_budget_ = FecRateBudget();
}

void RtpFecSender::SetProtection(const FecProtectionConfig& config, int64_t now_ms) {
  if (config.version < config_.version) return;
  auto next = config;
  next.source_ratio = std::isfinite(next.source_ratio) ?
      std::clamp(next.source_ratio, 0.0, 0.25) : 0;
  next.recovery_window_ms = std::clamp<int64_t>(next.recovery_window_ms, 20, 150);
  if (config_.source_ratio > 0 && next.source_ratio < config_.source_ratio)
    credit_bytes_ *= next.source_ratio / config_.source_ratio;
  if (!next.source_ratio || next.fec_bitrate_bps == 0) {
    pending_.clear();
    credit_bytes_ = 0;
  }
  if (next.fec_bitrate_bps >= 0)
    rate_budget_.SetRate(next.fec_bitrate_bps, now_ms);
  config_ = next;
}

FecSymbols RtpFecSender::FlushExpired(uint32_t repair_ssrc, int64_t now_ms) {
  repair_deadline_ms_ = -1;
  if (pending_.empty() || now_ms - first_ms_ < 20) return {};
  return Flush(repair_ssrc, now_ms);
}

FecSymbols RtpFecSender::AddPacket(const uint8_t* p, size_t size,
                                   uint32_t repair_ssrc, int64_t now_ms,
                                   bool keyframe) {
  FecSymbols result;
  repair_deadline_ms_ = -1;
  if (!HeaderSize(p, size) || size > kFecMaxMediaPacket || !repair_ssrc ||
      repair_ssrc == Read32(p + 8)) {
    pending_.clear();
    ++stats_.rejected_packets;
    return result;
  }
  ++stats_.source_packets;
  if (!config_.source_ratio || config_.fec_bitrate_bps == 0) return result;
  if (last_packet_ms_ < 0 && config_.fec_bitrate_bps >= 0)
    rate_budget_.SetRate(config_.fec_bitrate_bps, now_ms);
  if (last_packet_ms_ >= 0 &&
      (now_ms < last_packet_ms_ || now_ms - last_packet_ms_ > 1000)) {
    credit_bytes_ = 0;
    pending_.clear();
  }
  last_packet_ms_ = now_ms;
  credit_bytes_ = std::min(credit_bytes_ + size * config_.source_ratio, 4800.0);
  if (!pending_.empty()) {
    const auto& last = pending_.back();
    if (Read32(last.data() + 8) != Read32(p + 8) ||
        Read32(last.data() + 4) != Read32(p + 4) ||
        uint16_t(Read16(last.data() + 2) + 1) != Read16(p + 2) ||
        now_ms < first_ms_ || now_ms - first_ms_ >= 20) {
      result = Flush(repair_ssrc, now_ms);
    }
  }
  if (pending_.empty()) {
    first_ms_ = now_ms;
    block_config_ = config_;
    keyframe_ = keyframe;
  }
  pending_.emplace_back(p, p + size);
  if (pending_.size() == kFecMaxSources || (p[1] & 0x80)) {
    auto tail = Flush(repair_ssrc, now_ms);
    for (auto& repair : tail) result.push_back(std::move(repair));
  }
  return result;
}

FecSymbols RtpFecSender::Flush(uint32_t repair_ssrc, int64_t now_ms) {
  FecSymbols packets;
  if (pending_.empty()) return packets;
  FecSymbols pending;
  pending.swap(pending_);
  const int64_t deadline = first_ms_ +
      std::min(config_.recovery_window_ms, block_config_.recovery_window_ms);
  if (now_ms >= deadline) {
    ++stats_.expired_blocks;
    return packets;
  }
  size_t size = 0;
  for (const auto& p : pending) size = std::max(size, p.size() + 2);
  const auto& last = pending.back();
  const size_t header = HeaderSize(last.data(), last.size());
  const size_t wire_size = header + kFecHeaderSize + size;
  if (wire_size > kFecMaxWirePacket) {
    ++stats_.budget_skips;
    return packets;
  }
  const double ratio = std::min(config_.source_ratio, block_config_.source_ratio);
  const double block_ratio = keyframe_ && block_config_.keyframe_priority && ratio > 0
                                 ? std::min(0.25, ratio + 0.05) : ratio;
  size_t repair_count =
      std::min({kFecMaxRepairs, size_t(std::ceil(pending.size() * block_ratio)),
                size_t(credit_bytes_ / (wire_size + 64))});
  size_t affordable = 0;
  while (affordable < repair_count && rate_budget_.Consume(wire_size + 64, now_ms))
    ++affordable;
  repair_count = affordable;
  if (!repair_count) {
    ++stats_.budget_skips;
    return packets;
  }
  auto& sources = source_scratch_;
  sources.resize(pending.size());
  for (size_t i = 0; i < pending.size(); ++i) {
    sources[i].assign(size, 0);
    Write16(sources[i].data(), pending[i].size());
    std::copy(pending[i].begin(), pending[i].end(), sources[i].begin() + 2);
  }
  auto& repairs = repair_scratch_;
  if (!FecEncoder().EncodeSymbols(sources, repair_count, &repairs)) {
    ++stats_.codec_errors;
    return packets;
  }
  for (size_t i = 0; i < repairs.size(); ++i) {
    std::vector<uint8_t> packet(wire_size);
    std::copy_n(last.data(), header, packet.data());
    packet[1] = kRsFecPayloadType;
    Write16(packet.data() + 2, 0);  // Pacer owns the RTX sequence space.
    Write32(packet.data() + 8, repair_ssrc);
    auto* h = packet.data() + header;
    h[0] = 'M';
    h[1] = 'F';
    h[2] = 1;
    h[3] = uint8_t(sources.size());
    h[4] = uint8_t(repair_count);
    h[5] = uint8_t(sources.size() + i);
    Write16(h + 6, size);
    Write32(h + 8, Read32(pending.front().data() + 8));
    Write16(h + 12, Read16(pending.front().data() + 2));
    h[14] = h[15] = 0;
    std::copy(repairs[i].begin(), repairs[i].end(),
              packet.begin() + header + kFecHeaderSize);
    packets.push_back(std::move(packet));
  }
  credit_bytes_ -= repairs.size() * (wire_size + 64);
  repair_deadline_ms_ = repair_deadline_ms_ < 0 ? deadline :
                        std::min(repair_deadline_ms_, deadline);
  stats_.repair_packets += repairs.size();
  return packets;
}

struct RtpFecReceiver::Block {
  uint32_t timestamp = 0;
  uint16_t base = 0;
  size_t k = 0, r = 0, size = 0;
  int64_t arrival = 0;
  bool done = false;
  FecSymbols repairs;
  std::vector<bool> present;
};

RtpFecReceiver::RtpFecReceiver(uint32_t media_ssrc, uint32_t repair_ssrc,
                               uint8_t media_pt)
    : media_ssrc_(media_ssrc), repair_ssrc_(repair_ssrc), media_pt_(media_pt) {}
RtpFecReceiver::~RtpFecReceiver() = default;

void RtpFecReceiver::Expire(int64_t now_ms) {
  for (auto i = sources_.begin(); i != sources_.end();) {
    if (now_ms < i->second.arrival ||
        now_ms - i->second.arrival >= kFecWindowMs)
      i = sources_.erase(i);
    else
      ++i;
  }
  for (auto i = blocks_.begin(); i != blocks_.end();) {
    if (now_ms < i->second->arrival ||
        now_ms - i->second->arrival >= kFecWindowMs) {
      if (!i->second->done) ++stats_.expired_blocks;
      i = blocks_.erase(i);
    } else
      ++i;
  }
}

void RtpFecReceiver::Cache(const std::vector<uint8_t>& packet, int64_t now_ms) {
  const uint16_t seq = Read16(packet.data() + 2);
  // A duplicate must not refresh the retention deadline.
  if (sources_.count(seq)) return;
  if (sources_.size() >= 512) {
    const auto oldest = std::min_element(
        sources_.begin(), sources_.end(), [](const auto& a, const auto& b) {
          return a.second.arrival < b.second.arrival;
        });
    sources_.erase(oldest);
  }
  sources_.emplace(seq, Source{packet, now_ms});
}

FecSymbols RtpFecReceiver::AddPacket(const uint8_t* p, size_t size,
                                     int64_t now_ms) {
  Expire(now_ms);
  FecSymbols recovered;
  const size_t header = HeaderSize(p, size);
  if (!header || size > kFecMaxWirePacket) {
    ++stats_.rejected_packets;
    return recovered;
  }
  if ((p[1] & 127) == media_pt_ && Read32(p + 8) == media_ssrc_ &&
      size <= kFecMaxMediaPacket) {
    ++stats_.source_packets;
    Cache(std::vector<uint8_t>(p, p + size), now_ms);
    for (auto& entry : blocks_) TryDecode(*entry.second, &recovered);
  } else if ((p[1] & 127) == kRsFecPayloadType &&
             Read32(p + 8) == repair_ssrc_ && size - header >= kFecHeaderSize) {
    const uint8_t* h = p + header;
    const size_t k = h[3], r = h[4], id = h[5], symbol_size = Read16(h + 6);
    if (h[0] != 'M' || h[1] != 'F' || h[2] != 1 || h[14] || h[15] || !k ||
        k > kFecMaxSources || !r || r > kFecMaxRepairs || id < k ||
        id >= k + r || symbol_size < 14 ||
        symbol_size > kFecMaxMediaPacket + 2 ||
        size - header - kFecHeaderSize != symbol_size ||
        Read32(h + 8) != media_ssrc_) {
      ++stats_.rejected_packets;
      return recovered;
    }
    const Key key{Read32(p + 4), Read16(h + 12)};
    auto it = blocks_.find(key);
    if (it == blocks_.end()) {
      if (blocks_.size() >= 64) {
        auto oldest = std::min_element(
            blocks_.begin(), blocks_.end(), [](const auto& a, const auto& b) {
              return a.second->arrival < b.second->arrival;
            });
        if (!oldest->second->done) ++stats_.expired_blocks;
        blocks_.erase(oldest);
      }
      auto block = std::make_unique<Block>();
      block->timestamp = key.first;
      block->base = key.second;
      block->k = k;
      block->r = r;
      block->size = symbol_size;
      block->arrival = now_ms;
      block->repairs.resize(r);
      block->present.assign(r, false);
      it = blocks_.emplace(key, std::move(block)).first;
    }
    auto& b = *it->second;
    if (b.k != k || b.r != r || b.size != symbol_size) {
      ++stats_.rejected_packets;
      return recovered;
    }
    if (b.done || b.present[id - k]) return recovered;
    b.repairs[id - k].assign(h + kFecHeaderSize, p + size);
    b.present[id - k] = true;
    ++stats_.repair_packets;
    TryDecode(b, &recovered);
  } else {
    ++stats_.rejected_packets;
  }
  for (const auto& packet : recovered) Cache(packet, now_ms);
  return recovered;
}

void RtpFecReceiver::TryDecode(Block& b, FecSymbols* recovered) {
  if (b.done) return;
  size_t have = 0;
  std::array<const std::vector<uint8_t>*, kFecMaxSources> present{};
  for (size_t i = 0; i < b.k; ++i) {
    const auto it = sources_.find(uint16_t(b.base + i));
    if (it == sources_.end() ||
        Read32(it->second.bytes.data() + 4) != b.timestamp)
      continue;
    const auto& bytes = it->second.bytes;
    if (bytes.size() + 2 > b.size) {
      b.done = true;
      ++stats_.rejected_packets;
      return;
    }
    present[i] = &bytes;
    ++have;
  }
  if (have == b.k) {
    b.done = true;
    b.repairs.clear();
    return;
  }
  const size_t repairs = std::count(b.present.begin(), b.present.end(), true);
  if (have + repairs < b.k) return;
  // Decode only once, and only when enough distinct symbols exist.
  b.done = true;
  auto& decoder = decoder_;
  if (!decoder.Reset(b.k, b.r, b.size)) {
    ++stats_.codec_errors;
    return;
  }
  std::vector<uint8_t> symbol(b.size);
  for (size_t i = 0; i < b.k; ++i) {
    if (!present[i]) continue;
    std::fill(symbol.begin(), symbol.end(), 0);
    Write16(symbol.data(), present[i]->size());
    std::copy(present[i]->begin(), present[i]->end(), symbol.begin() + 2);
    decoder.AddSymbol(i, symbol.data(), b.size);
  }
  for (size_t i = 0; i < b.r && !decoder.Complete(); ++i)
    if (b.present[i]) decoder.AddSymbol(b.k + i, b.repairs[i].data(), b.size);
  b.repairs.clear();
  if (!decoder.Complete()) {
    ++stats_.codec_errors;
    return;
  }
  FecSymbols result;
  for (size_t i = 0; i < b.k; ++i) {
    if (present[i]) continue;
    const auto& symbol = decoder.Symbols()[i];
    const size_t length = Read16(symbol.data());
    const uint8_t* p = symbol.data() + 2;
    if (length > b.size - 2 || !HeaderSize(p, length) ||
        Read32(p + 8) != media_ssrc_ || Read32(p + 4) != b.timestamp ||
        Read16(p + 2) != uint16_t(b.base + i) || (p[1] & 127) != media_pt_) {
      ++stats_.rejected_packets;
      return;
    }
    result.emplace_back(p, p + length);
  }
  stats_.recovered_packets += result.size();
  for (auto& packet : result) recovered->push_back(std::move(packet));
}
}  // namespace minirtc
