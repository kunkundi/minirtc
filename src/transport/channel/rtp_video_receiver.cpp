#include "rtp_video_receiver.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include "common.h"
#include "fir.h"
#include "log.h"
#include "nack.h"
#include "rtcp_sender.h"
#include "video_recovery_timing.h"

// #define SAVE_RTP_RECV_STREAM

#define NV12_BUFFER_SIZE (1280 * 720 * 3 / 2)

namespace minirtc {
namespace {

constexpr uint32_t kMaxPacketsPerFrame = 4096;
// A worst-case NACK needs one four-byte PID/BLP field per packet id. Keeping a
// batch at 256 ids guarantees that one feedback block fits in the 1200-byte
// RTCP sender buffer, so every transport result maps to exactly one id batch.
constexpr size_t kMaxNackIdsPerRtcpPacket = 256;

uint32_t FramePacketCount(uint16_t start_sequence_number,
                          uint16_t end_sequence_number) {
  const uint32_t packet_count =
      static_cast<uint16_t>(end_sequence_number - start_sequence_number) + 1u;
  return packet_count <= kMaxPacketsPerFrame ? packet_count : 0;
}

uint16_t SequenceNumberAt(uint16_t start_sequence_number, uint32_t offset) {
  return static_cast<uint16_t>(start_sequence_number + offset);
}

constexpr int64_t kMaxFrameAssemblyMs = 1500;

bool IsH264KeyFrame(const uint8_t* data, size_t size) {
  if (!data) {
    return false;
  }
  for (size_t offset = 0; offset + 3 < size; ++offset) {
    size_t nal_offset = 0;
    if (data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 1) {
      nal_offset = offset + 3;
    } else if (offset + 4 < size && data[offset] == 0 &&
               data[offset + 1] == 0 && data[offset + 2] == 0 &&
               data[offset + 3] == 1) {
      nal_offset = offset + 4;
    }
    if (nal_offset != 0 && nal_offset < size &&
        (data[nal_offset] & 0x1f) == 5) {
      return true;
    }
  }
  return false;
}

}  // namespace

RtpVideoReceiver::RtpVideoReceiver(std::shared_ptr<SystemClock> clock)
    : RtpVideoReceiver(std::move(clock), nullptr) {}

RtpVideoReceiver::RtpVideoReceiver(std::shared_ptr<SystemClock> clock,
                                   std::shared_ptr<IOStatistics> io_statistics)
    : io_statistics_(io_statistics),
      is_running_(true),
      ssrc_(GenerateUniqueSsrc()),
      system_clock_(clock),
      clock_(webrtc::Clock::GetWebrtcClockShared(clock)),
      receive_side_congestion_controller_(
          clock_,
          [this](std::vector<std::unique_ptr<RtcpPacket>> packets) {
            SendCombinedRtcpPacket(std::move(packets));
          },
          [this](int64_t bitrate_bps, std::vector<uint32_t> ssrcs) {
            SendRemb(bitrate_bps, ssrcs);
          }),
      rtcp_sender_(std::make_unique<RtcpSender>(
          [this](const uint8_t* buffer, size_t size) -> int {
            if (!data_send_func_ || rtcp_stop_.load()) {
              return -1;
            }
            return data_send_func_((const char*)buffer, size);
          },
          1200)),
      nack_(std::make_unique<NackRequester>(clock_)) {
  SetPeriod(std::chrono::milliseconds(5));
  SetThreadName("RtpVideoReceiver");
  last_recovery_stats_log_ms_ = clock_->CurrentTime().ms();
  rtcp_thread_ = std::thread(&RtpVideoReceiver::RtcpThread, this);

#ifdef SAVE_RTP_RECV_STREAM
  file_rtp_recv_ = fopen("rtp_recv_stream.h264", "w+b");
  if (!file_rtp_recv_) {
    LOG_WARN("Fail to open rtp_recv_stream.h264");
  }
#endif
}

RtpVideoReceiver::~RtpVideoReceiver() {
  StopRtcp();
  Stop();
  MaybeLogRecoveryStats(true);

  SSRCManager::Instance().DeleteSsrc(ssrc_);

  delete[] nv12_data_;

  incomplete_h264_frame_list_.clear();
  incomplete_av1_frame_list_.clear();
  incomplete_h265_packets_.clear();
  incomplete_frame_list_.clear();

  {
    std::lock_guard<std::mutex> lock(pending_frames_mtx_);
    pending_frames_.clear();
  }

#ifdef SAVE_RTP_RECV_STREAM
  if (file_rtp_recv_) {
    fflush(file_rtp_recv_);
    fclose(file_rtp_recv_);
    file_rtp_recv_ = nullptr;
  }
#endif
}

bool RtpVideoReceiver::RestoreMediaPacketFromRtx(RtpPacket& rtx_packet,
                                                 RtpPacket* media_packet) {
  const uint32_t media_ssrc = remote_ssrc_.load();
  if (!media_packet || rtx_packet.PayloadType() != rtp::VideoRtxPayloadType(media_payload_type_) ||
      rtx_packet.PayloadSize() < 2 || rtx_packet.HeaderSize() < kFixedHeaderSize ||
      media_ssrc == 0 || media_payload_type_ == rtp::PAYLOAD_TYPE::UNDEFINED ||
      media_payload_type_ == rtp::PAYLOAD_TYPE::RTX) {
    return false;
  }

  const uint8_t* rtx_payload = rtx_packet.Payload();
  const uint16_t original_sequence_number =
      static_cast<uint16_t>((rtx_payload[0] << 8) | rtx_payload[1]);
  const CopyOnWriteBuffer rtx_buffer = rtx_packet.Buffer();
  const size_t header_size = rtx_packet.HeaderSize();
  const size_t padding_size = rtx_packet.padding_size();

  std::vector<uint8_t> media_buffer;
  media_buffer.reserve(rtx_packet.Size() - 2);
  media_buffer.insert(media_buffer.end(), rtx_buffer.data(),
                      rtx_buffer.data() + header_size);
  media_buffer.insert(media_buffer.end(), rtx_payload + 2,
                      rtx_payload + rtx_packet.PayloadSize());
  if (padding_size > 0) {
    media_buffer.insert(media_buffer.end(),
                        rtx_buffer.data() + rtx_packet.Size() - padding_size,
                        rtx_buffer.data() + rtx_packet.Size());
  }

  media_buffer[1] = static_cast<uint8_t>(
      (media_buffer[1] & 0x80) | static_cast<uint8_t>(media_payload_type_));
  media_buffer[2] = static_cast<uint8_t>(original_sequence_number >> 8);
  media_buffer[3] = static_cast<uint8_t>(original_sequence_number);
  media_buffer[8] = static_cast<uint8_t>(media_ssrc >> 24);
  media_buffer[9] = static_cast<uint8_t>(media_ssrc >> 16);
  media_buffer[10] = static_cast<uint8_t>(media_ssrc >> 8);
  media_buffer[11] = static_cast<uint8_t>(media_ssrc);

  return media_packet->Build(media_buffer.data(), media_buffer.size());
}

void RtpVideoReceiver::TrackPendingFramePacket(uint32_t timestamp,
                                               uint16_t sequence_number,
                                               bool is_keyframe) {
  std::lock_guard<std::mutex> lock(pending_frames_mtx_);
  PendingFrame& pending_frame = pending_frames_[timestamp];
  if (!pending_frame.last_sequence_number) {
    pending_frame.arrival_time = clock_->CurrentTime().ms();
    pending_frame.request_generation =
        keyframe_request_generation_.load(std::memory_order_relaxed);
  }
  pending_frame.is_keyframe |= is_keyframe;
  if (!pending_frame.last_sequence_number.has_value() ||
      webrtc::AheadOf(sequence_number,
                      *pending_frame.last_sequence_number)) {
    pending_frame.last_sequence_number = sequence_number;
    // Duplicates and late repairs do not extend the receive deadline.
    pending_frame.last_progress_time = clock_->CurrentTime().ms();
  }
}

bool RtpVideoReceiver::GetFrameSequenceRange(
    uint32_t timestamp, const char* codec_name,
    uint16_t* start_sequence_number, uint16_t* end_sequence_number,
    uint32_t* packet_count) {
  if (!start_sequence_number || !end_sequence_number || !packet_count) {
    return false;
  }

  auto start_it = fua_start_sequence_numbers_.find(timestamp);
  auto end_it = fua_end_sequence_numbers_.find(timestamp);
  if (start_it == fua_start_sequence_numbers_.end() ||
      end_it == fua_end_sequence_numbers_.end()) {
    return false;
  }

  *start_sequence_number = start_it->second;
  *end_sequence_number = end_it->second;
  *packet_count =
      FramePacketCount(*start_sequence_number, *end_sequence_number);
  if (*packet_count == 0) {
    LOG_WARN("Invalid {} frame sequence range [{} -> {}], timestamp {}",
             codec_name, *start_sequence_number, *end_sequence_number,
             timestamp);
    return false;
  }
  return true;
}

void RtpVideoReceiver::CommitPendingFrame(
    uint32_t timestamp, std::unique_ptr<ReceivedFrame> frame,
    bool is_keyframe, uint16_t end_sequence_number,
    bool require_existing_entry) {
  std::unique_lock<std::mutex> lock(pending_frames_mtx_);
  auto pending_it = pending_frames_.find(timestamp);
  if (require_existing_entry && pending_it == pending_frames_.end()) {
    return;
  }
  if (pending_it == pending_frames_.end()) {
    pending_it = pending_frames_.try_emplace(timestamp).first;
  }
  PendingFrame& pending_frame = pending_it->second;
  const int64_t first_arrival_time =
      pending_frame.arrival_time != 0 ? pending_frame.arrival_time
                                      : clock_->CurrentTime().ms();
  const uint64_t request_generation = pending_frame.last_sequence_number
      ? pending_frame.request_generation
      : keyframe_request_generation_.load(std::memory_order_relaxed);
  pending_frame = {std::move(frame), true, first_arrival_time, is_keyframe,
                   end_sequence_number};
  pending_frame.request_generation = request_generation;
  lock.unlock();
  // Deliver complete frames promptly. The periodic pass also runs while
  // packets are missing or the decoder is applying backpressure.
  RequestProcess();
}

void RtpVideoReceiver::ClearFrameMarkers(uint32_t timestamp) {
  fua_start_sequence_numbers_.erase(timestamp);
  fua_end_sequence_numbers_.erase(timestamp);
}

void RtpVideoReceiver::EnsureFrameBufferCapacity(size_t required_capacity) {
  if (nv12_data_ && frame_buffer_capacity_ >= required_capacity) {
    return;
  }
  delete[] nv12_data_;
  frame_buffer_capacity_ =
      std::max(required_capacity, static_cast<size_t>(NV12_BUFFER_SIZE));
  nv12_data_ = new uint8_t[frame_buffer_capacity_];
}

std::unique_ptr<ReceivedFrame> RtpVideoReceiver::CreateReceivedFrame(
    const uint8_t* data, size_t size, uint32_t timestamp) {
  auto frame = std::make_unique<ReceivedFrame>(data, size);
  const int64_t received_time_us = system_clock_->CurrentTimeUs();
  frame->SetReceivedTimestamp(received_time_us);
  frame->SetCapturedTimestamp(
      rtp_timestamp_mapper_.Estimate(timestamp, received_time_us).value_or(0));
  return frame;
}

void RtpVideoReceiver::SetFecEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(fec_mtx_);
  fec_receiver_.reset();
  if (enabled && RtxEnabled())
    fec_receiver_ = std::make_unique<RtpFecReceiver>(
        remote_ssrc_.load(), rtx_ssrc_.load(), uint8_t(media_payload_type_));
}

