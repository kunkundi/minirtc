#include "web_media_handler.h"

#include <algorithm>
#include <chrono>
#include <limits>

#include "log.h"

namespace minirtc {
namespace {
constexpr size_t kMaxQueuedBytes = 512 * 1024;
constexpr size_t kMaxCacheBytes = 2 * 1024 * 1024;
constexpr size_t kPacketOverhead = 64;
// Negotiated in DataChannelConnection::AddVideo. Timestamp at pacer departure,
// so browser delay estimation does not mistake our pacing for network queuing.
constexpr uint8_t kAbsSendTimeId = 3;
void AddSendTimeExtension(::rtc::Message& packet) {
  if (packet.size() < 12) return;
  auto* p = reinterpret_cast<uint8_t*>(packet.data());
  const size_t offset = 12 + (p[0] & 15) * 4;
  if ((p[0] & 16) || offset > packet.size()) return;
  packet.insert(packet.begin() + offset, 8, std::byte{0});
  p = reinterpret_cast<uint8_t*>(packet.data());
  p[0] |= 16;
  p[offset] = 0xbe;
  p[offset + 1] = 0xde;
  p[offset + 3] = 1;
  p[offset + 4] = (kAbsSendTimeId << 4) | 2;
}
void StampSendTime(::rtc::Message& packet) {
  auto* p = reinterpret_cast<uint8_t*>(packet.data());
  const size_t offset = 12 + (p[0] & 15) * 4;
  if (!(p[0] & 16) || offset + 8 > packet.size() || p[offset] != 0xbe ||
      p[offset + 1] != 0xde || p[offset + 4] != ((kAbsSendTimeId << 4) | 2))
    return;
  // Absolute Send Time is the middle 24 bits of NTP (6.18). The Unix/NTP
  // epoch difference is an exact multiple of its 64-second wrap interval.
  const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const uint32_t stamp =
      ((us / 1000000 & 63) << 18) | ((us % 1000000 << 18) / 1000000);
  p[offset + 5] = stamp >> 16;
  p[offset + 6] = stamp >> 8;
  p[offset + 7] = stamp;
}
uint16_t Read16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }
uint32_t Read32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         p[3];
}
uint64_t PacketKey(uint32_t ssrc, uint16_t seq) {
  return uint64_t(ssrc) << 16 | seq;
}
bool IsKeyFrame(const ::rtc::Message& packet) {
  const auto* p = reinterpret_cast<const uint8_t*>(packet.data());
  if (packet.size() < 12) return false;
  size_t offset = 12 + (p[0] & 15) * 4;
  if (p[0] & 16) {
    if (offset + 4 > packet.size()) return false;
    offset += 4 + Read16(p + offset + 2) * 4;
  }
  if (offset >= packet.size()) return false;
  const int type = p[offset] & 31;
  if (type == 5) return true;
  if (type == 28 && offset + 1 < packet.size())
    return (p[offset + 1] & 0x9f) == 0x85;  // IDR FU-A start.
  if (type == 24) {
    for (++offset; offset + 2 < packet.size();) {
      const size_t size = Read16(p + offset);
      offset += 2;
      if (!size || size > packet.size() - offset) break;
      if ((p[offset] & 31) == 5) return true;
      offset += size;
    }
  }
  return false;
}
}  // namespace

int64_t WebMediaHandler::NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

WebMediaHandler::WebMediaHandler() : qos_(NowMs()) {
  budget_ = qos_.Update(NowMs(), 0);
  worker_ = std::thread([this] { Run(); });
}
WebMediaHandler::~WebMediaHandler() { Stop(); }

void WebMediaHandler::Stop() {
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
  std::lock_guard lock(mutex_);
  packets_.clear();
  cache_.clear();
  queued_bytes_ = cache_bytes_ = 0;
}

void WebMediaHandler::RegisterStream(
    uint32_t ssrc, std::shared_ptr<::rtc::RtcpSrReporter> reporter) {
  std::lock_guard lock(mutex_);
  streams_.try_emplace(ssrc).first->second.reporter = std::move(reporter);
}

