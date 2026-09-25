/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-26
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RECEIVER_RTT_H_
#define _RECEIVER_RTT_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace minirtc {

// RFC 3611 RRTR/DLRR support, including receivers which send no RTP media.
class ReceiverRtt {
 public:
  struct Dlrr {
    uint32_t ssrc;
    uint32_t last_rr;
    uint32_t delay;
  };
  struct Report {
    uint32_t sender_ssrc = 0;
    std::optional<uint64_t> reference_time;
    std::vector<Dlrr> replies;

    // A reference time piggybacked on a reply timestamps that reply. It must
    // not start another exchange (otherwise two upgraded peers echo forever).
    bool NeedsReply() const {
      return reference_time && *reference_time != 0 && replies.empty();
    }
  };

  static bool Parse(const uint8_t* payload, size_t size, Report& report) {
    if (!payload || size < 4) return false;
    Report parsed;
    parsed.sender_ssrc = Read32(payload);
    for (size_t pos = 4; pos < size;) {
      if (size - pos < 4) return false;
      const size_t length =
          (size_t(payload[pos + 2]) * 256 + payload[pos + 3] + 1) * 4;
      if (length > size - pos) return false;
      if (payload[pos] == 4) {
        if (length != 12 || parsed.reference_time) return false;
        parsed.reference_time = (uint64_t(Read32(payload + pos + 4)) << 32) |
                                Read32(payload + pos + 8);
      } else if (payload[pos] == 5) {
        if ((length - 4) % 12 != 0) return false;
        for (size_t i = pos + 4; i < pos + length; i += 12)
          parsed.replies.push_back({Read32(payload + i),
                                    Read32(payload + i + 4),
                                    Read32(payload + i + 8)});
      }
      pos += length;
    }
    report = std::move(parsed);
    return true;
  }

  std::vector<uint8_t> Probe(uint32_t ssrc, uint64_t ntp, int64_t now_us) {
    std::lock_guard lock(mutex_);
    pending_.push_back({Compact(ntp), now_us});
    while (pending_.size() > 8) pending_.pop_front();
    auto packet = Header(ssrc, 20);
    packet[8] = 4;
    packet[11] = 2;
    Write32(packet.data() + 12, ntp >> 32);
    Write32(packet.data() + 16, uint32_t(ntp));
    return packet;
  }

  static std::vector<uint8_t> Reply(uint32_t local_ssrc, uint32_t receiver_ssrc,
                                    uint64_t ntp, int64_t delay_us,
                                    uint64_t reply_ntp = 0) {
    auto packet = Header(local_ssrc, reply_ntp != 0 ? 36 : 24);
    packet[8] = 5;
    packet[11] = 3;
    Write32(packet.data() + 12, receiver_ssrc);
    Write32(packet.data() + 16, Compact(ntp));
    Write32(packet.data() + 20, uint64_t(delay_us) * 65536 / 1'000'000);
    if (reply_ntp != 0) {
      packet[24] = 4;
      packet[27] = 2;
      Write32(packet.data() + 28, reply_ntp >> 32);
      Write32(packet.data() + 32, uint32_t(reply_ntp));
    }
    return packet;
  }

  void Reset() {
    std::lock_guard lock(mutex_);
    pending_.clear();
  }

  std::optional<int64_t> Receive(const Dlrr& reply, uint32_t local_ssrc,
                                 int64_t now_us) {
    if (reply.ssrc != local_ssrc || reply.last_rr == 0) return std::nullopt;
    std::lock_guard lock(mutex_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->compact_ntp != reply.last_rr) continue;
      const int64_t elapsed = now_us - it->sent_us;
      pending_.erase(it);  // A replay must not become another clock sample.
      const int64_t delay = uint64_t(reply.delay) * 1'000'000 / 65536;
      if (elapsed < 0 || elapsed > 5'000'000 || delay > elapsed ||
          elapsed - delay > 2'000'000)
        return std::nullopt;
      return elapsed - delay;
    }
    return std::nullopt;
  }

 private:
  static uint32_t Compact(uint64_t ntp) { return uint32_t(ntp >> 16); }
  static uint32_t Read32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | p[3];
  }
  static void Write32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
  }
  static std::vector<uint8_t> Header(uint32_t ssrc, size_t size) {
    std::vector<uint8_t> packet(size);
    packet[0] = 0x80;
    packet[1] = 207;
    packet[3] = size / 4 - 1;
    Write32(packet.data() + 4, ssrc);
    return packet;
  }
  struct ProbeTime {
    uint32_t compact_ntp;
    int64_t sent_us;
  };
  std::mutex mutex_;
  std::deque<ProbeTime> pending_;
};
}  // namespace minirtc

#endif