void RtpVideoReceiver::InsertRtpPacket(RtpPacket& packet) {
  const bool repair = packet.PayloadType() == rtp::PAYLOAD_TYPE::RS_FEC;
  const bool obsolete_repair = repair && IsFrameTimestampObsolete(packet.Timestamp());
  FecSymbols recovered;
  {
    std::lock_guard<std::mutex> lock(fec_mtx_);
    if (repair && (!fec_receiver_ || packet.Ssrc() != rtx_ssrc_.load())) return;
    if (fec_receiver_ && !obsolete_repair &&
        (repair || (packet.PayloadType() == media_payload_type_ &&
                    packet.Ssrc() == remote_ssrc_.load()))) {
      recovered = fec_receiver_->AddPacket(
          packet.Buffer().data(), packet.Size(), clock_->CurrentTime().ms());
    }
  }
  // FEC, RTX and padding share the auxiliary sequence space. Report actual
  // arrivals, including late repairs, so successful RTX is not misreported as
  // loss between two repair packets. Reconstructed media is never fed to BWE.
  const bool actual_rtx = packet.PayloadType() == rtp::VideoRtxPayloadType(media_payload_type_) &&
                          rtx_ssrc_.load() != 0 && packet.Ssrc() == rtx_ssrc_.load();
  if (repair || actual_rtx) {
    webrtc::RtpPacketReceived received;
    if (received.Build(packet.Buffer().data(), packet.Size())) {
      received.set_arrival_time(clock_->CurrentTime());
      received.set_payload_type_frequency(kVideoPayloadTypeFrequency);
      received.SetAbsoluteSendTimeExtensionId(abs_send_time_ext_id_);
      receive_side_congestion_controller_.OnReceivedPacket(received,
                                                           MediaType::VIDEO);
      if (repair && io_statistics_)
        io_statistics_->UpdateVideoInboundBytes(static_cast<uint32_t>(packet.Size()));
    }
  }
  for (const auto& bytes : recovered) {
    RtpPacket restored;
    if (restored.Build(bytes.data(), static_cast<uint32_t>(bytes.size())))
      InsertMediaPacket(restored, true);
  }
  if (!repair) InsertMediaPacket(packet);
}