int WebMediaHandler::QueueDelayLocked(int64_t now_ms) const {
  if (packets_.empty()) return 0;
  const int64_t drain_ms =
      queued_bytes_ * 8000 / std::max(1, budget_.network_bps);
  return static_cast<int>(std::min<int64_t>(
      10000, std::max(drain_ms, now_ms - packets_.front().enqueued_ms)));
}

int WebMediaHandler::QueueDelayMs() {
  std::lock_guard lock(mutex_);
  return QueueDelayLocked(NowMs());
}

void WebMediaHandler::ObserveSenderReport(const ::rtc::message_ptr& message) {
  if (!message || message->size() < 28) return;
  const auto* p = reinterpret_cast<const uint8_t*>(message->data());
  if (p[1] != 200) return;
  std::lock_guard lock(mutex_);
  auto stream = streams_.find(Read32(p + 4));
  if (stream == streams_.end() || stopped_) return;
  auto& reports = stream->second.sender_reports;
  if (reports.size() >= 16) reports.clear();
  reports[Read32(p + 10)] = NowMs();
}

bool WebMediaHandler::CanEncode(uint32_t ssrc) {
  std::lock_guard lock(mutex_);
  const auto it = streams_.find(ssrc);
  if (stopped_ || it == streams_.end()) return false;
  const auto now = NowMs();
  it->second.active_ms = now;
  return QueueDelayLocked(now) < 50;
}

bool WebMediaHandler::AdmitAudio(size_t payload_size) {
  std::lock_guard lock(mutex_);
  const size_t cost = payload_size + 12 + kPacketOverhead;
  if (stopped_ || audio_tokens_ < cost) return false;
  audio_tokens_ -= cost;
  // Audio is sent immediately; debit the aggregate pacer instead of adding
  // delay to speech. Video repays at most one audio packet of token debt.
  tokens_ -= cost;
  return true;
}

bool WebMediaHandler::TakeKeyFrameRequest(uint32_t ssrc) {
  std::lock_guard lock(mutex_);
  auto it = streams_.find(ssrc);
  if (it == streams_.end()) return false;
  auto& stream = it->second;
  const auto now = NowMs();
  if ((!stream.request_keyframe && !stream.waiting_for_keyframe) ||
      (stream.last_keyframe_request_ms >= 0 &&
       now - stream.last_keyframe_request_ms < 500))
    return false;
  stream.last_keyframe_request_ms = now;
  stream.request_keyframe = false;
  return true;
}

WebQosController::Budget WebMediaHandler::Budget(uint32_t ssrc) {
  std::lock_guard lock(mutex_);
  auto budget = budget_;
  if (ssrc) {
    const auto now = NowMs();
    int active = 0;
    for (const auto& [id, state] : streams_)
      if (state.active_ms >= 0 && now - state.active_ms < 1000) ++active;
    budget.video_bps = std::max(16000, budget.video_bps / std::max(1, active));
    const auto it = streams_.find(ssrc);
    if (it != streams_.end()) budget.decode_scale *= it->second.recovery_scale;
  }
  return budget;
}

void WebMediaHandler::OnTelemetry(int64_t lost, int64_t expected, int rtt_ms,
                                  double jitter_ms, double decode_ms,
                                  double fps, bool frozen) {
  std::lock_guard lock(mutex_);
  if (stopped_) return;
  const auto now = NowMs();
  qos_.OnTelemetry(lost, expected, rtt_ms, jitter_ms, decode_ms, fps, frozen,
                   now);
  if (frozen) {
    for (auto& [ssrc, stream] : streams_) {
      if (stream.last_freeze_request_ms < 0 ||
          now - stream.last_freeze_request_ms >= 2000) {
        stream.request_keyframe = true;
        stream.last_freeze_request_ms = now;
      }
    }
  }
}

void WebMediaHandler::RecoverLocked(uint32_t ssrc, int64_t now_ms,
                                    bool oversized) {
  auto& stream = streams_.at(ssrc);
  stream.waiting_for_keyframe = stream.request_keyframe = true;
  if (oversized) {
    stream.recovery_scale = std::max(0.125, stream.recovery_scale * 0.5);
    stream.last_recovery_ms = now_ms;
  }
  for (auto it = packets_.begin(); it != packets_.end();) {
    if (it->ssrc == ssrc) {
      queued_bytes_ -= it->message->size() + kPacketOverhead;
      it = packets_.erase(it);
    } else
      ++it;
  }
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->second.packet.ssrc == ssrc) {
      cache_bytes_ -= it->second.packet.message->size();
      it = cache_.erase(it);
    } else
      ++it;
  }
}

