/*
 * @Author: DI JUNKUN
 * @Date: 2025-09-25
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RTP_FEC_H_
#define _RTP_FEC_H_

#include <map>
#include <memory>
#include <string>

#include "fec_decoder.h"
#include "fec_adaptation_controller.h"

namespace minirtc {
// Private minirtc RS wire format; not RFC 5109 ULPFEC or RFC 8627 FlexFEC.
// Only enabled after both endpoints negotiate rs-v1 and an RTX SSRC.
constexpr uint8_t kRsFecPayloadType = 122;
constexpr size_t kFecMaxMediaPacket = 1148;
constexpr size_t kFecMaxWirePacket = 1184;  // +16-byte SRTP tag <=1200
constexpr int64_t kFecWindowMs = 150;
constexpr size_t kFecMaxSources = 16;
constexpr size_t kFecMaxRepairs = 4;
constexpr size_t kFecHeaderSize = 16;
constexpr char kFecSdpAttribute[] = "a=x-minirtc-fec:rs-v1 122";

bool SupportsRsFec(const std::string& media_section);
bool SupportsOpusFec(const std::string& media_section);

struct FecTransportStats {
  uint64_t source_packets = 0;
  uint64_t repair_packets = 0;
  uint64_t recovered_packets = 0;
  uint64_t expired_blocks = 0;
  uint64_t rejected_packets = 0;
  uint64_t budget_skips = 0;
  uint64_t codec_errors = 0;
};

// Call after final sequence/extension assignment, before SRTP. A single sender
// belongs to one media SSRC and one pacer queue. Source packets are unchanged.
class RtpFecSender {
 public:
  FecSymbols AddPacket(const uint8_t* packet, size_t size, uint32_t repair_ssrc,
                       int64_t now_ms, bool keyframe = false);
  void SetProtection(const FecProtectionConfig& config, int64_t now_ms);
  FecSymbols FlushExpired(uint32_t repair_ssrc, int64_t now_ms);
  int64_t PendingSinceMs() const { return pending_.empty() ? -1 : first_ms_; }
  int64_t RepairDeadlineMs() const { return repair_deadline_ms_; }
  void Reset();
  const FecTransportStats& Stats() const { return stats_; }

 private:
  FecSymbols Flush(uint32_t repair_ssrc, int64_t now_ms);
  FecSymbols pending_;
  FecSymbols source_scratch_, repair_scratch_;
  int64_t first_ms_ = 0;
  double credit_bytes_ = 0;
  FecProtectionConfig config_;
  FecProtectionConfig block_config_;
  FecRateBudget rate_budget_;
  bool keyframe_ = false;
  int64_t last_packet_ms_ = -1, repair_deadline_ms_ = -1;
  FecTransportStats stats_;
};

// Owns all received bytes. Bounds: 512 media packets, 64 blocks, 150 ms.
// Call only after authentication/demux and from the serialized receive path.
// Restored packets bypass SRTP replay checking but must still pass RTP parsing.
class RtpFecReceiver {
 public:
  RtpFecReceiver(uint32_t media_ssrc, uint32_t repair_ssrc, uint8_t media_pt);
  ~RtpFecReceiver();
  FecSymbols AddPacket(const uint8_t* packet, size_t size, int64_t now_ms);
  void Expire(int64_t now_ms);
  const FecTransportStats& Stats() const { return stats_; }
  size_t CachedPackets() const { return sources_.size(); }
  size_t PendingBlocks() const { return blocks_.size(); }

 private:
  struct Source {
    std::vector<uint8_t> bytes;
    int64_t arrival;
  };
  struct Block;
  using Key = std::pair<uint32_t, uint16_t>;  // RTP timestamp, base sequence
  void TryDecode(Block& block, FecSymbols* recovered);
  void Cache(const std::vector<uint8_t>& packet, int64_t now_ms);
  uint32_t media_ssrc_;
  uint32_t repair_ssrc_;
  uint8_t media_pt_;
  std::map<uint16_t, Source> sources_;
  std::map<Key, std::unique_ptr<Block>> blocks_;
  FecTransportStats stats_;
  FecDecoder decoder_;
};
}  // namespace minirtc
#endif