void RtpVideoReceiver::InsertMediaPacket(RtpPacket& rtp_packet,
                                         bool fec_recovered) {
  const bool is_rtx = rtp_packet.PayloadType() == rtp::VideoRtxPayloadType(media_payload_type_);
  const bool is_recovered = fec_recovered || is_rtx;
  const uint32_t negotiated_rtx_ssrc = rtx_ssrc_.load();
  if (is_rtx &&
      (negotiated_rtx_ssrc == 0 || rtp_packet.Ssrc() != negotiated_rtx_ssrc)) {
    LOG_WARN("Dropping unnegotiated RTX packet from SSRC {}",
             rtp_packet.Ssrc());
    return;
  }
  RtpPacket restored_media_packet;
  RtpPacket* media_packet = &rtp_packet;
  if (is_rtx) {
    if (!RestoreMediaPacketFromRtx(rtp_packet, &restored_media_packet)) {
      LOG_WARN("Failed to restore media packet from RTX SSRC {}",
               rtp_packet.Ssrc());
      return;
    }
    media_packet = &restored_media_packet;
  }

  const rtp::PAYLOAD_TYPE payload_type = media_packet->PayloadType();
  const bool is_media_payload = payload_type == media_payload_type_;
  if (is_media_payload &&
      IsFrameTimestampObsolete(media_packet->Timestamp())) {
    recovery_obsolete_packets_.fetch_add(1, std::memory_order_relaxed);
    if (is_recovered) {
      // A late RTX packet still satisfies its NACK even though its frame has
      // deliberately been abandoned. Retiring the sequence number prevents
      // needless retries without allowing the old frame to be resurrected.
      std::lock_guard<std::mutex> lock(nack_mtx_);
      nack_->OnReceivedPacket(media_packet->SequenceNumber(), true);
      if (is_rtx)
        recovery_late_rtx_packets_.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  if (is_rtx) {
    recovery_accepted_rtx_packets_.fetch_add(1,
                                             std::memory_order_relaxed);
  }

  webrtc::RtpPacketReceived rtp_packet_received;
  if (!rtp_packet_received.Build(media_packet->Buffer().data(),
                                 media_packet->Size())) {
    return;
  }
  rtp_packet_received.set_arrival_time(clock_->CurrentTime());
  rtp_packet_received.set_ecn(EcnMarking::kEct0);
  rtp_packet_received.set_recovered(is_recovered);
  rtp_packet_received.set_payload_type_frequency(kVideoPayloadTypeFrequency);
  rtp_packet_received.SetAbsoluteSendTimeExtensionId(
      abs_send_time_ext_id_);

  const uint8_t padding_payload_type =
      static_cast<uint8_t>(media_payload_type_) - 1;
  const uint32_t media_ssrc = remote_ssrc_.load();
  const bool is_auxiliary_padding =
      !is_recovered && media_packet->Ssrc() != media_ssrc &&
      static_cast<uint8_t>(media_packet->PayloadType()) == padding_payload_type;
  if (is_auxiliary_padding) {
    // Probe padding intentionally uses the RTX SSRC and sequence space. It
    // contributes to bandwidth estimation but must never update the media
    // sequence tracker or create an unrepairable media NACK gap.
    receive_side_congestion_controller_.OnReceivedPacket(
        rtp_packet_received, MediaType::VIDEO);
    last_recv_bytes_ = static_cast<uint32_t>(rtp_packet.PayloadSize());
    total_rtp_payload_recv_ += last_recv_bytes_;
    ++total_rtp_packets_recv_;
    if (io_statistics_) {
      io_statistics_->UpdateVideoInboundBytes(last_recv_bytes_);
      io_statistics_->IncrementVideoInboundRtpPacketCount();
    }
    return;
  }

  if (!is_recovered) {
    std::lock_guard<std::mutex> stats_lock(receiver_stats_mtx_);
    webrtc::Timestamp now = clock_->CurrentTime();
    uint16_t sequence_number = media_packet->SequenceNumber();
    --cumulative_loss_;
    if (!last_receive_time_.has_value()) {
      last_extended_high_seq_num_ = sequence_number - 1;
      extended_high_seq_num_ = sequence_number - 1;
      last_receive_time_ = now;
    }

    cumulative_loss_ += sequence_number - extended_high_seq_num_;
    extended_high_seq_num_ = sequence_number;

    if (rtp_packet_received.Timestamp() != last_received_timestamp_) {
      webrtc::TimeDelta receive_diff = now - *last_receive_time_;
      uint32_t receive_diff_rtp =
          (receive_diff * rtp_packet_received.payload_type_frequency())
              .seconds<uint32_t>();
      int32_t time_diff_samples =
          receive_diff_rtp -
          (rtp_packet_received.Timestamp() - last_received_timestamp_);

      ReviseFrequencyAndJitter(rtp_packet_received.payload_type_frequency());

      // lib_jingle sometimes deliver crazy jumps in TS for the same stream.
      // If this happens, don't update jitter value. Use 5 secs video frequency
      // as the threshold.
      if (time_diff_samples < 5 * kVideoPayloadTypeFrequency &&
          time_diff_samples > -5 * kVideoPayloadTypeFrequency) {
        // Note we calculate in Q4 to avoid using float.
        int32_t jitter_diff_q4 =
            (std::abs(time_diff_samples) << 4) - jitter_q4_;
        jitter_q4_ += ((jitter_diff_q4 + 8) >> 4);
      }

      jitter_ = jitter_q4_ >> 4;
    }

    last_received_timestamp_ = rtp_packet_received.Timestamp();
    last_receive_time_ = now;
    has_received_media_packet_.store(true);
  }

  last_recv_bytes_ = static_cast<uint32_t>(rtp_packet.PayloadSize());
  total_rtp_payload_recv_ += static_cast<uint32_t>(rtp_packet.PayloadSize());
  total_rtp_packets_recv_++;

  if (io_statistics_) {
    io_statistics_->UpdateVideoInboundBytes(last_recv_bytes_);
    io_statistics_->IncrementVideoInboundRtpPacketCount();
    if (!is_recovered) {
      io_statistics_->UpdateVideoPacketLossCount(
          media_packet->SequenceNumber());
    }
  }

  const bool is_media_padding =
      !is_recovered &&
      static_cast<uint8_t>(payload_type) == padding_payload_type;
  if (!is_media_payload && !is_media_padding) {
    return;
  }

  // Sequence tracking is a transport concern and must happen after RTP
  // validation but before codec depacketization. Otherwise one malformed
  // payload hides its sequence number from NACK and can stall discovery of all
  // following losses. A corrupt payload is handled below by switching to a
  // keyframe rather than retransmitting the same bytes forever.
  if (!is_recovered) {
    receive_side_congestion_controller_.OnReceivedPacket(
        rtp_packet_received, MediaType::VIDEO);
  }
  if (RtxEnabled()) {
    std::vector<uint16_t> nack_batch;
    bool request_keyframe_for_nack_overflow = false;
    {
      std::lock_guard<std::mutex> lock(nack_mtx_);
      nack_batch = nack_->OnReceivedPacket(media_packet->SequenceNumber(),
                                           is_recovered);
      request_keyframe_for_nack_overflow = nack_->ConsumeKeyFrameRequest();
    }
    SendPreparedNackBatch(std::move(nack_batch));
    if (request_keyframe_for_nack_overflow) {
      RequestKeyFrame();
    }
  }

  if (is_media_padding) {
    return;
  }

  if (payload_type == rtp::H265) {
    std::lock_guard<std::mutex> lock(frame_assembly_mtx_);
    ProcessH265RtpPacket(*media_packet);
  } else if (payload_type == rtp::PAYLOAD_TYPE::AV1) {
    RtpPacketAv1 rtp_packet_av1;
    if (!rtp_packet_av1.Build(media_packet->Buffer().data(),
                              media_packet->Size()) ||
        !rtp_packet_av1.GetFrameHeaderInfo()) {
      LOG_WARN("Invalid AV1 RTP payload at sequence {}",
               media_packet->SequenceNumber());
      RequestKeyFrame();
      return;
    }
#ifdef SAVE_RTP_RECV_STREAM
    fwrite((unsigned char*)rtp_packet_av1.Payload(), 1,
           rtp_packet_av1.PayloadSize(), file_rtp_recv_);
#endif
    {
      std::lock_guard<std::mutex> lock(frame_assembly_mtx_);
      ProcessAv1RtpPacket(rtp_packet_av1);
    }
  } else if (payload_type == rtp::PAYLOAD_TYPE::H264) {
    RtpPacketH264 rtp_packet_h264;
    if (!rtp_packet_h264.Build(media_packet->Buffer().data(),
                               media_packet->Size()) ||
        !rtp_packet_h264.GetFrameHeaderInfo()) {
      LOG_WARN("Invalid H264 RTP payload at sequence {}",
               media_packet->SequenceNumber());
      RequestKeyFrame();
      return;
    }
#ifdef SAVE_RTP_RECV_STREAM
    fwrite((unsigned char*)rtp_packet_h264.Payload(), 1,
           rtp_packet_h264.PayloadSize(), file_rtp_recv_);
#endif
    {
      std::lock_guard<std::mutex> lock(frame_assembly_mtx_);
      ProcessH264RtpPacket(rtp_packet_h264);
    }
  }
}

void RtpVideoReceiver::ProcessH265RtpPacket(RtpPacket& packet) {
  const uint8_t* payload = packet.Payload();
  const size_t size = packet.PayloadSize();
  if (size < 3 || (payload[0] & 0x80) || !(payload[1] & 7)) return;
  const uint8_t type = (payload[0] >> 1) & 63;
  const bool fu = type == 49;
  if (type >= 48 && !fu) return;
  if (fu &&
      (size < 4 || (payload[2] & 63) >= 48 || (payload[2] & 0xc0) == 0xc0))
    return;
  const uint32_t ts = packet.Timestamp();
  const uint16_t seq = packet.SequenceNumber();
  const uint8_t nal_type = fu ? payload[2] & 63 : type;
  TrackPendingFramePacket(ts, seq, nal_type >= 16 && nal_type <= 21);
  incomplete_h265_packets_[seq] = packet;
  if (type == 35) fua_start_sequence_numbers_[ts] = seq;
  if (packet.Marker()) fua_end_sequence_numbers_[ts] = seq;

  uint16_t start, end;
  uint32_t count;
  if (!GetFrameSequenceRange(ts, "H265", &start, &end, &count)) return;
  size_t capacity = 0;
  for (uint32_t i = 0; i < count; ++i) {
    auto it = incomplete_h265_packets_.find(SequenceNumberAt(start, i));
    if (it == incomplete_h265_packets_.end() || it->second.Timestamp() != ts)
      return;
    capacity += it->second.PayloadSize() + 4;
  }

  std::vector<uint8_t> frame;
  frame.reserve(capacity);
  bool keyframe = false;
  std::optional<uint16_t> open_fu;
  for (uint32_t i = 0; i < count; ++i) {
    auto& part = incomplete_h265_packets_.at(SequenceNumberAt(start, i));
    const auto* data = part.Payload();
    size_t length = part.PayloadSize();
    const bool fragment = ((data[0] >> 1) & 63) == 49;
    const uint8_t nal = fragment ? data[2] & 63 : (data[0] >> 1) & 63;
    const bool starts_nal = !fragment || (data[2] & 0x80);
    const uint8_t header = (data[0] & 0x81) | (nal << 1);
    const uint16_t identity = (header << 8) | data[1];
    if (starts_nal) {
      if (open_fu) return;
      frame.insert(frame.end(), {0, 0, 0, 1, header, data[1]});
      keyframe |= nal >= 16 && nal <= 21;
      if (fragment) open_fu = identity;
    } else if (open_fu != identity) {
      return;
    }
    if (fragment && (data[2] & 0x40)) open_fu.reset();
    frame.insert(frame.end(), data + (fragment ? 3 : 2), data + length);
  }
  if (open_fu) return;
  for (uint32_t i = 0; i < count; ++i)
    incomplete_h265_packets_.erase(SequenceNumberAt(start, i));
  ClearFrameMarkers(ts);
  CommitPendingFrame(ts, CreateReceivedFrame(frame.data(), frame.size(), ts),
                     keyframe, end, true);
}

void RtpVideoReceiver::ProcessH264RtpPacket(RtpPacketH264& rtp_packet_h264) {
  {
    rtp::NAL_UNIT_TYPE nalu_type = rtp_packet_h264.NalUnitType();
    if (rtp::NAL_UNIT_TYPE::NALU == nalu_type) {
      std::vector<uint8_t> bytestream;
      bytestream.reserve(rtp_packet_h264.PayloadSize() + 1);
      uint8_t header = (rtp_packet_h264.ForbiddenBit() << 7) |
                       (rtp_packet_h264.NalRefIdc() << 5) |
                       (uint8_t)rtp_packet_h264.NalUnitType();
      bytestream.push_back(header);
      const uint8_t* payload = rtp_packet_h264.Payload();
      bytestream.insert(bytestream.end(), payload,
                        payload + rtp_packet_h264.PayloadSize());
      std::unique_ptr<ReceivedFrame> received_frame = CreateReceivedFrame(
          bytestream.data(), bytestream.size(), rtp_packet_h264.Timestamp());
      const bool is_keyframe =
          IsH264KeyFrame(received_frame->Buffer(), received_frame->Size());

      CommitPendingFrame(rtp_packet_h264.Timestamp(),
                         std::move(received_frame), is_keyframe,
                         rtp_packet_h264.SequenceNumber(), false);
    } else if (rtp::NAL_UNIT_TYPE::FU_A == nalu_type) {
      incomplete_h264_frame_list_[rtp_packet_h264.SequenceNumber()] =
          rtp_packet_h264;
      CheckIsH264FrameCompleted(rtp_packet_h264,
                                rtp_packet_h264.FuAStart(),
                                rtp_packet_h264.FuAEnd());
    }
  }
}

void RtpVideoReceiver::ProcessAv1RtpPacket(RtpPacketAv1& rtp_packet_av1) {
  {
    incomplete_av1_frame_list_[rtp_packet_av1.SequenceNumber()] =
        rtp_packet_av1;
    CheckIsAv1FrameCompleted(rtp_packet_av1);
  }
}

bool RtpVideoReceiver::CheckIsH264FrameCompleted(RtpPacketH264& rtp_packet_h264,
                                                 bool is_start, bool is_end) {
  uint32_t timestamp = rtp_packet_h264.Timestamp();
  uint16_t seq = rtp_packet_h264.SequenceNumber();
  uint16_t start_seq, end_seq;
  uint32_t packet_count;

  TrackPendingFramePacket(
      timestamp, seq,
      is_start && IsH264KeyFrame(rtp_packet_h264.Payload(),
                                 rtp_packet_h264.PayloadSize()));

  if (is_start) {
    fua_start_sequence_numbers_[timestamp] = seq;
  }

  if (is_end) {
    fua_end_sequence_numbers_[timestamp] = seq;
  }

  if (!GetFrameSequenceRange(timestamp, "H264", &start_seq, &end_seq,
                             &packet_count)) {
    return false;
  }

  for (uint32_t offset = 0; offset < packet_count; ++offset) {
    const uint16_t sequence_number = SequenceNumberAt(start_seq, offset);
    auto packet_it = incomplete_h264_frame_list_.find(sequence_number);
    if (packet_it == incomplete_h264_frame_list_.end() ||
        packet_it->second.Timestamp() != timestamp) {
      return false;
    }
  }

  return PopCompleteFrame(start_seq, end_seq, packet_count, timestamp);
}

bool RtpVideoReceiver::PopCompleteFrame(uint16_t start_seq, uint16_t end_seq,
                                        uint32_t packet_count,
                                        uint32_t timestamp) {
  size_t complete_frame_size = 0;

  for (uint32_t offset = 0; offset < packet_count; ++offset) {
    const uint16_t seq = SequenceNumberAt(start_seq, offset);
    auto packet_it = incomplete_h264_frame_list_.find(seq);
    if (packet_it != incomplete_h264_frame_list_.end()) {
      complete_frame_size += packet_it->second.PayloadSize();
    }
  }

  const size_t required_capacity = complete_frame_size + 1;
  EnsureFrameBufferCapacity(required_capacity);

  uint8_t* dest = nv12_data_;
  auto first_packet_it = incomplete_h264_frame_list_.find(start_seq);
  if (first_packet_it != incomplete_h264_frame_list_.end()) {
    auto& first_pkt = first_packet_it->second;
    uint8_t header = (first_pkt.ForbiddenBit() << 7) |
                     (first_pkt.NalRefIdc() << 5) |
                     (uint8_t)first_pkt.FuNalUnitType();
    *dest++ = header;
  }
  for (uint32_t offset = 0; offset < packet_count; ++offset) {
    const uint16_t seq = SequenceNumberAt(start_seq, offset);
    auto packet_it = incomplete_h264_frame_list_.find(seq);
    if (packet_it != incomplete_h264_frame_list_.end()) {
      size_t payload_size = packet_it->second.PayloadSize();
      memcpy(dest, packet_it->second.Payload(), payload_size);
      dest += payload_size;
      incomplete_h264_frame_list_.erase(packet_it);
    }
  }

  std::unique_ptr<ReceivedFrame> received_frame = CreateReceivedFrame(
      nv12_data_, static_cast<size_t>(dest - nv12_data_), timestamp);
  const bool is_keyframe =
      IsH264KeyFrame(received_frame->Buffer(), received_frame->Size());

  ClearFrameMarkers(timestamp);
  CommitPendingFrame(timestamp, std::move(received_frame), is_keyframe,
                     end_seq, true);
  return true;
}

bool RtpVideoReceiver::CheckIsAv1FrameCompleted(RtpPacketAv1& rtp_packet_av1) {
  uint32_t timestamp = rtp_packet_av1.Timestamp();
  uint16_t seq = rtp_packet_av1.SequenceNumber();
  uint16_t start_seq, end_seq;
  uint32_t packet_count;

  TrackPendingFramePacket(timestamp, seq, rtp_packet_av1.IsKeyFrame());

  if (rtp_packet_av1.Av1FrameStart()) {
    fua_start_sequence_numbers_[timestamp] = seq;
  }
  if (rtp_packet_av1.Av1FrameEnd()) {
    fua_end_sequence_numbers_[timestamp] = seq;
  }

  if (!GetFrameSequenceRange(timestamp, "AV1", &start_seq, &end_seq,
                             &packet_count)) {
    return false;
  }

  for (uint32_t offset = 0; offset < packet_count; ++offset) {
    const uint16_t sequence_number = SequenceNumberAt(start_seq, offset);
    auto packet_it = incomplete_av1_frame_list_.find(sequence_number);
    if (packet_it == incomplete_av1_frame_list_.end() ||
        packet_it->second.Timestamp() != timestamp) {
      return false;
    }
  }

  // Pop complete AV1 frame
  const bool is_keyframe =
      incomplete_av1_frame_list_.find(start_seq)->second.IsKeyFrame();
  size_t complete_frame_size = 0;
  for (uint32_t offset = 0; offset < packet_count; ++offset) {
    const uint16_t s = SequenceNumberAt(start_seq, offset);
    complete_frame_size +=
        incomplete_av1_frame_list_.find(s)->second.PayloadSize();
  }

  EnsureFrameBufferCapacity(complete_frame_size);

  uint8_t* dest = nv12_data_;
  for (uint32_t offset = 0; offset < packet_count; ++offset) {
    const uint16_t s = SequenceNumberAt(start_seq, offset);
    auto packet_it = incomplete_av1_frame_list_.find(s);
    const size_t payload_size = packet_it->second.PayloadSize();
    memcpy(dest, packet_it->second.Payload(), payload_size);
    dest += payload_size;
    incomplete_av1_frame_list_.erase(packet_it);
  }

  std::unique_ptr<ReceivedFrame> received_frame =
      CreateReceivedFrame(nv12_data_, complete_frame_size, timestamp);

  ClearFrameMarkers(timestamp);
  CommitPendingFrame(timestamp, std::move(received_frame), is_keyframe,
                     end_seq, true);
  return true;
}

void RtpVideoReceiver::SetSendDataFunc(
    std::function<int(const char*, size_t)> data_send_func) {
  data_send_func_ = data_send_func;
}

int RtpVideoReceiver::SendRtcpRR(ReceiverReport& rtcp_rr) {
  if (!data_send_func_) {
    LOG_ERROR("data_send_func_ is nullptr");
    return -1;
  }

  if (data_send_func_((const char*)rtcp_rr.Buffer(), rtcp_rr.Size())) {
    LOG_ERROR("Send RR failed");
    return -1;
  }

  return 0;
}

TimeDelta AtoToTimeDelta(uint16_t receive_info) {
  // receive_info
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  // |R|ECN|  Arrival time offset    |
  // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
  const uint16_t ato = receive_info & 0x1FFF;
  if (ato == 0x1FFE) {
    return TimeDelta::PlusInfinity();
  }
  if (ato == 0x1FFF) {
    return TimeDelta::MinusInfinity();
  }
  return TimeDelta::Seconds(ato) / 1024;
}

void RtpVideoReceiver::SendCombinedRtcpPacket(
    std::vector<std::unique_ptr<RtcpPacket>> rtcp_packets) {
  if (!data_send_func_) {
    LOG_ERROR("data_send_func_ is nullptr");
  }

  // LOG_ERROR("Send combined rtcp packet");

  std::lock_guard<std::mutex> lock(rtcp_sender_mtx_);
  for (auto& rtcp_packet : rtcp_packets) {
    rtcp_packet->SetSenderSsrc(ssrc_);
    rtcp_sender_->AppendPacket(*rtcp_packet);
    if (!rtcp_sender_->Send()) {
      LOG_WARN("Failed sending combined RTCP packet");
    }
  }
}

void RtpVideoReceiver::SendRemb(int64_t bitrate_bps,
                                std::vector<uint32_t> ssrcs) {
  if (!active_remb_module_) {
    return;
  }

  // The Add* and Remove* methods above ensure that REMB is disabled on all
  // other modules, because otherwise, they will send REMB with stale info.
  active_remb_module_->SetRemb(bitrate_bps, std::move(ssrcs));
}

void RtpVideoReceiver::ProcessPendingNacks() {
  if (!RtxEnabled() || !nack_) {
    return;
  }

  std::vector<uint16_t> nack_batch;
  {
    std::lock_guard<std::mutex> lock(nack_mtx_);
    nack_batch = nack_->ProcessNacks();
  }
  // Never execute the transport callback while holding NACK state. This keeps
  // packet ingestion and shutdown independent from a slow RTCP send.
  SendPreparedNackBatch(std::move(nack_batch));
}

RtpVideoReceiver::FrameRecoveryTiming
RtpVideoReceiver::GetFrameRecoveryTiming() {
  if (!RtxEnabled()) {
    // Without an RTX stream there is no repair path. Escalate promptly and
    // avoid holding complete newer frames behind an unrecoverable gap.
    return {80, 0, 200};
  }
  std::lock_guard<std::mutex> lock(nack_mtx_);
  if (!nack_ || !nack_->HasRttSample()) {
    // Remote desktop favors a fresh synchronization point over preserving an
    // old damaged frame. The previous two-second recovery window released
    // newer frames in large bursts, inflating decoded FPS while the viewer saw
    // long freezes. Leave enough time for an initial RTX exchange, but request
    // a key frame promptly when no RTT sample is available yet.
    return {200, 100, 600};
  }
  const int64_t rtt_ms = std::max<int64_t>(nack_->RttMs(), 1);
  recovery_rtt_ms_.store(rtt_ms, std::memory_order_relaxed);
  const int64_t soft_deadline_ms =
      video_recovery::SoftFrameRecoveryDeadlineMs(rtt_ms);
  const int64_t soft_fir_confirmation_delay_ms =
      video_recovery::SoftFirConfirmationDelayMs(rtt_ms);
  const int64_t hard_deadline_ms =
      video_recovery::HardFrameRecoveryDeadlineMs(rtt_ms);
  return {soft_deadline_ms, soft_fir_confirmation_delay_ms,
          hard_deadline_ms};
}

void RtpVideoReceiver::OnRttUpdate(int64_t rtt_ms) {
  if (!RtxEnabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(nack_mtx_);
  if (nack_) {
    nack_->UpdateRtt(rtt_ms);
    if (nack_->HasRttSample()) {
      recovery_rtt_ms_.store(std::max<int64_t>(nack_->RttMs(), 1),
                             std::memory_order_relaxed);
    }
  }
}

void RtpVideoReceiver::MaybeLogRecoveryStats(bool force) {
  constexpr int64_t kRecoveryStatsIntervalMs = 5000;
  const int64_t now_ms = clock_->CurrentTime().ms();
  const int64_t interval_ms = now_ms - last_recovery_stats_log_ms_;
  if (!force && interval_ms < kRecoveryStatsIntervalMs) {
    return;
  }
  last_recovery_stats_log_ms_ = now_ms;
  {
    std::lock_guard<std::mutex> lock(fec_mtx_);
    if (fec_receiver_) {
      const auto& fec = fec_receiver_->Stats();
      LOG_DEBUG(
          "FEC receive: channel={} source={} repair={} recovered={} expired={} "
          "rejected={} codec_errors={}",
          log_context_, fec.source_packets, fec.repair_packets,
          fec.recovered_packets, fec.expired_blocks, fec.rejected_packets,
          fec.codec_errors);
    }
  }

  const auto take = [](std::atomic<uint64_t>& counter) {
    return counter.exchange(0, std::memory_order_relaxed);
  };
  const uint64_t completed_frames = take(recovery_completed_frames_);
  const uint64_t discarded_frames = take(recovery_discarded_frames_);
  const uint64_t soft_stalls = take(recovery_soft_stalls_);
  const uint64_t hard_timeouts = take(recovery_hard_timeouts_);
  const uint64_t admission_stalls = take(recovery_admission_stalls_);
  const uint64_t obsolete_packets = take(recovery_obsolete_packets_);
  const uint64_t accepted_rtx_packets =
      take(recovery_accepted_rtx_packets_);
  const uint64_t late_rtx_packets = take(recovery_late_rtx_packets_);
  const uint64_t fir_sent = take(recovery_fir_sent_);
  const uint64_t fir_suppressed = take(recovery_fir_suppressed_);
  const uint64_t fir_failed = take(recovery_fir_failed_);
  const uint64_t soft_fir_sent = take(recovery_soft_fir_sent_);
  const uint64_t soft_fir_avoided = take(recovery_soft_fir_avoided_);
  const uint64_t soft_fir_repaired = take(recovery_soft_fir_repaired_);
  const uint64_t soft_fir_repair_total_ms =
      take(recovery_soft_fir_repair_total_ms_);
  const uint64_t soft_fir_repair_max_ms =
      take(recovery_soft_fir_repair_max_ms_);
  const int64_t soft_fir_repair_avg_ms = soft_fir_repaired != 0
      ? static_cast<int64_t>(soft_fir_repair_total_ms / soft_fir_repaired)
      : -1;
  const uint64_t fir_responses = take(recovery_fir_responses_);
  const uint64_t fir_response_total_ms =
      take(recovery_fir_response_total_ms_);
  const uint64_t fir_response_max_ms =
      take(recovery_fir_response_max_ms_);
  const int64_t fir_response_avg_ms =
      fir_responses != 0
          ? static_cast<int64_t>(fir_response_total_ms / fir_responses)
          : -1;

  const bool has_recovery_activity =
      discarded_frames != 0 || soft_stalls != 0 || hard_timeouts != 0 ||
      obsolete_packets != 0 || accepted_rtx_packets != 0 || fir_sent != 0 ||
      fir_suppressed != 0 || fir_failed != 0 || soft_fir_sent != 0 ||
      soft_fir_avoided != 0 || soft_fir_repaired != 0 || fir_responses != 0 ||
      admission_stalls != 0;
  if (!has_recovery_activity) {
    return;
  }

  const FrameRecoveryTiming recovery_timing = GetFrameRecoveryTiming();
  int64_t rtt_ms = -1;
  {
    std::lock_guard<std::mutex> lock(nack_mtx_);
    if (nack_ && nack_->HasRttSample()) {
      rtt_ms = nack_->RttMs();
    }
  }

  if (hard_timeouts != 0) {
    LOG_WARN(
        "Video recovery degraded: channel={} media_ssrc={} interval_ms={} "
        "rtt_ms={} soft_deadline_ms={} soft_fir_delay_ms={} "
        "hard_deadline_ms={} completed_frames={} "
        "discarded_frames={} soft_stalls={} hard_timeouts={} "
        "obsolete_packets={} accepted_rtx_packets={} late_rtx_packets={} "
        "fir_pending={} fir_sent={} fir_suppressed={} fir_failed={} "
        "soft_fir_sent={} soft_fir_avoided={} soft_fir_repaired={} "
        "soft_fir_repair_avg_ms={} soft_fir_repair_max_ms={} "
        "fir_response_count={} fir_response_avg_ms={} fir_response_max_ms={} "
        "admission_stalls={}",
        log_context_, remote_ssrc_.load(), interval_ms, rtt_ms,
        recovery_timing.soft_deadline_ms,
        recovery_timing.soft_fir_confirmation_delay_ms,
        recovery_timing.hard_deadline_ms, completed_frames,
        discarded_frames, soft_stalls, hard_timeouts, obsolete_packets,
        accepted_rtx_packets, late_rtx_packets,
        keyframe_request_pending_.load(std::memory_order_relaxed), fir_sent,
        fir_suppressed, fir_failed, soft_fir_sent, soft_fir_avoided,
        soft_fir_repaired, soft_fir_repair_avg_ms, soft_fir_repair_max_ms,
        fir_responses, fir_response_avg_ms, fir_response_max_ms,
        admission_stalls);
  } else {
    LOG_DEBUG(
        "Video recovery activity: channel={} media_ssrc={} interval_ms={} "
        "rtt_ms={} soft_deadline_ms={} soft_fir_delay_ms={} "
        "hard_deadline_ms={} completed_frames={} "
        "discarded_frames={} soft_stalls={} hard_timeouts={} "
        "obsolete_packets={} accepted_rtx_packets={} late_rtx_packets={} "
        "fir_pending={} fir_sent={} fir_suppressed={} fir_failed={} "
        "soft_fir_sent={} soft_fir_avoided={} soft_fir_repaired={} "
        "soft_fir_repair_avg_ms={} soft_fir_repair_max_ms={} "
        "fir_response_count={} fir_response_avg_ms={} fir_response_max_ms={} "
        "admission_stalls={}",
        log_context_, remote_ssrc_.load(), interval_ms, rtt_ms,
        recovery_timing.soft_deadline_ms,
        recovery_timing.soft_fir_confirmation_delay_ms,
        recovery_timing.hard_deadline_ms, completed_frames,
        discarded_frames, soft_stalls, hard_timeouts, obsolete_packets,
        accepted_rtx_packets, late_rtx_packets,
        keyframe_request_pending_.load(std::memory_order_relaxed), fir_sent,
        fir_suppressed, fir_failed, soft_fir_sent, soft_fir_avoided,
        soft_fir_repaired, soft_fir_repair_avg_ms, soft_fir_repair_max_ms,
        fir_responses, fir_response_avg_ms, fir_response_max_ms,
        admission_stalls);
  }
}

bool RtpVideoReceiver::IsReceivingKeyFrame() {
  const int64_t now_ms = clock_->CurrentTime().ms();
  const auto timing = GetFrameRecoveryTiming();
  std::lock_guard<std::mutex> lock(pending_frames_mtx_);
  for (const auto& [_, frame] : pending_frames_) {
    if (frame.is_keyframe &&
        (frame.is_complete ||
         (now_ms - frame.arrival_time <= kMaxFrameAssemblyMs &&
          now_ms - frame.last_progress_time <= timing.soft_deadline_ms)))
      return true;
  }
  return false;
}

void RtpVideoReceiver::MaybeRetryKeyFrameRequest() {
  const int64_t now_ms = clock_->CurrentTime().ms();
  bool awaiting_keyframe = false;
  {
    std::lock_guard<std::mutex> lock(pending_frames_mtx_);
    awaiting_keyframe = awaiting_keyframe_;
  }
  if (!awaiting_keyframe) {
    return;
  }

  if (now_ms >=
      next_keyframe_request_ms_.load(std::memory_order_relaxed)) {
    SendKeyFrameRequest(false);
  }
}

void RtpVideoReceiver::OnCompleteRecoveryFrame(uint32_t timestamp,
                                              bool is_keyframe,
                                              int64_t now_ms,
                                              uint64_t request_generation) {
  if (!keyframe_request_pending_.load(std::memory_order_acquire) &&
      !keyframe_request_in_flight_.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(rtcp_sender_mtx_);
  std::lock_guard<std::mutex> pending_lock(pending_frames_mtx_);
  if (is_keyframe && !awaiting_keyframe_ &&
      request_generation ==
          keyframe_request_generation_.load(std::memory_order_relaxed) &&
      now_ms >= last_keyframe_request_ms_ &&
      (!keyframe_request_timestamp_floor_ ||
       !webrtc::AheadOf(*keyframe_request_timestamp_floor_, timestamp))) {
    keyframe_request_in_flight_.store(false, std::memory_order_release);
  }
  if (!keyframe_request_pending_.load(std::memory_order_relaxed)) return;
  const int64_t request_started_ms =
      keyframe_request_started_ms_.load(std::memory_order_relaxed);
  // Process clears awaiting_keyframe_ while accepting a synchronization
  // frame. A new explicit request may have re-entered that state before this
  // completion gets the send lock, even within the same clock millisecond.
  // An older keyframe also cannot close a newer soft frame's recovery event.
  if (awaiting_keyframe_ || now_ms < request_started_ms ||
      (soft_keyframe_request_timestamp_ &&
       webrtc::AheadOf(*soft_keyframe_request_timestamp_, timestamp)) ||
      (!is_keyframe && soft_keyframe_request_timestamp_ != timestamp)) {
    return;
  }
  if (!keyframe_request_pending_.exchange(false,
                                           std::memory_order_acq_rel)) {
    return;
  }
  keyframe_request_started_ms_.store(0, std::memory_order_relaxed);
  soft_keyframe_request_timestamp_.reset();
  // Keep the FIR send cooldown. Finishing a soft repair must not cause another
  // request merely to tidy up an already healthy decoder reference chain.

  const uint64_t response_ms =
      static_cast<uint64_t>(now_ms - request_started_ms);
  auto& completed = is_keyframe ? recovery_fir_responses_
                               : recovery_soft_fir_repaired_;
  auto& total_ms = is_keyframe ? recovery_fir_response_total_ms_
                              : recovery_soft_fir_repair_total_ms_;
  auto& max_ms = is_keyframe ? recovery_fir_response_max_ms_
                            : recovery_soft_fir_repair_max_ms_;
  completed.fetch_add(1, std::memory_order_relaxed);
  total_ms.fetch_add(response_ms, std::memory_order_relaxed);
  uint64_t previous_max = max_ms.load(std::memory_order_relaxed);
  while (previous_max < response_ms &&
         !max_ms.compare_exchange_weak(
             previous_max, response_ms, std::memory_order_relaxed)) {
  }
  LOG_DEBUG("Video recovery completed: channel={} outcome={} "
            "first_fir_age_ms={} last_fir_age_ms={}",
            log_context_, is_keyframe ? "keyframe" : "repaired_without_keyframe",
            response_ms, now_ms - last_keyframe_request_ms_);
}

void RtpVideoReceiver::DropFrameAssembly(uint32_t timestamp) {
  std::lock_guard<std::mutex> lock(frame_assembly_mtx_);
  ClearFrameMarkers(timestamp);

  for (auto it = incomplete_h264_frame_list_.begin();
       it != incomplete_h264_frame_list_.end();) {
    if (it->second.Timestamp() == timestamp) {
      it = incomplete_h264_frame_list_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = incomplete_h265_packets_.begin(); it != incomplete_h265_packets_.end();) {
    if (it->second.Timestamp() == timestamp) it = incomplete_h265_packets_.erase(it);
    else ++it;
  }
  for (auto it = incomplete_av1_frame_list_.begin();
       it != incomplete_av1_frame_list_.end();) {
    if (it->second.Timestamp() == timestamp) {
      it = incomplete_av1_frame_list_.erase(it);
    } else {
      ++it;
    }
  }
}

bool RtpVideoReceiver::IsFrameTimestampObsolete(uint32_t timestamp) {
  std::lock_guard<std::mutex> lock(pending_frames_mtx_);
  const bool already_completed =
      last_complete_frame_ts_.has_value() &&
      !webrtc::AheadOf(timestamp, *last_complete_frame_ts_);
  const bool already_discarded =
      last_discarded_frame_ts_.has_value() &&
      !webrtc::AheadOf(timestamp, *last_discarded_frame_ts_);
  return already_completed || already_discarded;
}

void RtpVideoReceiver::MarkFrameDiscardedLocked(uint32_t timestamp) {
  if (!last_discarded_frame_ts_.has_value() ||
      webrtc::AheadOf(timestamp, *last_discarded_frame_ts_)) {
    last_discarded_frame_ts_ = timestamp;
  }
}

bool RtpVideoReceiver::Process() {
  {
    std::lock_guard<std::mutex> lock(fec_mtx_);
    if (fec_receiver_) fec_receiver_->Expire(clock_->CurrentTime().ms());
  }
  if (!is_running_.load()) {
    return false;
  }

  ProcessPendingNacks();
  MaybeLogRecoveryStats();
  MaybeRetryKeyFrameRequest();

  const FrameRecoveryTiming recovery_timing = GetFrameRecoveryTiming();
  while (true) {
    std::unique_ptr<ReceivedFrame> completed_frame;
    uint32_t dropped_timestamp = 0;
    bool dropped_frame = false;
    bool recovery_timed_out = false;
    std::optional<FailedKeyFrame> failed_keyframe;
    bool soft_stall_started = false;
    bool request_soft_fir = false;
    bool completed_keyframe = false;
    uint32_t recovery_timestamp = 0;
    uint64_t recovery_request_generation = 0;
    bool soft_fir_avoided = false;
    const int64_t now_ms = clock_->CurrentTime().ms();

    {
      // Always acquire NACK state before pending-frame state. Packet ingestion
      // never holds these locks in the opposite order.
      std::lock_guard<std::mutex> nack_lock(nack_mtx_);
      std::lock_guard<std::mutex> pending_lock(pending_frames_mtx_);
      if (pending_frames_.empty()) {
        break;
      }
      auto it = pending_frames_.begin();
      recovery_timestamp = it->first;
      recovery_request_generation = it->second.request_generation;
      const int64_t frame_age_ms = now_ms - it->second.arrival_time;
      const auto missing_since =
          nack_ ? nack_->FirstMissingTime(it->second.last_sequence_number)
                : std::nullopt;
      const bool has_blocking_nacks = missing_since.has_value();
      // Separate packet serialization from loss repair. A gap discovered late
      // in a large frame still gets an RTT-based repair window. Forward
      // progress and promotion to the head of the queue never restart it.
      const int64_t recovery_age_ms =
          missing_since
              ? now_ms - missing_since->ms()
              : (!it->second.is_complete
                     ? now_ms - it->second.last_progress_time
                     : frame_age_ms);
      const bool is_older_than_decoded_frame =
          last_complete_frame_ts_.has_value() &&
          !webrtc::AheadOf(it->first, *last_complete_frame_ts_);
      bool newer_complete_keyframe_available = false;
      if ((awaiting_keyframe_ || it->second.recovery_escalated) &&
          !(it->second.is_complete && it->second.is_keyframe)) {
        for (auto keyframe_it = std::next(it);
             keyframe_it != pending_frames_.end(); ++keyframe_it) {
          if (keyframe_it->second.is_complete &&
              keyframe_it->second.is_keyframe) {
            newer_complete_keyframe_available = true;
            break;
          }
        }
      }

      if (is_older_than_decoded_frame) {
        dropped_timestamp = it->first;
        dropped_frame = true;
        pending_frames_.erase(it);
      } else if (newer_complete_keyframe_available) {
        // A complete synchronization point is already available. Do not let
        // an older incomplete delta frame hold it behind another timeout.
        awaiting_keyframe_ = true;
        dropped_timestamp = it->first;
        dropped_frame = true;
        pending_frames_.erase(it);
      } else if (it->second.is_complete && awaiting_keyframe_ &&
                 !it->second.is_keyframe) {
        // An incomplete frame may still be missing its IDR header. Preserve
        // that repair opportunity; only discard known complete deltas here.
        dropped_timestamp = it->first;
        dropped_frame = true;
        if (nack_ && it->second.last_sequence_number)
          nack_->ClearUpTo(*it->second.last_sequence_number);
        MarkFrameDiscardedLocked(dropped_timestamp);
        pending_frames_.erase(it);
      } else if (it->second.is_complete &&
                 (!has_blocking_nacks || (awaiting_keyframe_ && it->second.is_keyframe)) &&
                 (it->second.is_keyframe || frame_age_ms <= kMaxFrameAssemblyMs)) {
        if (frame_admission_ && !frame_admission_(it->second.is_keyframe)) {
          recovery_admission_stalls_.fetch_add(1, std::memory_order_relaxed);
          if (!it->second.is_keyframe) break;
          // A complete IDR can recover a stalled decoder when its output
          // timeout expires. Start its wait at the first admission refusal.
          auto& since = it->second.admission_blocked_since_ms;
          if (!since) since = now_ms;
          if (now_ms - *since < video_recovery::kDecodeOutputTimeoutMs) break;
          recovery_timed_out = true;
        } else {
          if (awaiting_keyframe_ && nack_ && it->second.last_sequence_number) {
            nack_->ClearUpTo(*it->second.last_sequence_number);
          }
          awaiting_keyframe_ = false;
          soft_fir_avoided = it->second.recovery_escalated &&
                             !it->second.soft_fir_requested;
          last_complete_frame_ts_ = it->first;
          completed_keyframe = it->second.is_keyframe;
          completed_frame = std::move(it->second.frame);
          pending_frames_.erase(it);
        }
      } else if (frame_age_ms > kMaxFrameAssemblyMs ||
                 recovery_age_ms > recovery_timing.hard_deadline_ms) {
        recovery_timed_out = true;
      } else if (!it->second.recovery_escalated &&
                 recovery_age_ms > recovery_timing.soft_deadline_ms) {
        // Treat the soft deadline as a warning first. A short confirmation
        // window gives the aggressive NACK schedule one more chance to finish
        // without paying for an unnecessary full key frame.
        it->second.recovery_escalated = true;
        soft_stall_started = true;
      } else if (it->second.recovery_escalated &&
                 !it->second.soft_fir_requested &&
                 recovery_age_ms >
                     recovery_timing.soft_deadline_ms +
                         recovery_timing.soft_fir_confirmation_delay_ms) {
        it->second.soft_fir_requested = true;
        request_soft_fir = true;
        LOG_DEBUG("Video frame recovery FIR: channel={} timestamp={} "
                  "keyframe={} frame_age_ms={} recovery_age_ms={} missing={}",
                  log_context_, it->first, it->second.is_keyframe, frame_age_ms,
                  recovery_age_ms, has_blocking_nacks);
      } else {
        break;
      }

      if (recovery_timed_out) {
        if (it->second.is_keyframe) {
          failed_keyframe = FailedKeyFrame{it->second.arrival_time,
                                          it->second.request_generation,
                                          it->first};
        }
        dropped_timestamp = it->first;
        dropped_frame = true;
        LOG_DEBUG("Video frame recovery timeout: channel={} timestamp={} "
                  "keyframe={} frame_age_ms={} recovery_age_ms={} missing={}",
                  log_context_, it->first, it->second.is_keyframe, frame_age_ms,
                  recovery_age_ms, has_blocking_nacks);
        awaiting_keyframe_ = true;
        if (nack_ && it->second.last_sequence_number.has_value()) {
          // Only abandon losses that can affect this frame. Newer frames,
          // especially an in-flight keyframe, retain their retransmission
          // state.
          nack_->ClearUpTo(*it->second.last_sequence_number);
        }
        pending_frames_.erase(it);
        // A hard timeout is an explicit decision to abandon this timestamp.
        // Remember it before releasing the lock so late fragments cannot
        // recreate the same pending frame and time out a second time.
        MarkFrameDiscardedLocked(dropped_timestamp);
      }
    }

    if (dropped_frame) {
      recovery_discarded_frames_.fetch_add(1, std::memory_order_relaxed);
      DropFrameAssembly(dropped_timestamp);
    }

    if (soft_fir_avoided) {
      recovery_soft_fir_avoided_.fetch_add(1,
                                            std::memory_order_relaxed);
    }

    if (recovery_timed_out) {
      recovery_hard_timeouts_.fetch_add(1, std::memory_order_relaxed);
      if (failed_keyframe ||
          now_ms >=
              next_keyframe_request_ms_.load(std::memory_order_relaxed)) {
        SendKeyFrameRequest(false, failed_keyframe);
      }
      return false;
    }

    if (soft_stall_started) {
      recovery_soft_stalls_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    if (request_soft_fir) {
      if (now_ms >=
          next_keyframe_request_ms_.load(std::memory_order_relaxed)) {
        if (SendKeyFrameRequest(false, std::nullopt, recovery_timestamp)) {
          recovery_soft_fir_sent_.fetch_add(1,
                                             std::memory_order_relaxed);
        }
      }
      return false;
    }

    if (dropped_frame) {
      continue;
    }

    if (completed_frame) {
      OnCompleteRecoveryFrame(recovery_timestamp, completed_keyframe, now_ms,
                              recovery_request_generation);
      recovery_completed_frames_.fetch_add(1, std::memory_order_relaxed);
      if (on_receive_complete_frame_) {
        on_receive_complete_frame_(std::move(completed_frame));
      }
    }
  }

  return true;
}

void RtpVideoReceiver::ReviseFrequencyAndJitter(int payload_type_frequency) {
  if (payload_type_frequency == last_payload_type_frequency_) {
    return;
  }

  if (payload_type_frequency != 0) {
    if (last_payload_type_frequency_ != 0) {
      // Value in "jitter_q4_" variable is a number of samples.
      // I.e. jitter = timestamp (s) * frequency (Hz).
      // Since the frequency has changed we have to update the number of
      // samples accordingly. The new value should rely on a new frequency.

      // If we don't do such procedure we end up with the number of samples
      // that cannot be converted into TimeDelta correctly (i.e. jitter =
      // jitter_q4_ >> 4 / payload_type_frequency). In such case, the number
      // of samples has a "mix".

      // Doing so we pretend that everything prior and including the current
      // packet were computed on packet's frequency.
      jitter_q4_ = static_cast<int>(static_cast<uint64_t>(jitter_q4_) *
                                    payload_type_frequency /
                                    last_payload_type_frequency_);
    }
    // If last_payload_type_frequency_ is not present, the jitter_q4_
    // variable has its initial value.

    // Keep last_payload_type_frequency_ up to date and non-zero (set).
    last_payload_type_frequency_ = payload_type_frequency;
  }
}

void RtpVideoReceiver::SendRR() {
  const uint32_t now =
      SystemClock::CompactNtp(system_clock_->CurrentNtpTime());
  ReceiverReport rtcp_rr;
  RtcpReportBlock report;

  {
    std::lock_guard<std::mutex> stats_lock(receiver_stats_mtx_);

    // Calculate fraction lost.
    int64_t exp_since_last =
        extended_high_seq_num_ - last_extended_high_seq_num_;
    int32_t lost_since_last = cumulative_loss_ - last_report_cumulative_loss_;
    if (exp_since_last > 0 && lost_since_last > 0) {
      // Scale 0 to 255, where 255 is 100% loss.
      fraction_lost_ = 255 * lost_since_last / exp_since_last;
    } else {
      fraction_lost_ = 0;
    }

    cumulative_lost_ = cumulative_loss_ + cumulative_loss_rtcp_offset_;
    if (cumulative_lost_ < 0) {
      // Clamp to zero. Work around to accommodate for senders that misbehave
      // with negative cumulative loss.
      cumulative_lost_ = 0;
      cumulative_loss_rtcp_offset_ = -cumulative_loss_;
    }
    if (cumulative_lost_ > 0x7fffff) {
      // Packets lost is a 24 bit signed field, and thus should be clamped, as
      // described in RFC 3550 appendix A.3.
      cumulative_lost_ = 0x7fffff;
    }

    const uint32_t delay_since_last_sr =
        last_remote_ntp_timestamp != 0
            ? now - last_arrival_ntp_timestamp
            : 0;

    report.SetMediaSsrc(remote_ssrc_.load());
    report.SetFractionLost(fraction_lost_);
    report.SetCumulativeLost(cumulative_lost_);
    report.SetExtHighestSeqNum(extended_high_seq_num_);
    report.SetJitter(jitter_);
    report.SetLastSr(last_remote_ntp_timestamp);
    report.SetDelayLastSr(delay_since_last_sr);
    rtcp_rr.SetSenderSsrc(ssrc_);
    rtcp_rr.SetReportBlock(report);

    last_extended_high_seq_num_ = extended_high_seq_num_;
    last_report_cumulative_loss_ = cumulative_loss_;
  }

  rtcp_rr.Build();
  SendRtcpRR(rtcp_rr);
}

void RtpVideoReceiver::StopRtcp() {
  is_running_.store(false);
  if (rtcp_stop_.load()) {
    return;
  }

  rtcp_stop_.store(true);
  rtcp_cv_.notify_all();
  if (rtcp_thread_.joinable()) {
    rtcp_thread_.join();
  }
}

void RtpVideoReceiver::RtcpThread() {
  auto feedback_delay = std::chrono::milliseconds(25);
  while (!rtcp_stop_.load()) {
    std::unique_lock<std::mutex> lock(rtcp_mtx_);
    if (rtcp_cv_.wait_for(lock, feedback_delay,
                          [&]() { return rtcp_stop_.load(); })) {
      break;
    }
    lock.unlock();

    // Flush the last packets of a short probe even if no more RTP arrives.
    // Keep receiver reports on their existing cadence while honoring the
    // congestion feedback generator's earlier deadline.
    const auto next_feedback =
        receive_side_congestion_controller_.MaybeProcess();
    feedback_delay = std::chrono::milliseconds(std::clamp<int64_t>(
        next_feedback.ms(), 1, rtcp_scheduler_interval_ms_));

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_send_rtcp_rr_ts_)
                       .count();
    if (elapsed >= rtcp_rr_interval_ms_ &&
        has_received_media_packet_.load()) {
      SendRR();
      last_send_rtcp_rr_ts_ = now;
    }
  }
}

/******************************************************************************/

bool RtpVideoReceiver::SendNack(const std::vector<uint16_t>& nack_list) {
  if (!RtxEnabled() || nack_list.empty() || rtcp_stop_.load()) {
    return false;
  }

  webrtc::rtcp::Nack nack;
  nack.SetSenderSsrc(ssrc_);
  nack.SetMediaSsrc(remote_ssrc_.load());
  nack.SetPacketIds(nack_list);

  std::lock_guard<std::mutex> lock(rtcp_sender_mtx_);
  rtcp_sender_->AppendPacket(nack);
  return rtcp_sender_->Send();
}

void RtpVideoReceiver::SendPreparedNackBatch(
    std::vector<uint16_t> nack_batch) {
  if (nack_batch.empty()) {
    return;
  }

  for (size_t offset = 0; offset < nack_batch.size();
       offset += kMaxNackIdsPerRtcpPacket) {
    const size_t end =
        std::min(nack_batch.size(), offset + kMaxNackIdsPerRtcpPacket);
    std::vector<uint16_t> datagram_batch(nack_batch.begin() + offset,
                                         nack_batch.begin() + end);
    const bool send_successful = SendNack(datagram_batch);
    {
      std::lock_guard<std::mutex> lock(nack_mtx_);
      if (nack_) {
        nack_->OnNackBatchSent(datagram_batch, send_successful);
      }
    }
    if (!send_successful && !rtcp_stop_.load()) {
      LOG_WARN("Failed sending NACK datagram containing {} packets",
               datagram_batch.size());
    }
  }
}

bool RtpVideoReceiver::SendKeyFrameRequest(
    bool enter_awaiting_state,
    std::optional<FailedKeyFrame> failed_keyframe,
    std::optional<uint32_t> soft_frame_timestamp) {
  if (enter_awaiting_state) {
    std::lock_guard<std::mutex> lock(pending_frames_mtx_);
    awaiting_keyframe_ = true;
  }

  std::lock_guard<std::mutex> lock(rtcp_sender_mtx_);
  // Apply the same guard to soft/hard timeouts and periodic retries. A
  // superseded partial delta must not replace a keyframe still arriving.
  if (IsReceivingKeyFrame()) {
    recovery_fir_suppressed_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const int64_t now_ms = clock_->CurrentTime().ms();
  // A fixed 1.5 s retry meant a viewer could wait a full second and a half for
  // a second synchronization point when the first FIR or key frame was lost.
  // Scale the retry with the path instead. A key frame that is actually making
  // progress still suppresses the retry below (IsReceivingKeyFrame), so slow
  // TURN paths keep their single large frame without extra requests.
  const int64_t measured_rtt_ms =
      recovery_rtt_ms_.load(std::memory_order_relaxed);
  const int64_t fir_retry_interval_ms =
      measured_rtt_ms > 0 ? std::max<int64_t>(300, 3 * measured_rtt_ms) : 600;
  constexpr int64_t kFailedFirRetryIntervalMs = 500;
  // A keyframe received after the latest FIR has now been explicitly
  // abandoned. Retry it without waiting for the normal in-flight allowance.
  // Recheck under the send lock: an older failure must not bypass a newer
  // request's cooldown or a completed request. IsReceivingKeyFrame above
  // still protects any newer keyframe making forward progress.
  const bool retry_failed_keyframe =
      failed_keyframe &&
      keyframe_request_in_flight_.load(std::memory_order_relaxed) &&
      failed_keyframe->request_generation ==
          keyframe_request_generation_.load(std::memory_order_relaxed) &&
      failed_keyframe->arrival_ms >= last_keyframe_request_ms_ &&
      (!keyframe_request_timestamp_floor_ ||
       !webrtc::AheadOf(*keyframe_request_timestamp_floor_,
                       failed_keyframe->timestamp));
  if (now_ms <
          next_keyframe_request_ms_.load(std::memory_order_relaxed) &&
      !retry_failed_keyframe) {
    recovery_fir_suppressed_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const bool request_already_pending =
      keyframe_request_pending_.load(std::memory_order_acquire);

  ++sequence_number_fir_;
  webrtc::rtcp::Fir fir;
  fir.SetSenderSsrc(ssrc_);
  fir.AddRequestTo(remote_ssrc_.load(), sequence_number_fir_);

  {
    std::lock_guard<std::mutex> pending_lock(pending_frames_mtx_);
    keyframe_request_timestamp_floor_ = last_complete_frame_ts_;
    for (const auto timestamp : {last_discarded_frame_ts_,
                                 soft_frame_timestamp}) {
      if (timestamp && (!keyframe_request_timestamp_floor_ ||
                        webrtc::AheadOf(*timestamp,
                                       *keyframe_request_timestamp_floor_))) {
        keyframe_request_timestamp_floor_ = timestamp;
      }
    }
    keyframe_request_generation_.fetch_add(1, std::memory_order_relaxed);
  }
  // Consume the old attempt even if the transport rejects this retry. A
  // duplicate failure must respect the normal send-failure backoff below.
  keyframe_request_in_flight_.store(false, std::memory_order_release);
  rtcp_sender_->AppendPacket(fir);
  if (!rtcp_sender_->Send()) {
    next_keyframe_request_ms_.store(now_ms + kFailedFirRetryIntervalMs,
                                    std::memory_order_relaxed);
    recovery_fir_failed_.fetch_add(1, std::memory_order_relaxed);
    LOG_WARN("Failed sending FIR for media SSRC {}", remote_ssrc_.load());
    return false;
  }

  if (!request_already_pending) {
    keyframe_request_started_ms_.store(now_ms, std::memory_order_relaxed);
    soft_keyframe_request_timestamp_ = soft_frame_timestamp;
    keyframe_request_pending_.store(true, std::memory_order_release);
  }
  next_keyframe_request_ms_.store(now_ms + fir_retry_interval_ms,
                                  std::memory_order_relaxed);
  last_keyframe_request_ms_ = now_ms;
  keyframe_request_in_flight_.store(true, std::memory_order_release);
  if (retry_failed_keyframe) {
    LOG_DEBUG("Retry FIR after failed keyframe: channel={} frame_age_ms={}",
              log_context_, now_ms - failed_keyframe->arrival_ms);
  }
  recovery_fir_sent_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void RtpVideoReceiver::RequestKeyFrame() { SendKeyFrameRequest(true); }

void RtpVideoReceiver::OnSenderReport(const SenderReport& sender_report) {
  if (!system_clock_ || sender_report.SenderSsrc() != remote_ssrc_.load() ||
      sender_report.NtpTimestamp() == 0) {
    return;
  }

  const uint64_t ntp_timestamp = sender_report.NtpTimestamp();
  rtp_timestamp_mapper_.UpdateSenderReport(
      sender_report.Timestamp(), system_clock_->NtpToUtcTimeUs(ntp_timestamp),
      system_clock_->CurrentTimeUs());

  std::lock_guard<std::mutex> stats_lock(receiver_stats_mtx_);
  remote_ssrc = sender_report.SenderSsrc();
  last_remote_ntp_timestamp = sender_report.CompactNtpTimestamp();
  last_remote_rtp_timestamp = sender_report.Timestamp();
  last_arrival_timestamp = clock_->CurrentTime().ms();
  last_arrival_ntp_timestamp =
      SystemClock::CompactNtp(system_clock_->CurrentNtpTime());
  packets_sent = sender_report.SenderPacketCount();
  bytes_sent = sender_report.SenderOctetCount();
  reports_count++;
}
}  // namespace minirtc