void WebMediaHandler::outgoing(::rtc::message_vector& messages,
                               const ::rtc::message_callback& send) {
  std::lock_guard lock(mutex_);
  if (stopped_) {
    messages.clear();
    return;
  }
  const auto now = NowMs();
  // A packetizer invocation contains one complete encoded access unit.
  size_t bytes = 0;
  uint32_t ssrc = 0;
  bool keyframe = false;
  for (const auto& message : messages) {
    if (message->type == ::rtc::Message::Control || message->size() < 12)
      continue;
    AddSendTimeExtension(*message);
    const auto* p = reinterpret_cast<const uint8_t*>(message->data());
    ssrc = Read32(p + 8);
    bytes += message->size() + kPacketOverhead;
    keyframe |= IsKeyFrame(*message);
  }
  auto it = streams_.find(ssrc);
  const int deadline_ms = keyframe ? 1200 : 250;
  bool accept = it != streams_.end() && bytes > 0 &&
                (!it->second.waiting_for_keyframe || keyframe);
  if (accept &&
      (bytes + queued_bytes_ > kMaxQueuedBytes ||
       (bytes + queued_bytes_) * 8000 / std::max(1, budget_.network_bps) >
           deadline_ms)) {
    RecoverLocked(ssrc, now, true);
    accept = false;
  }
  if (accept && keyframe) {
    RecoverLocked(ssrc, now, false);
    it->second.waiting_for_keyframe = it->second.request_keyframe = false;
  }
  for (auto msg = messages.begin(); msg != messages.end();) {
    if ((*msg)->type == ::rtc::Message::Control) {
      // SR is generated before pacing; remember its actual local departure
      // time instead of subtracting unrelated browser and host wall clocks.
      const auto* p = reinterpret_cast<const uint8_t*>((*msg)->data());
      if ((*msg)->size() >= 28 && p[1] == 200) {
        const auto stream = streams_.find(Read32(p + 4));
        if (stream != streams_.end()) {
          auto& reports = stream->second.sender_reports;
          if (reports.size() >= 16) reports.clear();
          reports[Read32(p + 10)] = now;
        }
      }
      ++msg;
      continue;
    }
    if (accept && (*msg)->size() >= 12) {
      queued_bytes_ += (*msg)->size() + kPacketOverhead;
      packets_.push_back({*msg, send, ssrc, now, now + deadline_ms});
    }
    msg = messages.erase(msg);
  }
  wake_.notify_one();
}

void WebMediaHandler::RequestRepairLocked(uint32_t ssrc, uint16_t sequence,
                                          int64_t now_ms) {
  auto it = cache_.find(PacketKey(ssrc, sequence));
  if (it == cache_.end()) return;
  auto& cached = it->second;
  const size_t cost = cached.packet.message->size() + kPacketOverhead;
  if (cached.repairs >= 2 || now_ms - cached.sent_ms > 600 ||
      cached.packet.deadline_ms <= now_ms ||
      (cached.last_repair_ms >= 0 &&
       now_ms - cached.last_repair_ms < std::max(30, budget_.rtt_ms)) ||
      repair_tokens_ < cost || queued_bytes_ + cost > kMaxQueuedBytes)
    return;
  repair_tokens_ -= cost;
  cached.last_repair_ms = now_ms;
  ++cached.repairs;
  auto packet = cached.packet;
  packet.repair = true;
  packet.enqueued_ms = now_ms;
  packets_.push_front(std::move(packet));
  queued_bytes_ += cost;
}

