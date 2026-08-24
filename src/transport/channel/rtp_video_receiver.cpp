#include "rtp_video_receiver.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include "api/ntp/ntp_time_util.h"
#include "common.h"
#include "fir.h"
#include "log.h"
#include "nack.h"
#include "rtcp_sender.h"

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
      nack_(std::make_unique<NackRequester>(clock_)),
      delta_ntp_internal_ms_(clock->CurrentNtpInMilliseconds() -
                             clock->CurrentTimeMs()) {
  SetPeriod(std::chrono::milliseconds(5));
  SetThreadName("RtpVideoReceiver");
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

  SSRCManager::Instance().DeleteSsrc(ssrc_);

  delete[] nv12_data_;

  incomplete_h264_frame_list_.clear();
  incomplete_av1_frame_list_.clear();
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
  if (!media_packet || rtx_packet.PayloadType() != rtp::PAYLOAD_TYPE::RTX ||
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
                                               uint16_t sequence_number) {
  std::lock_guard<std::mutex> lock(pending_frames_mtx_);
  PendingFrame& pending_frame = pending_frames_[timestamp];
  if (pending_frame.arrival_time == 0) {
    pending_frame.arrival_time = clock_->CurrentTime().ms();
  }
  if (!pending_frame.last_sequence_number.has_value() ||
      webrtc::AheadOf(sequence_number,
                      *pending_frame.last_sequence_number)) {
    pending_frame.last_sequence_number = sequence_number;
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
  std::lock_guard<std::mutex> lock(pending_frames_mtx_);
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
  pending_frame = {std::move(frame), true, first_arrival_time, is_keyframe,
                   end_sequence_number};
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
  frame->SetReceivedTimestamp(clock_->CurrentTime().us());
  frame->SetCapturedTimestamp(
      (static_cast<int64_t>(timestamp) / rtp::kMsToRtpTimestamp -
       delta_ntp_internal_ms_) *
      1000);
  return frame;
}

void RtpVideoReceiver::InsertRtpPacket(RtpPacket& rtp_packet) {
  const bool is_recovered =
      rtp_packet.PayloadType() == rtp::PAYLOAD_TYPE::RTX;
  const uint32_t negotiated_rtx_ssrc = rtx_ssrc_.load();
  if (is_recovered &&
      (negotiated_rtx_ssrc == 0 ||
       rtp_packet.Ssrc() != negotiated_rtx_ssrc)) {
    LOG_WARN("Dropping unnegotiated RTX packet from SSRC {}",
             rtp_packet.Ssrc());
    return;
  }
  RtpPacket restored_media_packet;
  RtpPacket* media_packet = &rtp_packet;
  if (is_recovered) {
    if (!RestoreMediaPacketFromRtx(rtp_packet, &restored_media_packet)) {
      LOG_WARN("Failed to restore media packet from RTX SSRC {}",
               rtp_packet.Ssrc());
      return;
    }
    media_packet = &restored_media_packet;

    bool is_late_recovery = false;
    {
      std::lock_guard<std::mutex> lock(pending_frames_mtx_);
      is_late_recovery =
          last_complete_frame_ts_.has_value() &&
          !webrtc::AheadOf(media_packet->Timestamp(),
                           *last_complete_frame_ts_);
    }
    if (is_late_recovery) {
      std::lock_guard<std::mutex> lock(nack_mtx_);
      nack_->OnReceivedPacket(media_packet->SequenceNumber(), true);
      LOG_WARN("Dropping late RTX packet OSN {} timestamp {}",
               media_packet->SequenceNumber(), media_packet->Timestamp());
      return;
    }
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

  const rtp::PAYLOAD_TYPE payload_type = media_packet->PayloadType();
  const bool is_media_payload = payload_type == media_payload_type_;
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

  if (payload_type == rtp::PAYLOAD_TYPE::AV1) {
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

void RtpVideoReceiver::ProcessH264RtpPacket(RtpPacketH264& rtp_packet_h264) {
  if (!fec_enable_) {
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
  //  else {
  //   if (rtp::PAYLOAD_TYPE::H264 == rtp_packet.PayloadType()) {
  //     if (rtp::NAL_UNIT_TYPE::NALU == rtp_packet.NalUnitType()) {
  //       compelete_video_frame_queue_.push(
  //           VideoFrame(rtp_packet.Payload(), rtp_packet.PayloadSize()));
  //     } else if (rtp::NAL_UNIT_TYPE::FU_A == rtp_packet.NalUnitType()) {
  //       incomplete_h264_frame_list_[rtp_packet.SequenceNumber()] =
  //       rtp_packet; bool complete = CheckIsH264FrameCompleted(rtp_packet);
  //       if
  //       (!complete) {
  //       }
  //     }
  //   } else if (rtp::PAYLOAD_TYPE::H264_FEC_SOURCE ==
  //   rtp_packet.PayloadType()) {
  //     if (last_packet_ts_ != rtp_packet.Timestamp()) {
  //       fec_decoder_.Init();
  //       fec_decoder_.ResetParams(rtp_packet.FecSourceSymbolNum());
  //       last_packet_ts_ = rtp_packet.Timestamp();
  //     }

  //     incomplete_fec_packet_list_[rtp_packet.Timestamp()]
  //                                [rtp_packet.SequenceNumber()] =
  //                                rtp_packet;

  //     uint8_t** complete_frame = fec_decoder_.DecodeWithNewSymbol(
  //         (const char*)incomplete_fec_packet_list_[rtp_packet.Timestamp()]
  //                                                 [rtp_packet.SequenceNumber()]
  //                                                     .Payload(),
  //         rtp_packet.FecSymbolId());

  //     if (nullptr != complete_frame) {
  //       if (!nv12_data_) {
  //         nv12_data_ = new uint8_t[NV12_BUFFER_SIZE];
  //       }

  //       size_t complete_frame_size = 0;
  //       for (int index = 0; index < rtp_packet.FecSourceSymbolNum();
  //       index++)
  //       {
  //         if (nullptr == complete_frame[index]) {
  //           LOG_ERROR("Invalid complete_frame[{}]", index);
  //         }
  //         memcpy(nv12_data_ + complete_frame_size, complete_frame[index],
  //         1400); complete_frame_size += 1400;
  //       }

  //       fec_decoder_.ReleaseSourcePackets(complete_frame);
  //       fec_decoder_.Release();
  //       LOG_ERROR("Release incomplete_fec_packet_list_");
  //       incomplete_fec_packet_list_.erase(rtp_packet.Timestamp());

  //       if (incomplete_fec_frame_list_.end() !=
  //           incomplete_fec_frame_list_.find(rtp_packet.Timestamp())) {
  //         incomplete_fec_frame_list_.erase(rtp_packet.Timestamp());
  //       }

  //       compelete_video_frame_queue_.push(
  //           VideoFrame(nv12_data_, complete_frame_size));
  //     } else {
  //       incomplete_fec_frame_list_.insert(rtp_packet.Timestamp());
  //     }
  //   } else if (rtp::PAYLOAD_TYPE::H264_FEC_REPAIR ==
  //   rtp_packet.PayloadType()) {
  //     if (incomplete_fec_frame_list_.end() ==
  //         incomplete_fec_frame_list_.find(rtp_packet.Timestamp())) {
  //       return;
  //     }

  //     if (last_packet_ts_ != rtp_packet.Timestamp()) {
  //       fec_decoder_.Init();
  //       fec_decoder_.ResetParams(rtp_packet.FecSourceSymbolNum());
  //       last_packet_ts_ = rtp_packet.Timestamp();
  //     }

  //     incomplete_fec_packet_list_[rtp_packet.Timestamp()]
  //                                [rtp_packet.SequenceNumber()] =
  //                                rtp_packet;

  //     uint8_t** complete_frame = fec_decoder_.DecodeWithNewSymbol(
  //         (const char*)incomplete_fec_packet_list_[rtp_packet.Timestamp()]
  //                                                 [rtp_packet.SequenceNumber()]
  //                                                     .Payload(),
  //         rtp_packet.FecSymbolId());

  //     if (nullptr != complete_frame) {
  //       if (!nv12_data_) {
  //         nv12_data_ = new uint8_t[NV12_BUFFER_SIZE];
  //       }

  //       size_t complete_frame_size = 0;
  //       for (int index = 0; index < rtp_packet.FecSourceSymbolNum();
  //       index++)
  //       {
  //         if (nullptr == complete_frame[index]) {
  //           LOG_ERROR("Invalid complete_frame[{}]", index);
  //         }
  //         memcpy(nv12_data_ + complete_frame_size, complete_frame[index],
  //         1400); complete_frame_size += 1400;
  //       }

  //       fec_decoder_.ReleaseSourcePackets(complete_frame);
  //       fec_decoder_.Release();
  //       incomplete_fec_packet_list_.erase(rtp_packet.Timestamp());

  //       compelete_video_frame_queue_.push(
  //           VideoFrame(nv12_data_, complete_frame_size));
  //     }
  //   }
  // }
}

void RtpVideoReceiver::ProcessAv1RtpPacket(RtpPacketAv1& rtp_packet_av1) {
  if (!fec_enable_) {
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

  TrackPendingFramePacket(timestamp, seq);

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

  TrackPendingFramePacket(timestamp, seq);

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

std::pair<int64_t, int64_t>
RtpVideoReceiver::FrameRecoveryDeadlinesMs() {
  if (!RtxEnabled()) {
    // Without an RTX stream there is no repair path. Escalate promptly and
    // avoid holding complete newer frames behind an unrecoverable gap.
    return {80, 200};
  }
  std::lock_guard<std::mutex> lock(nack_mtx_);
  if (!nack_ || !nack_->HasRttSample()) {
    // Remote desktop favors a fresh synchronization point over preserving an
    // old damaged frame. The previous two-second recovery window released
    // newer frames in large bursts, inflating decoded FPS while the viewer saw
    // long freezes. Leave enough time for an initial RTX exchange, but request
    // a key frame promptly when no RTT sample is available yet.
    return {200, 600};
  }
  const int64_t rtt_ms = std::max<int64_t>(nack_->RttMs(), 1);
  const int64_t soft_deadline_ms =
      std::clamp<int64_t>(2 * rtt_ms + 30, 100, 500);
  const int64_t hard_deadline_ms =
      std::clamp<int64_t>(4 * rtt_ms + 80, 300, 1000);
  return {soft_deadline_ms, hard_deadline_ms};
}

void RtpVideoReceiver::OnRttUpdate(int64_t rtt_ms) {
  if (!RtxEnabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(nack_mtx_);
  if (nack_) {
    nack_->UpdateRtt(rtt_ms);
  }
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
  for (auto it = incomplete_av1_frame_list_.begin();
       it != incomplete_av1_frame_list_.end();) {
    if (it->second.Timestamp() == timestamp) {
      it = incomplete_av1_frame_list_.erase(it);
    } else {
      ++it;
    }
  }
}

bool RtpVideoReceiver::Process() {
  if (!is_running_.load()) {
    return false;
  }

  ProcessPendingNacks();

  const auto [soft_deadline_ms, hard_deadline_ms] =
      FrameRecoveryDeadlinesMs();
  while (true) {
    std::unique_ptr<ReceivedFrame> completed_frame;
    uint32_t dropped_timestamp = 0;
    bool dropped_frame = false;
    bool recovery_timed_out = false;
    bool request_keyframe = false;
    bool escalate_recovery = false;
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
      const int64_t frame_age_ms = now_ms - it->second.arrival_time;
      const bool has_blocking_nacks =
          nack_ &&
          (it->second.last_sequence_number.has_value()
               ? nack_->HasPendingNacksUpTo(
                     *it->second.last_sequence_number)
               : nack_->HasPendingNacks());
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
                 it->second.is_keyframe) {
        awaiting_keyframe_ = false;
        if (nack_ && it->second.last_sequence_number.has_value()) {
          nack_->ClearUpTo(*it->second.last_sequence_number);
        }
        last_complete_frame_ts_ = it->first;
        completed_frame = std::move(it->second.frame);
        pending_frames_.erase(it);
      } else if (it->second.is_complete && awaiting_keyframe_) {
        dropped_timestamp = it->first;
        dropped_frame = true;
        request_keyframe = true;
        pending_frames_.erase(it);
      } else if (it->second.is_complete && !has_blocking_nacks) {
        last_complete_frame_ts_ = it->first;
        completed_frame = std::move(it->second.frame);
        pending_frames_.erase(it);
      } else if (frame_age_ms > hard_deadline_ms) {
        dropped_timestamp = it->first;
        dropped_frame = true;
        recovery_timed_out = true;
        awaiting_keyframe_ = true;
        if (nack_ && it->second.last_sequence_number.has_value()) {
          // Only abandon losses that can affect this frame. Newer frames,
          // especially an in-flight keyframe, retain their retransmission
          // state.
          nack_->ClearUpTo(*it->second.last_sequence_number);
        }
        pending_frames_.erase(it);
      } else if (!it->second.recovery_escalated &&
                 frame_age_ms > soft_deadline_ms) {
        // Ask the encoder for a synchronization point without abandoning a
        // potentially recoverable frame. If that keyframe arrives first, the
        // branch above bypasses this frame immediately; otherwise RTX retains
        // the full hard-deadline opportunity.
        it->second.recovery_escalated = true;
        escalate_recovery = true;
      } else {
        break;
      }
    }

    if (dropped_frame) {
      DropFrameAssembly(dropped_timestamp);
    }

    if (recovery_timed_out) {
      LOG_WARN("Frame timestamp {} exceeded RTX recovery window {} ms",
               dropped_timestamp, hard_deadline_ms);
      RequestKeyFrame();
      return false;
    }

    if (escalate_recovery) {
      LOG_WARN(
          "Frame recovery stalled at {} ms; requesting a key frame while "
          "retaining RTX until {} ms",
          soft_deadline_ms, hard_deadline_ms);
      SendKeyFrameRequest(false);
      return false;
    }

    if (request_keyframe) {
      RequestKeyFrame();
    }

    if (dropped_frame) {
      continue;
    }

    if (on_receive_complete_frame_) {
      on_receive_complete_frame_(std::move(completed_frame));
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
  uint32_t now = CompactNtp(clock_->CurrentNtpTime());
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

    uint32_t receive_time = last_arrival_ntp_timestamp;
    uint32_t delay_since_last_sr = now - receive_time;

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
  while (!rtcp_stop_.load()) {
    std::unique_lock<std::mutex> lock(rtcp_mtx_);
    if (rtcp_cv_.wait_for(
        lock, std::chrono::milliseconds(rtcp_scheduler_interval_ms_),
        [&]() { return rtcp_stop_.load(); })) {
      break;
    }
    lock.unlock();

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

void RtpVideoReceiver::SendKeyFrameRequest(bool enter_awaiting_state) {
  if (enter_awaiting_state) {
    std::lock_guard<std::mutex> lock(pending_frames_mtx_);
    awaiting_keyframe_ = true;
  }

  std::lock_guard<std::mutex> lock(rtcp_sender_mtx_);
  const int64_t now_ms = clock_->CurrentTime().ms();
  constexpr int64_t kMinFirIntervalMs = 500;
  if (last_keyframe_request_ms_ != 0 &&
      now_ms - last_keyframe_request_ms_ < kMinFirIntervalMs) {
    return;
  }
  last_keyframe_request_ms_ = now_ms;

  ++sequence_number_fir_;
  webrtc::rtcp::Fir fir;
  fir.SetSenderSsrc(ssrc_);
  fir.AddRequestTo(remote_ssrc_.load(), sequence_number_fir_);

  rtcp_sender_->AppendPacket(fir);
  if (!rtcp_sender_->Send()) {
    LOG_WARN("Failed sending FIR for media SSRC {}", remote_ssrc_.load());
  }
}

void RtpVideoReceiver::RequestKeyFrame() { SendKeyFrameRequest(true); }

inline uint32_t DivideRoundToNearest(int64_t dividend, int64_t divisor) {
  if (dividend < 0) {
    int64_t half_of_divisor = divisor / 2;
    int64_t quotient = dividend / divisor;
    int64_t remainder = dividend % divisor;
    if (-remainder > half_of_divisor) {
      --quotient;
    }
    return quotient;
  }

  int64_t half_of_divisor = (divisor - 1) / 2;
  int64_t quotient = dividend / divisor;
  int64_t remainder = dividend % divisor;
  if (remainder > half_of_divisor) {
    ++quotient;
  }
  return quotient;
}

void RtpVideoReceiver::OnSenderReport(const SenderReport& sender_report) {
  std::lock_guard<std::mutex> stats_lock(receiver_stats_mtx_);
  remote_ssrc = sender_report.SenderSsrc();
  last_remote_ntp_timestamp = sender_report.NtpTimestamp();
  last_remote_rtp_timestamp = sender_report.Timestamp();
  last_arrival_timestamp = clock_->CurrentTime().ms();
  last_arrival_ntp_timestamp = webrtc::CompactNtp(clock_->CurrentNtpTime());
  packets_sent = sender_report.SenderPacketCount();
  bytes_sent = sender_report.SenderOctetCount();
  reports_count++;
}
}  // namespace minirtc