void WebMediaHandler::HandleRtcpLocked(const uint8_t* data, size_t size,
                                       int64_t now_ms) {
  for (size_t offset = 0; offset + 4 <= size;) {
    const auto* p = data + offset;
    const size_t length = (static_cast<size_t>(Read16(p + 2)) + 1) * 4;
    if ((p[0] >> 6) != 2 || length < 4 || length > size - offset) return;
    size_t payload_size = length;
    if (p[0] & 32) {
      const size_t padding = p[length - 1];
      if (!padding || padding > length - 4) return;
      payload_size -= padding;
    }
    const int count = p[0] & 31;
    if (p[1] == 201 || p[1] == 200) {
      const size_t first = p[1] == 201 ? 8 : 28;
      for (int i = 0; i < count && first + (i + 1) * 24 <= payload_size; ++i) {
        const auto* block = p + first + i * 24;
        auto it = streams_.find(Read32(block));
        if (it == streams_.end()) continue;
        auto& stream = it->second;
        const uint32_t sequence = Read32(block + 8);
        int32_t lost = int32_t(Read32(block + 4) & 0xffffff);
        if (lost & 0x800000) lost -= 0x1000000;
        int rtt_ms = 0;
        const auto sr = stream.sender_reports.find(Read32(block + 16));
        if (sr != stream.sender_reports.end()) {
          const int64_t rtt =
              now_ms - sr->second - int64_t(Read32(block + 20)) * 1000 / 65536;
          if (rtt > 0 && rtt <= 10000) rtt_ms = static_cast<int>(rtt);
        }
        const uint32_t expected = sequence - stream.reported_sequence;
        if (stream.has_report && expected > 0 && expected <= 1000000)
          qos_.OnReceiverReport(int64_t(lost) - stream.reported_loss, expected,
                                rtt_ms, now_ms);
        else if (!stream.has_report || rtt_ms > 0)
          qos_.OnReceiverReport(0, 0, rtt_ms, now_ms);
        if (!stream.has_report || (expected > 0 && expected <= 1000000)) {
          stream.has_report = true;
          stream.reported_sequence = sequence;
          stream.reported_loss = lost;
        }
      }
    } else if (p[1] == 206 && count == 15 && payload_size >= 20 &&
               p[12] == 'R' && p[13] == 'E' && p[14] == 'M' && p[15] == 'B') {
      const size_t sources = p[16];
      if (20 + sources * 4 <= payload_size) {
        bool relevant = false;
        for (size_t i = 0; i < sources; ++i)
          relevant |= streams_.count(Read32(p + 20 + i * 4)) != 0;
        if (relevant) {
          const unsigned exponent = p[17] >> 2;
          const uint64_t mantissa =
              (uint32_t(p[17] & 3) << 16) | Read16(p + 18);
          const uint64_t rate =
              exponent >= 32 ? uint64_t(INT32_MAX) : mantissa << exponent;
          qos_.OnReceiverEstimate(
              static_cast<int>(std::min<uint64_t>(rate, INT32_MAX)), now_ms);
        }
      }
    } else if (p[1] == 206 && payload_size >= 12) {
      if (count == 1) {
        auto it = streams_.find(Read32(p + 8));
        if (it != streams_.end()) it->second.request_keyframe = true;
      } else if (count == 4) {
        for (size_t i = 12; i + 8 <= payload_size; i += 8) {
          auto it = streams_.find(Read32(p + i));
          if (it != streams_.end()) it->second.request_keyframe = true;
        }
      }
    } else if (p[1] == 205 && count == 1 && payload_size >= 16) {
      const uint32_t ssrc = Read32(p + 8);
      if (streams_.count(ssrc)) {
        for (size_t i = 12; i + 4 <= payload_size; i += 4) {
          const uint16_t seq = Read16(p + i), mask = Read16(p + i + 2);
          RequestRepairLocked(ssrc, seq, now_ms);
          for (int bit = 0; bit < 16; ++bit)
            if (mask & (1 << bit))
              RequestRepairLocked(ssrc, seq + bit + 1, now_ms);
        }
      }
    }
    offset += length;
  }
}

void WebMediaHandler::incoming(::rtc::message_vector& messages,
                               const ::rtc::message_callback&) {
  std::lock_guard lock(mutex_);
  if (stopped_) return;
  for (const auto& message : messages)
    if (message->type == ::rtc::Message::Control)
      HandleRtcpLocked(reinterpret_cast<const uint8_t*>(message->data()),
                       message->size(), NowMs());
}

void WebMediaHandler::Run() {
  auto previous = NowMs();
  auto last_log = previous;
  std::unique_lock lock(mutex_);
  while (!stopped_) {
    wake_.wait_for(lock, std::chrono::milliseconds(5));
    if (stopped_) break;
    const auto now = NowMs();
    const int delay = QueueDelayLocked(now);
    budget_ = qos_.Update(now, delay);
    const double elapsed = std::clamp<int64_t>(now - previous, 0, 50) / 1000.0;
    previous = now;
    const double bytes_per_second = budget_.network_bps / 8.0;
    audio_tokens_ =
        std::min(1200.0, audio_tokens_ + budget_.audio_bps / 8.0 * elapsed);
    // At most 10 ms of credit (or two MTU-sized packets), even after idle.
    tokens_ = std::min(std::max(2400.0, bytes_per_second * 0.01),
                       tokens_ + bytes_per_second * elapsed);
    repair_tokens_ =
        std::min(std::max(1500.0, bytes_per_second * 0.04),
                 repair_tokens_ + bytes_per_second * 0.20 * elapsed);
    for (auto it = cache_.begin(); it != cache_.end();) {
      if (now - it->second.sent_ms > 600 ||
          it->second.packet.deadline_ms <= now ||
          cache_bytes_ > kMaxCacheBytes) {
        cache_bytes_ -= it->second.packet.message->size();
        it = cache_.erase(it);
      } else
        ++it;
    }
    for (auto& [ssrc, state] : streams_) {
      if (state.recovery_scale < 1 && state.last_recovery_ms >= 0 &&
          now - state.last_recovery_ms > 5000 && delay < 50 &&
          budget_.loss_ratio <= 0.02 && budget_.feedback_age_ms < 1500) {
        state.recovery_scale = std::min(1.0, state.recovery_scale + 0.125);
        state.last_recovery_ms = now;
      }
    }
    while (!packets_.empty() && !stopped_) {
      if (packets_.front().deadline_ms <= now) {
        if (packets_.front().repair) {
          queued_bytes_ -= packets_.front().message->size() + kPacketOverhead;
          packets_.pop_front();
        } else {
          RecoverLocked(packets_.front().ssrc, now, true);
        }
        continue;
      }
      const size_t cost = packets_.front().message->size() + kPacketOverhead;
      if (tokens_ < cost) break;
      auto packet = std::move(packets_.front());
      packets_.pop_front();
      queued_bytes_ -= cost;
      tokens_ -= cost;
      if (!packet.repair) {
        StampSendTime(*packet.message);
        auto* p = reinterpret_cast<uint8_t*>(packet.message->data());
        auto& stream = streams_.at(packet.ssrc);
        // Discarded unsent frames must not create RTP gaps and artificial loss
        // feedback. Repairs retain the sequence assigned at their first send.
        if (!stream.sequence_started) {
          stream.next_sequence = Read16(p + 2);
          stream.sequence_started = true;
        }
        const uint16_t sequence = stream.next_sequence++;
        p[2] = sequence >> 8;
        p[3] = sequence & 0xff;
        const uint64_t key = PacketKey(packet.ssrc, Read16(p + 2));
        auto old = cache_.find(key);
        if (old != cache_.end())
          cache_bytes_ -= old->second.packet.message->size();
        cache_[key] = {packet, now};
        cache_bytes_ += packet.message->size();
      }
      auto reporter = streams_.at(packet.ssrc).reporter;
      lock.unlock();
      bool failed = false;
      try {
        packet.send(packet.message);
        if (!packet.repair && reporter) {
          ::rtc::message_vector sent{packet.message};
          reporter->outgoing(sent, [this, &packet](::rtc::message_ptr report) {
            ObserveSenderReport(report);
            packet.send(std::move(report));
          });
        }
      } catch (const std::exception&) {
        failed = true;
      }
      lock.lock();
      if (failed) RecoverLocked(packet.ssrc, now, false);
    }
    if (now - last_log >= 5000 && !streams_.empty()) {
      LOG_DEBUG(
          "Web QoS: network={} video={} fps={} queue_ms={} loss={} rtt={} "
          "receiver={} "
          "feedback_age={}",
          budget_.network_bps, budget_.video_bps, budget_.max_fps, delay,
          budget_.loss_ratio, budget_.rtt_ms, budget_.receiver_bps,
          budget_.feedback_age_ms);
      last_log = now;
    }
  }
}

}  // namespace minirtc
