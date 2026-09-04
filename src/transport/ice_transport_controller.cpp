#include "ice_transport_controller.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "data_channel_send.h"
#include "native_video_frame.h"
#include "resolution_adapter.h"
#include "video_adaptation_policy.h"
#include "video_frame_wrapper.h"

#if defined(__APPLE__)
#if USE_CUDA
#pragma message("Warning: CUDA is ignored on macOS.")
#endif
#elif USE_CUDA && !defined(__aarch64__) && !defined(__arm__)
#include "nvcodec_api.h"
#endif

#include "api/transport/network_types.h"

namespace minirtc {
namespace {

constexpr int64_t kDesktopPacerQueueLimitMs = 100;
constexpr int64_t kDesktopFrameAdmissionQueueMs = 80;

}  // namespace

IceTransportController::IceTransportController(
    std::shared_ptr<SystemClock> clock, std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics, bool enable_srtp,
    VideoQuality video_quality, int video_frame_rate,
    VideoContentType video_content_type,
    VideoDegradationPreference video_degradation_preference)
    : clock_(clock),
      ice_agent_(ice_agent),
      enable_srtp_(enable_srtp),
      video_quality_(video_quality),
      ice_io_statistics_(ice_io_statistics),
      webrtc_clock_(webrtc::Clock::GetWebrtcClockShared(clock)),
      last_report_block_time_(
          webrtc::Timestamp::Millis(webrtc_clock_->TimeInMilliseconds())),
      b_force_i_frame_(true),
      video_codec_inited_(false),
      audio_codec_inited_(false),
      hardware_acceleration_(false),
      native_video_output_(false),
      is_running_(true),
      congestion_window_size_(DataSize::PlusInfinity()) {
  media_config_.max_frame_rate = video_frame_rate == 30 ? 30 : 60;
  media_config_.video_content_type = video_content_type;
  media_config_.video_degradation_preference =
      video_degradation_preference;
  SetPeriod(std::chrono::milliseconds(25));
  SetThreadName("IceTransportController");
}

IceTransportController::~IceTransportController() {
  if (paced_sender_) {
    paced_sender_->Shutdown();
  }
  if (task_queue_cc_) {
    task_queue_cc_->Stop();
  }
  if (task_queue_pacer_) {
    task_queue_pacer_->Stop();
  }
  if (task_queue_encode_) {
    task_queue_encode_->Stop();
  }
  if (task_queue_decode_) {
    task_queue_decode_->Stop();
  }
  if (task_queue_trans_fb_) {
    task_queue_trans_fb_->Stop();
  }

  user_data_ = nullptr;
  video_codec_inited_ = false;
  audio_codec_inited_ = false;
}

void IceTransportController::Create(bool offer_peer, std::string remote_user_id,
                                    rtp::PAYLOAD_TYPE video_codec_payload_type,
                                    bool video_rtx_enabled,
                                    bool hardware_acceleration,
                                    bool native_video_output,
                                    OnReceiveVideo on_receive_video,
                                    OnReceiveAudio on_receive_audio,
                                    OnReceiveData on_receive_data,
                                    void* user_data) {
  offer_peer_ = offer_peer;
  remote_user_id_ = remote_user_id;
  video_rtx_enabled_ = video_rtx_enabled;
  on_receive_video_ = on_receive_video;
  on_receive_audio_ = on_receive_audio;
  on_receive_data_ = on_receive_data;
  user_data_ = user_data;
  native_video_output_ = native_video_output;

  CreateCodecs(clock_, video_codec_payload_type, hardware_acceleration);

  if (enable_srtp_) {
    SrtpEngine::GlobalInit();
  }

  task_queue_cc_ = std::make_shared<TaskQueue>("congest control");
  task_queue_pacer_ = std::make_shared<TaskQueue>("pacer");
  task_queue_encode_ = std::make_shared<TaskQueueLockFree>("encode");
  task_queue_decode_ = std::make_shared<TaskQueueLockFree>("decode");
  task_queue_trans_fb_ =
      std::make_shared<TaskQueueLockFree>("transport feedback adapter");

  controller_ = std::make_unique<CongestionControl>();
  paced_sender_ = std::make_shared<PacedSender>(ice_agent_, webrtc_clock_,
                                                task_queue_pacer_);
  paced_sender_->SetPacingRates(DataRate::BitsPerSec(300000), DataRate::Zero());
  paced_sender_->SetSendBurstInterval(TimeDelta::Millis(40));
  paced_sender_->SetQueueTimeLimit(TimeDelta::Millis(
      media_config_.video_content_type == VideoContentType::ScreenContent
          ? kDesktopPacerQueueLimitMs
          : 2000));
  paced_sender_->SetAllowProbeWithoutMediaPacket(false);
  std::weak_ptr<IceTransportController> weak_this = shared_from_this();
  paced_sender_->SetOnSentPacketFunc(
      [weak_this](std::unique_ptr<webrtc::RtpPacketToSend> packet,
                  const webrtc::PacedPacketInfo& pacing_info) {
        if (auto self = weak_this.lock()) {
          auto notify_send_failure = [&]() {
            if (!packet || !packet->packet_type().has_value() ||
                (packet->packet_type() != webrtc::RtpPacketMediaType::kVideo &&
                 packet->packet_type() !=
                     webrtc::RtpPacketMediaType::kRetransmission)) {
              return;
            }
            std::shared_lock lock(self->stream_senders_mutex_);
            auto sender_it =
                self->stream_senders_.find(packet->get_stream_name());
            if (sender_it != self->stream_senders_.end() &&
                sender_it->second && sender_it->second->transceiver) {
              sender_it->second->transceiver->OnRtpPacketSendFailed(*packet);
            }
          };

          if (!self->ice_agent_) {
            notify_send_failure();
            return;
          }

          std::vector<uint8_t> protected_packet;
          const char* send_buffer = nullptr;
          size_t send_size = 0;

          if (self->enable_srtp_) {
            int len = packet->Size();

            protected_packet.resize(len + 16);
            memcpy(protected_packet.data(), packet->Buffer().data(), len);

            auto srtp_it =
                self->ssrc_to_srtp_sender_.find(packet->Ssrc());
            if (srtp_it == self->ssrc_to_srtp_sender_.end() ||
                !srtp_it->second || !srtp_it->second->valid()) {
              LOG_ERROR("No SRTP sender session for SSRC {}", packet->Ssrc());
              notify_send_failure();
              return;
            }
            if (srtp_it->second->protectRtp(protected_packet.data(), &len) < 0) {
              LOG_ERROR("SRTP protect failed for stream [{}]", packet->Ssrc());
              notify_send_failure();
              return;
            }

            send_buffer =
                reinterpret_cast<const char*>(protected_packet.data());
            send_size = static_cast<size_t>(len);
          } else {
            send_buffer =
                reinterpret_cast<const char*>(packet->Buffer().data());
            send_size = packet->Size();
          }

          // Register synchronously before the packet can generate remote
          // feedback. The previous asynchronous registration allowed a fast
          // loopback peer to report the packet before it existed in history.
          const PacketFeedbackRegistration feedback_registration =
              self->RegisterPacketForFeedback(*packet, pacing_info);
          const int send_result =
              self->ice_agent_->Send(send_buffer, send_size);
          if (send_result < 0) {
            self->RollbackPacketFeedback(*packet, feedback_registration);
            notify_send_failure();
            return;
          }

          self->OnSentPacket(*packet, feedback_registration);

          if (packet->packet_type().has_value()) {
            switch (packet->packet_type().value()) {
              case webrtc::RtpPacketMediaType::kVideo:
              case webrtc::RtpPacketMediaType::kRetransmission: {
                self->last_active_stream_ = packet->get_stream_name();
                std::shared_lock lock(self->stream_senders_mutex_);
                auto sender_it =
                    self->stream_senders_.find(self->last_active_stream_);
                if (sender_it != self->stream_senders_.end() &&
                    sender_it->second && sender_it->second->transceiver) {
                  sender_it->second->transceiver->OnSentRtpPacket(
                      std::move(packet));
                }
              } break;
              default:
                break;
            }
          }
        }
      });

  paced_sender_->SetGeneratePaddingFunc(
      [weak_this](uint32_t size, int64_t padding_time_us)
          -> std::vector<std::unique_ptr<RtpPacket>> {
        if (auto self = weak_this.lock()) {
          std::shared_lock lock(self->stream_senders_mutex_);
          auto it = self->stream_senders_.find(self->last_active_stream_);
          if (it != self->stream_senders_.end() && it->second &&
              it->second->type == StreamType::kVideo &&
              it->second->transceiver) {
            return it->second->transceiver->GeneratePadding(
                size, padding_time_us);
          }
          std::shared_ptr<StreamContext> best_ctx = nullptr;
          int64_t best_ts = std::numeric_limits<int64_t>::min();
          for (auto& [name, context] : self->stream_senders_) {
            if (context && context->type == StreamType::kVideo &&
                context->transceiver) {
              int64_t ts = context->last_active_time.value_or(0);
              if (ts > best_ts) {
                best_ts = ts;
                best_ctx = context;
              }
            }
          }
          if (best_ctx) {
            return best_ctx->transceiver->GeneratePadding(
                size, padding_time_us);
          }
          return {};
        } else {
          return {};
        }
      });

  resolution_adapter_ = std::make_unique<ResolutionAdapter>(
      video_quality_, media_config_.max_frame_rate,
      media_config_.video_content_type,
      media_config_.video_degradation_preference);

  {
    std::shared_lock lock(stream_senders_mutex_);
    for (auto& [channel_name, context] : stream_senders_) {
      if (context) {
        if (context->type == StreamType::kVideo) {
          std::static_pointer_cast<VideoChannelSend>(context->transceiver)
              ->Initialize(video_codec_payload_type, paced_sender_,
                           video_rtx_enabled_);
        } else if (context->type == StreamType::kAudio) {
          context->transceiver->Initialize(rtp::PAYLOAD_TYPE::OPUS,
                                           paced_sender_);
        } else if (context->type == StreamType::kData) {
          rtp::PAYLOAD_TYPE data_pt = context->reliable
                                          ? rtp::PAYLOAD_TYPE::KCP
                                          : rtp::PAYLOAD_TYPE::DATA;
          context->transceiver->Initialize(data_pt, paced_sender_);
        }
      }
    }
  }

  {
    std::shared_lock lock(stream_receivers_mutex_);
    for (auto& [_, context] : stream_receivers_) {
      if (context) {
        if (context->type == StreamType::kVideo) {
          context->transceiver->Initialize(video_codec_payload_type);
        } else if (context->type == StreamType::kAudio) {
          context->transceiver->Initialize(rtp::PAYLOAD_TYPE::OPUS);
        } else if (context->type == StreamType::kData) {
          rtp::PAYLOAD_TYPE data_pt = context->reliable
                                          ? rtp::PAYLOAD_TYPE::KCP
                                          : rtp::PAYLOAD_TYPE::DATA;
          context->transceiver->Initialize(data_pt);
        }
      }
    }
  }

  UpdateMediaTransportState();
}

void IceTransportController::Destroy() {
  is_running_.store(false);

  if (paced_sender_) {
    paced_sender_->Shutdown();
  }

  if (task_queue_cc_) {
    task_queue_cc_->Stop();
  }
  if (task_queue_pacer_) {
    task_queue_pacer_->Stop();
  }
  if (task_queue_encode_) {
    task_queue_encode_->Stop();
  }
  if (task_queue_decode_) {
    task_queue_decode_->Stop();
  }
  if (task_queue_trans_fb_) {
    task_queue_trans_fb_->Stop();
  }

  std::map<std::string, std::shared_ptr<StreamContext>> senders;
  std::map<std::string, std::shared_ptr<StreamContext>> receivers;
  std::vector<std::shared_ptr<MediaCodec>> codecs;

  {
    std::unique_lock lock(stream_senders_mutex_);
    senders.swap(stream_senders_);
  }

  {
    std::unique_lock lock(stream_receivers_mutex_);
    receivers.swap(stream_receivers_);
  }

  for (auto& [_, context] : senders) {
    if (context && context->transceiver) {
      context->transceiver->Destroy();
    }
    if (context && context->codec) {
      codecs.push_back(std::move(context->codec));
    }
  }
  for (auto& [_, context] : receivers) {
    if (context && context->transceiver) {
      context->transceiver->Destroy();
    }
    if (context && context->codec) {
      codecs.push_back(std::move(context->codec));
    }
  }

  // Destroy contexts before codecs so a callback cannot become the last owner
  // of its encoder and destroy VideoToolbox from inside its own callback.
  senders.clear();
  receivers.clear();
  codecs.clear();

  Stop();
}

uint32_t IceTransportController::AddVideoSendChannel(
    const std::string& channel_name) {
  std::unique_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it != stream_senders_.end() && it->second) {
    uint32_t ssrc =
        it->second->transceiver ? it->second->transceiver->GetSsrc() : 0;
    LOG_ERROR("Stream sender [{}] already exist with ssrc [{}]", channel_name,
              ssrc);
    return ssrc;
  }

  auto& context = stream_senders_[channel_name];
  if (!context) {
    context = std::make_shared<StreamContext>();
    context->name = channel_name;
    context->type = StreamType::kVideo;
    context->direction = StreamDirection::kSend;
  }
  if (!context->transceiver) {
    context->transceiver = std::make_shared<VideoChannelSend>(
        channel_name, clock_, ice_agent_, ice_io_statistics_);
    if (!context->transceiver) {
      LOG_ERROR("Video stream sender [{}] create failed", channel_name);
      return -1;
    }
    context->ssrc = context->transceiver->GetSsrc();
    context->rtx_ssrc = context->transceiver->GetRtxSsrc();
  }
  return context->transceiver ? context->transceiver->GetSsrc() : 0;
}

uint32_t IceTransportController::GetVideoRtxSsrc(
    const std::string& channel_name) {
  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second ||
      it->second->type != StreamType::kVideo || !it->second->transceiver) {
    return 0;
  }
  return it->second->transceiver->GetRtxSsrc();
}

uint32_t IceTransportController::AddAudioSendChannel(
    const std::string& channel_name) {
  std::unique_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it != stream_senders_.end() && it->second) {
    uint32_t ssrc =
        it->second->transceiver ? it->second->transceiver->GetSsrc() : 0;
    LOG_ERROR("Stream sender [{}] already exists with ssrc [{}]", channel_name,
              ssrc);
    return ssrc;
  }

  auto& context = stream_senders_[channel_name];
  if (!context) {
    context = std::make_shared<StreamContext>();
    context->name = channel_name;
    context->type = StreamType::kAudio;
    context->direction = StreamDirection::kSend;
  }
  if (!context->transceiver) {
    context->transceiver = std::make_shared<AudioChannelSend>(
        channel_name, ice_agent_, ice_io_statistics_);
    if (!context->transceiver) {
      LOG_ERROR("Audio stream sender [{}] create failed", channel_name);
      return -1;
    }
    context->ssrc = context->transceiver->GetSsrc();
  }
  return context->transceiver ? context->transceiver->GetSsrc() : 0;
}

uint32_t IceTransportController::AddDataSendChannel(
    const std::string& channel_name, bool reliable) {
  std::unique_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it != stream_senders_.end() && it->second) {
    uint32_t ssrc =
        it->second->transceiver ? it->second->transceiver->GetSsrc() : 0;
    LOG_ERROR("Stream sender [{}] already exists with ssrc [{}]", channel_name,
              ssrc);
    return ssrc;
  }

  auto& context = stream_senders_[channel_name];
  if (!context) {
    context = std::make_shared<StreamContext>();
    context->name = channel_name;
    context->type = StreamType::kData;
    context->direction = StreamDirection::kSend;
    context->reliable = reliable;
  }
  if (!context->transceiver) {
    context->transceiver = std::make_shared<DataChannelSend>(
        channel_name, ice_agent_, ice_io_statistics_, reliable);
    if (!context->transceiver) {
      LOG_ERROR("Data stream sender [{}] create failed", channel_name);
      return -1;
    }
    context->ssrc = context->transceiver->GetSsrc();
  }
  return context->transceiver ? context->transceiver->GetSsrc() : 0;
}

uint32_t IceTransportController::AddVideoReceiveChannel(
    const std::string& channel_name, uint32_t ssrc, uint32_t rtx_ssrc) {
  std::unique_lock lock(stream_receivers_mutex_);
  auto it = stream_receivers_.find(channel_name);
  if (it != stream_receivers_.end() && it->second) {
    LOG_ERROR("Stream receiver [{}] already exists with ssrc [{}]",
              channel_name, ssrc);
    return ssrc;
  }
  auto& context = stream_receivers_[channel_name];
  if (!context) {
    context = std::make_shared<StreamContext>();
    context->name = channel_name;
    context->type = StreamType::kVideo;
    context->direction = StreamDirection::kReceive;
    context->ssrc = ssrc;
    if (rtx_ssrc != 0 && rtx_ssrc != ssrc) {
      context->rtx_ssrc = rtx_ssrc;
    }
    ssrc_to_name_[ssrc] = channel_name;
    if (context->rtx_ssrc.has_value()) {
      ssrc_to_name_[*context->rtx_ssrc] = channel_name;
    }
  }

  if (!context->transceiver) {
    std::weak_ptr<IceTransportController> weak_self = shared_from_this();
    context->transceiver = std::make_shared<VideoChannelReceive>(
        channel_name, ssrc, context->rtx_ssrc.value_or(0), clock_, ice_agent_,
        ice_io_statistics_,
        [this, weak_self,
         channel_name](std::unique_ptr<ReceivedFrame> received_frame) {
          if (auto self = weak_self.lock()) {
            OnReceiveCompleteFrame(std::move(received_frame), channel_name);
          }
        });

    if (!context->transceiver) {
      LOG_ERROR("Video stream receiver [{}:{}] create failed", channel_name,
                ssrc);
      return 0;
    }
  }

  return ssrc;
}

uint32_t IceTransportController::AddAudioReceiveChannel(
    const std::string& channel_name, uint32_t ssrc) {
  std::unique_lock lock(stream_receivers_mutex_);
  auto it = stream_receivers_.find(channel_name);
  if (it != stream_receivers_.end() && it->second) {
    LOG_ERROR("Stream receiver [{}] already exists with ssrc [{}]",
              channel_name, ssrc);
    return ssrc;
  }
  auto& context = stream_receivers_[channel_name];
  if (!context) {
    context = std::make_shared<StreamContext>();
    context->name = channel_name;
    context->type = StreamType::kAudio;
    context->direction = StreamDirection::kReceive;
    context->ssrc = ssrc;
    ssrc_to_name_[ssrc] = channel_name;
  }

  if (!context->transceiver) {
    std::weak_ptr<IceTransportController> weak_self = shared_from_this();
    context->transceiver = std::make_shared<AudioChannelReceive>(
        channel_name, ssrc, ice_agent_, ice_io_statistics_,
        [this, weak_self, channel_name](const char* data, size_t size) {
          if (auto self = weak_self.lock()) {
            OnReceiveCompleteAudio(data, size, channel_name);
          }
        });
    if (!context->transceiver) {
      LOG_ERROR("Audio stream receiver [{}:{}] create failed", channel_name,
                ssrc);
      return 0;
    }
  }

  return ssrc;
}

uint32_t IceTransportController::AddDataReceiveChannel(
    const std::string& channel_name, uint32_t ssrc, bool reliable) {
  std::unique_lock lock(stream_receivers_mutex_);
  auto it = stream_receivers_.find(channel_name);
  if (it != stream_receivers_.end() && it->second) {
    LOG_ERROR("Stream receiver [{}] already exists with ssrc [{}]",
              channel_name, ssrc);
    return ssrc;
  }
  auto& context = stream_receivers_[channel_name];
  if (!context) {
    context = std::make_shared<StreamContext>();
    context->name = channel_name;
    context->type = StreamType::kData;
    context->direction = StreamDirection::kReceive;
    context->ssrc = ssrc;
    context->reliable = reliable;
    ssrc_to_name_[ssrc] = channel_name;
  }

  if (!context->transceiver) {
    std::weak_ptr<IceTransportController> weak_self = shared_from_this();
    context->transceiver = std::make_shared<DataChannelReceive>(
        channel_name, ssrc, ice_agent_, ice_io_statistics_,
        [this, weak_self, channel_name](const char* data, size_t size) {
          if (auto self = weak_self.lock()) {
            OnReceiveCompleteData(data, size, channel_name);
          }
        },
        reliable);
    if (!context->transceiver) {
      LOG_ERROR("Data stream receiver [{}:{}] create failed", channel_name,
                ssrc);
      return 0;
    }
  }

  return ssrc;
}

bool IceTransportController::CheckSteamContext(
    const std::string& channel_name,
    const std::shared_ptr<StreamContext>& context) {
  if (!context) {
    LOG_ERROR("Stream context [{}] not found", channel_name);
    return false;
  }

  if (!context->transceiver) {
    LOG_ERROR("Stream transceiver [{}] not found", channel_name);
    return false;
  }

  if (context->type != StreamType::kData && !context->codec) {
    LOG_ERROR("Stream codec [{}] not found", channel_name);
    return false;
  }

  return true;
}

int IceTransportController::SendVideo(const MiniRtcVideoFrame* video_frame,
                                      const std::string& channel_name) {
  if (!is_running_.load()) {
    return -1;
  }

  const MiniRtcNativeVideoFrame* native_frame =
      GetNativeVideoFrameInput(video_frame);
  size_t required_cpu_size = 0;
  const bool valid_cpu_frame =
      video_frame &&
      GetNv12FrameSize(video_frame->width, video_frame->height,
                       &required_cpu_size) &&
      video_frame->data && video_frame->size >= required_cpu_size;
  if (!native_frame && !valid_cpu_frame) {
    LOG_ERROR("Invalid video frame for stream [{}]", channel_name);
    return -1;
  }

  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) {
    if (!is_running_.load()) {
      return -1;
    }
    LOG_ERROR("Failed to find stream sender [{}]", channel_name);
    return -1;
  }
  auto& context = it->second;
  if (!CheckSteamContext(channel_name, context)) {
    return -1;
  }

  if (task_queue_encode_) {
    context->capture_input_frame_total.fetch_add(1,
                                                 std::memory_order_relaxed);
    if (media_config_.video_content_type == VideoContentType::ScreenContent &&
        paced_sender_ &&
        (paced_sender_->ExpectedQueueTime() >=
             TimeDelta::Millis(kDesktopFrameAdmissionQueueMs) ||
         paced_sender_->OldestPacketWaitTime() >=
             TimeDelta::Millis(kDesktopPacerQueueLimitMs))) {
      // Remote desktop frames expire quickly. Stop admitting new frames while
      // the packet queue drains instead of extending end-to-end latency with
      // content the viewer will only see long after it was captured.
      context->pacer_rejected_frame_total.fetch_add(
          1, std::memory_order_relaxed);
      return 0;
    }

    // Reject coalesced frames before copying the full desktop buffer. At 4K,
    // copying frames that are known to be dropped can consume hundreds of MB/s
    // and slow the capture callback itself below its configured frame rate.
    if (task_queue_encode_->PendingTasks() > 0) {
      context->encode_queue_dropped_frame_total.fetch_add(
          1, std::memory_order_relaxed);
      return 0;
    }

    RawFrame raw_frame = native_frame
                             ? RawFrame(*native_frame)
                             : RawFrame(
                                   reinterpret_cast<const uint8_t*>(
                                       video_frame->data),
                                   video_frame->size, video_frame->width,
                                   video_frame->height);
    raw_frame.SetCapturedTimestamp(
        video_frame->captured_timestamp != 0
            ? static_cast<int64_t>(video_frame->captured_timestamp)
            : clock_->CurrentTimeUs());

    // Save the original capture resolution so later resolution changes keep the
    // same aspect ratio.
    if (context->source_width <= 0 || context->source_height <= 0 ||
        context->source_width != raw_frame.Width() ||
        context->source_height != raw_frame.Height()) {
      context->source_width = raw_frame.Width();
      context->source_height = raw_frame.Height();
      context->source_resolution_initialized_ms = clock_->CurrentTimeMs();
      context->pending_mapped_width.reset();
      context->pending_mapped_height.reset();
      context->mapping_stability_count = 0;
      context->pending_mapped_since_ms = 0;
      context->ResetResolutionUpgradeProbe();
      context->ResetEncodedFrameRateTracking();
      context->ResetEncoderQualityTracking();
      context->ResetEncodeQueueDelayTracking();
      context->post_upgrade_protection_until_ms = 0;
    }

    // Consume a key-frame request only after this frame is known to be
    // accepted by the encode queue. Otherwise a busy queue would drop both
    // the captured frame and the one-shot FIR request.
    bool force_i_frame = false;
    if (b_force_i_frame_.exchange(false)) {
      force_i_frame = true;
    }
    {
      std::lock_guard<std::mutex> lock(force_i_frame_streams_mutex_);
      auto it_force = force_i_frame_streams_.find(channel_name);
      if (it_force != force_i_frame_streams_.end()) {
        force_i_frame = true;
        force_i_frame_streams_.erase(it_force);
      }
    }

    std::weak_ptr<IceTransportController> weak_self = shared_from_this();
    std::weak_ptr<StreamContext> weak_context = context;
    std::shared_ptr<TaskQueueLockFree> encode_queue = task_queue_encode_;
    auto post_encode = [weak_self, weak_context, encode_queue, channel_name,
                        force_i_frame](RawFrame&& frame) mutable {
      encode_queue->PostTask([weak_self, weak_context, encode_queue,
                              channel_name, force_i_frame,
                              frame = std::move(frame)]() mutable {
        auto self = weak_self.lock();
        auto context = weak_context.lock();
        if (!self || !context || !self->is_running_.load()) {
          return;
        }

        if (!context->codec) {
          return;
        }
        if (const auto* native_frame = frame.NativeFrame();
            native_frame &&
            !context->codec->SupportsNativeFrameInput(native_frame->type)) {
          if (!frame.MaterializeNativeFrame()) {
            LOG_ERROR("Failed to materialize native video frame for stream [{}]",
                      channel_name);
            return;
          }
        }
        int64_t queue_delay_ms = encode_queue->CurrentTaskQueueDelayMs();
        if (force_i_frame) {
          if (context->codec->ForceIdr() != 0) {
            // Keep the request pending so a transient encoder failure cannot
            // turn a recoverable loss into a long wait for the periodic IDR.
            std::lock_guard<std::mutex> lock(
                self->force_i_frame_streams_mutex_);
            self->force_i_frame_streams_.insert(channel_name);
            LOG_ERROR("Failed to force I frame for stream [{}]", channel_name);
          } else {
            LOG_INFO("Force I frame for stream [{}]", channel_name);
          }
        }
        context->codec->Encode(
            std::move(frame),
            [weak_self, weak_context, channel_name, queue_delay_ms,
             is_first_callback =
                 true](const EncodedFrame& encoded_frame) mutable -> int {
              auto self = weak_self.lock();
              auto context = weak_context.lock();
              if (!self || !context || !self->is_running_.load()) {
                return -1;
              }

              const bool measure_encode_delay = is_first_callback;
              is_first_callback = false;
              return self->OnVideoEncoded(channel_name, context,
                                          static_cast<int>(queue_delay_ms),
                                          measure_encode_delay, encoded_frame);
            });
      });
    };

    if (context->target_width.has_value() &&
        context->target_height.has_value() &&
        context->target_width.value() < raw_frame.Width() &&
        context->target_height.value() < raw_frame.Height()) {
      if (raw_frame.NativeFrame() && !raw_frame.MaterializeNativeFrame()) {
        LOG_ERROR("Failed to materialize native frame before scaling [{}x{}]",
                  raw_frame.Width(), raw_frame.Height());
        return -1;
      }
      RawFrame scaled_frame(context->target_width.value() *
                            context->target_height.value() * 3 / 2);

      scaled_frame.SetWidth(context->target_width.value());
      scaled_frame.SetHeight(context->target_height.value());
      scaled_frame.SetCapturedTimestamp(raw_frame.CapturedTimestamp());

      if (resolution_adapter_->ResolutionDowngrade(
              raw_frame, context->target_width.value(),
              context->target_height.value(), scaled_frame) != 0) {
        LOG_ERROR("Failed to scale video frame from [{}x{}] to [{}x{}]",
                  raw_frame.Width(), raw_frame.Height(),
                  context->target_width.value(),
                  context->target_height.value());
        return -1;
      }

      post_encode(std::move(scaled_frame));
    } else {
      post_encode(std::move(raw_frame));
    }
  }

  return 0;
}

void IceTransportController::MaybeDegradeResolutionOnEncodeTime(
    const std::string& channel_name, int queue_delay_ms,
    const EncodedFrame& encoded_frame) {
  const bool maintain_frame_rate =
      media_config_.video_degradation_preference ==
      VideoDegradationPreference::MaintainFrameRate;
  const bool maintain_resolution =
      media_config_.video_degradation_preference ==
      VideoDegradationPreference::MaintainResolution;
  const bool balanced = media_config_.video_degradation_preference ==
                        VideoDegradationPreference::Balanced;
  const int frame_budget_ms =
      std::max(1, 1000 / std::max(1, media_config_.max_frame_rate));
  const int delay_threshold_ms =
      maintain_frame_rate
          ? std::max(2, frame_budget_ms / 3)
          : (balanced ? std::max(4, frame_budget_ms / 2) : 8);
  const int minimum_frame_rate =
      VideoAdaptationPolicy::MinimumFrameRate(media_config_.max_frame_rate);
  // A lower spatial rung must have enough temporal margin to absorb the next
  // resolution step. For 60 fps streams, 45 fps remains the downgrade floor
  // while 55 fps is required before an upgrade is attempted.
  const int minimum_upgrade_frame_rate =
      VideoAdaptationPolicy::UpgradeFrameRate(media_config_.max_frame_rate);
  constexpr int kCriticalFrameRate = 20;
  constexpr int kFrameRateWindowMs = 1000;
  constexpr int kFrameRateHealthyDurationMs = 2000;
  constexpr int kUpgradeProbeMinDurationMs = 2000;
  constexpr int kPostUpgradeProtectionMs = 2000;
  constexpr int kQueueDelayWindowMs = 1000;
  constexpr int kQueueBacklogSustainMs = 500;
  constexpr int kCriticalQueueBacklogSustainMs = 250;
  constexpr int kCriticalQueueDelayMs = 100;
  constexpr float kMaxNormalizedQpForUpgrade = 0.60f;
  constexpr float kQpEwmaAlpha = 0.10f;
  constexpr int kUpgradeProbeBackoffBaseMs = 3000;
  constexpr int kMaxUpgradeProbeBackoffMs = 24000;
  constexpr int upgrade_cooldown_ms = 3000;
  const int downgrade_cooldown_ms =
      maintain_frame_rate ? 750 : (balanced ? 1500 : 3000);

  if (!is_running_.load()) return;

  std::unique_lock lock(stream_senders_mutex_);
  if (!is_running_.load()) return;
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) return;
  std::shared_ptr<StreamContext> context = it->second;
  context->last_encoder_quality_stats = encoded_frame.QualityStats();
  if (context->last_encoder_quality_stats.HasQp()) {
    const float normalized_qp = std::clamp(
        context->last_encoder_quality_stats.NormalizedQp(), 0.0f, 1.0f);
    context->normalized_qp_ewma =
        context->normalized_qp_ewma < 0.0f
            ? normalized_qp
            : context->normalized_qp_ewma * (1.0f - kQpEwmaAlpha) +
                  normalized_qp * kQpEwmaAlpha;
  }

  const int64_t now_ms = clock_->CurrentTimeMs();
  const uint64_t capture_input_total =
      context->capture_input_frame_total.load(std::memory_order_acquire);
  const uint64_t pacer_rejected_total =
      context->pacer_rejected_frame_total.load(std::memory_order_acquire);
  const uint64_t encode_queue_dropped_total =
      context->encode_queue_dropped_frame_total.load(
          std::memory_order_acquire);
  if (context->frame_admission_window_started_ms == 0) {
    context->frame_admission_window_started_ms = now_ms;
    context->frame_admission_window_capture_start = capture_input_total;
    context->frame_admission_window_pacer_rejected_start =
        pacer_rejected_total;
    context->frame_admission_window_encode_queue_dropped_start =
        encode_queue_dropped_total;
  } else {
    const int64_t admission_window_ms =
        now_ms - context->frame_admission_window_started_ms;
    if (admission_window_ms >=
        VideoAdaptationPolicy::kFrameHealthWindowMs) {
      context->frame_admission_capture_samples =
          capture_input_total - context->frame_admission_window_capture_start;
      context->frame_admission_pacer_rejected_samples =
          pacer_rejected_total -
          context->frame_admission_window_pacer_rejected_start;
      context->frame_admission_encode_queue_dropped_samples =
          encode_queue_dropped_total -
          context->frame_admission_window_encode_queue_dropped_start;
      context->measured_capture_input_frame_rate = static_cast<int>(
          (context->frame_admission_capture_samples * 1000 +
           admission_window_ms / 2) /
          admission_window_ms);
      context->measured_pacer_rejection_percent =
          context->frame_admission_capture_samples > 0
              ? static_cast<int>(
                    (context->frame_admission_pacer_rejected_samples * 100 +
                     context->frame_admission_capture_samples / 2) /
                    context->frame_admission_capture_samples)
              : 0;
      context->measured_encode_queue_drop_percent =
          context->frame_admission_capture_samples > 0
              ? static_cast<int>(
                    (context->frame_admission_encode_queue_dropped_samples *
                         100 +
                     context->frame_admission_capture_samples / 2) /
                    context->frame_admission_capture_samples)
              : 0;
      context->frame_admission_metrics_ready = true;
      context->frame_admission_window_started_ms = now_ms;
      context->frame_admission_window_capture_start = capture_input_total;
      context->frame_admission_window_pacer_rejected_start =
          pacer_rejected_total;
      context->frame_admission_window_encode_queue_dropped_start =
          encode_queue_dropped_total;
    }
  }

  bool encoded_frame_rate_window_updated = false;
  if (context->encoded_frame_rate_window_started_ms == 0) {
    context->encoded_frame_rate_window_started_ms = now_ms;
    context->encoded_frame_rate_window_frame_count = 1;
  } else {
    ++context->encoded_frame_rate_window_frame_count;
    const int64_t frame_rate_window_ms =
        now_ms - context->encoded_frame_rate_window_started_ms;
    if (frame_rate_window_ms >= kFrameRateWindowMs) {
      context->measured_encoded_frame_rate = static_cast<int>(
          (static_cast<int64_t>(context->encoded_frame_rate_window_frame_count) *
               1000 +
           frame_rate_window_ms / 2) /
          frame_rate_window_ms);
      context->encoded_frame_rate_ready = true;
      if (context->encoded_frame_rate_valid_window_count <
          context->encoded_frame_rate_windows.size()) {
        context->encoded_frame_rate_windows
            [context->encoded_frame_rate_valid_window_count++] =
                context->measured_encoded_frame_rate;
      } else {
        for (size_t i = 1; i < context->encoded_frame_rate_windows.size();
             ++i) {
          context->encoded_frame_rate_windows[i - 1] =
              context->encoded_frame_rate_windows[i];
        }
        context->encoded_frame_rate_windows.back() =
            context->measured_encoded_frame_rate;
      }
      encoded_frame_rate_window_updated = true;
      if (VideoAdaptationPolicy::IsUpgradeFrameRateHealthy(
              media_config_.max_frame_rate,
              context->measured_encoded_frame_rate)) {
        if (context->encoded_frame_rate_healthy_since_ms == 0) {
          context->encoded_frame_rate_healthy_since_ms = now_ms;
        }
      } else {
        context->encoded_frame_rate_healthy_since_ms = 0;
      }
      context->encoded_frame_rate_window_started_ms = now_ms;
      context->encoded_frame_rate_window_frame_count = 0;
    }
  }

  const bool encoded_frame_rate_below_minimum =
      context->encoded_frame_rate_ready &&
      context->measured_encoded_frame_rate < minimum_frame_rate;
  const bool encoded_frame_rate_persistently_low =
      VideoAdaptationPolicy::IsEncodedFrameRatePersistentlyLow(
          media_config_.max_frame_rate, context->encoded_frame_rate_windows,
          context->encoded_frame_rate_valid_window_count);
  const auto frame_health = VideoAdaptationPolicy::EvaluateFrameHealth(
      media_config_.max_frame_rate, encoded_frame_rate_persistently_low,
      context->frame_admission_metrics_ready,
      context->measured_capture_input_frame_rate,
      context->frame_admission_capture_samples,
      context->frame_admission_pacer_rejected_samples,
      context->frame_admission_encode_queue_dropped_samples);
  const bool startup_critical_encode_backlog_candidate =
      queue_delay_ms >= kCriticalQueueDelayMs &&
      (!context->encoded_frame_rate_ready ||
       context->measured_encoded_frame_rate < kCriticalFrameRate);
  if (startup_critical_encode_backlog_candidate) {
    ++context->encode_exceed_count;
  } else {
    context->encode_exceed_count = 0;
  }
  if (context->encode_queue_delay_tracking_started_ms == 0) {
    context->encode_queue_delay_tracking_started_ms = now_ms;
  }
  context->encode_queue_delay_samples.emplace_back(now_ms, queue_delay_ms);
  while (!context->encode_queue_delay_samples.empty() &&
         context->encode_queue_delay_samples.front().first <
             now_ms - kQueueDelayWindowMs) {
    context->encode_queue_delay_samples.pop_front();
  }

  int64_t queue_delay_sum_ms = 0;
  std::vector<int> queue_delay_values;
  queue_delay_values.reserve(context->encode_queue_delay_samples.size());
  for (const auto& [_, delay_ms] : context->encode_queue_delay_samples) {
    queue_delay_sum_ms += delay_ms;
    queue_delay_values.push_back(delay_ms);
  }
  if (!queue_delay_values.empty()) {
    context->average_encode_queue_delay_ms = static_cast<int>(
        (queue_delay_sum_ms + queue_delay_values.size() / 2) /
        queue_delay_values.size());
    const size_t p95_index =
        (queue_delay_values.size() * 95 + 99) / 100 - 1;
    std::nth_element(queue_delay_values.begin(),
                     queue_delay_values.begin() + p95_index,
                     queue_delay_values.end());
    context->p95_encode_queue_delay_ms = queue_delay_values[p95_index];
  }
  context->encode_queue_delay_window_ready =
      now_ms - context->encode_queue_delay_tracking_started_ms >=
      kQueueDelayWindowMs;

  const bool normal_queue_pressure =
      context->encode_queue_delay_window_ready &&
      encoded_frame_rate_below_minimum &&
      context->average_encode_queue_delay_ms >= delay_threshold_ms;
  const bool severe_queue_pressure =
      context->encode_queue_delay_window_ready &&
      context->p95_encode_queue_delay_ms >= frame_budget_ms * 2;
  const bool critical_queue_pressure =
      context->encode_queue_delay_window_ready &&
      context->p95_encode_queue_delay_ms >= kCriticalQueueDelayMs;

  auto update_pressure_since = [now_ms](bool pressure, int64_t* since_ms) {
    if (pressure) {
      if (*since_ms == 0) {
        *since_ms = now_ms;
      }
    } else {
      *since_ms = 0;
    }
  };
  update_pressure_since(normal_queue_pressure || severe_queue_pressure,
                        &context->encode_backlog_since_ms);
  update_pressure_since(severe_queue_pressure,
                        &context->severe_encode_backlog_since_ms);
  update_pressure_since(critical_queue_pressure,
                        &context->critical_encode_backlog_since_ms);
  update_pressure_since(frame_health.capture_frame_rate_low,
                        &context->low_capture_frame_rate_since_ms);
  update_pressure_since(frame_health.pacer_rejection_high,
                        &context->high_pacer_rejection_since_ms);
  update_pressure_since(frame_health.encode_queue_drop_high,
                        &context->high_encode_queue_drop_since_ms);

  const bool sustained_encode_backlog =
      context->encode_backlog_since_ms > 0 &&
      now_ms - context->encode_backlog_since_ms >= kQueueBacklogSustainMs;
  const bool sustained_severe_encode_backlog =
      context->severe_encode_backlog_since_ms > 0 &&
      now_ms - context->severe_encode_backlog_since_ms >=
          kQueueBacklogSustainMs;
  const bool sustained_critical_encode_backlog =
      context->critical_encode_backlog_since_ms > 0 &&
      now_ms - context->critical_encode_backlog_since_ms >=
          kCriticalQueueBacklogSustainMs;
  const bool startup_critical_encode_backlog =
      context->encode_exceed_count >= 3;
  const bool sustained_low_encoded_frame_rate =
      frame_health.encoded_frame_rate_low;
  const bool sustained_low_capture_frame_rate =
      VideoAdaptationPolicy::IsFrameHealthPressureSustained(
          now_ms, context->low_capture_frame_rate_since_ms);
  const bool sustained_high_pacer_rejection =
      VideoAdaptationPolicy::IsFrameHealthPressureSustained(
          now_ms, context->high_pacer_rejection_since_ms);
  const bool sustained_high_encode_queue_drop =
      VideoAdaptationPolicy::IsFrameHealthPressureSustained(
          now_ms, context->high_encode_queue_drop_since_ms);
  if (encoded_frame_rate_window_updated) {
    const size_t low_frame_rate_window_count =
        VideoAdaptationPolicy::CountLowFrameRateWindows(
            media_config_.max_frame_rate,
            context->encoded_frame_rate_windows,
            context->encoded_frame_rate_valid_window_count);
    LOG_INFO(
        "Video frame health: channel={} encoded_fps={} low_windows={}/{} "
        "capture_fps={} pacer_reject_percent={} "
        "encode_queue_drop_percent={} delay_avg_ms={} delay_p95_ms={} "
        "resolution={}x{}",
        channel_name, context->measured_encoded_frame_rate,
        low_frame_rate_window_count,
        context->encoded_frame_rate_valid_window_count,
        context->measured_capture_input_frame_rate,
        context->measured_pacer_rejection_percent,
        context->measured_encode_queue_drop_percent,
        context->average_encode_queue_delay_ms,
        context->p95_encode_queue_delay_ms, encoded_frame.EncodedWidth(),
        encoded_frame.EncodedHeight());
  }
  if (context->resolution_upgrade_probe_active) {
    ++context->resolution_upgrade_probe_sample_count;
  }

  // Resolution-priority mode intentionally accepts a lower temporal rate.
  // The bounded encode queue already coalesces excess input frames, so do not
  // undo that choice by reducing spatial detail here.
  if (maintain_resolution) {
    return;
  }

  auto set_encoding_speed_priority =
      [&](bool prioritize_speed) -> std::optional<bool> {
    std::shared_ptr<MediaCodec> codec = context->codec;
    lock.unlock();
    const int result =
        codec ? codec->SetPrioritizeEncodingSpeedOverQuality(prioritize_speed)
              : -1;
    lock.lock();

    auto current = stream_senders_.find(channel_name);
    if (!is_running_.load() || current == stream_senders_.end() ||
        current->second != context || context->codec != codec) {
      return std::nullopt;
    }
    if (result != 0) {
      return false;
    }

    context->encoding_speed_priority_enabled = prioritize_speed;
    context->encode_exceed_count = 0;
    context->encode_below_threshold_count = 0;
    return true;
  };

  if (!context->resolution_upgrade_probe_active &&
      !context->encoding_speed_priority_enabled &&
      (sustained_encode_backlog || startup_critical_encode_backlog) &&
      context->codec &&
      media_config_.video_content_type == VideoContentType::ScreenContent &&
      context->codec->SupportsDynamicEncodingSpeedPriority()) {
    const std::optional<bool> changed = set_encoding_speed_priority(true);
    if (!changed.has_value()) {
      return;
    }
    if (changed.value()) {
      LOG_INFO(
          "Encoding queue backlog; enable speed priority: channel={} delay_ms={}",
          channel_name, queue_delay_ms);
      // Give the faster encoder setting a fresh observation window before
      // deciding whether balanced mode also needs a spatial downgrade.
      context->ResetEncodedFrameRateTracking();
      context->ResetEncoderQualityTracking();
      context->ResetEncodeQueueDelayTracking();
      return;
    }
    // If the encoder rejects the property, fall through to the existing
    // resolution downgrade instead of repeatedly delaying adaptation.
  }

  const bool balanced_pipeline_healthy =
      balanced && !context->resolution_upgrade_probe_active &&
      context->encoding_speed_priority_enabled &&
      context->encoded_frame_rate_ready &&
      VideoAdaptationPolicy::IsUpgradeFrameRateHealthy(
          media_config_.max_frame_rate,
          context->measured_encoded_frame_rate) &&
      context->encoded_frame_rate_healthy_since_ms > 0 &&
      now_ms - context->encoded_frame_rate_healthy_since_ms >=
          kFrameRateHealthyDurationMs &&
      (!context->encode_queue_delay_window_ready ||
       (context->average_encode_queue_delay_ms < delay_threshold_ms &&
        context->p95_encode_queue_delay_ms < frame_budget_ms)) &&
      (context->normalized_qp_ewma < 0.0f ||
       context->normalized_qp_ewma <= kMaxNormalizedQpForUpgrade);
  if (balanced_pipeline_healthy) {
    const std::optional<bool> changed = set_encoding_speed_priority(false);
    if (!changed.has_value()) {
      return;
    }
    if (changed.value()) {
      LOG_INFO(
          "Balanced pipeline healthy; restore encoder quality priority: "
          "channel={} fps={} required_fps={} qp={} delay_avg_ms={} "
          "delay_p95_ms={}",
          channel_name, context->measured_encoded_frame_rate,
          minimum_upgrade_frame_rate,
          context->last_encoder_quality_stats.qp,
          context->average_encode_queue_delay_ms,
          context->p95_encode_queue_delay_ms);
      context->ResetEncodedFrameRateTracking();
      context->ResetEncoderQualityTracking();
      context->ResetEncodeQueueDelayTracking();
      return;
    }
  }

  auto base = [&]() -> std::pair<int, int> {
    if (context->target_width && context->target_height)
      return {*context->target_width, *context->target_height};
    return {static_cast<int>(encoded_frame.EncodedWidth()),
            static_cast<int>(encoded_frame.EncodedHeight())};
  };

  if (context->resolution_upgrade_probe_active) {
    const int64_t probe_duration_ms =
        now_ms - context->resolution_upgrade_probe_started_ms;
    const int probe_frame_rate =
        probe_duration_ms > 0
            ? static_cast<int>(
                  (static_cast<int64_t>(
                       context->resolution_upgrade_probe_sample_count) *
                       1000 +
                   probe_duration_ms / 2) /
                  probe_duration_ms)
            : 0;
    const bool probe_backlogged =
        sustained_severe_encode_backlog ||
        sustained_critical_encode_backlog ||
        (startup_critical_encode_backlog &&
         probe_duration_ms >= downgrade_cooldown_ms);
    const bool probe_frame_rate_too_low =
        probe_duration_ms >= kUpgradeProbeMinDurationMs &&
        !VideoAdaptationPolicy::IsUpgradeFrameRateHealthy(
            media_config_.max_frame_rate, probe_frame_rate);
    const bool probe_qp_too_high =
        probe_duration_ms >= kUpgradeProbeMinDurationMs &&
        context->normalized_qp_ewma >= 0.0f &&
        context->normalized_qp_ewma > kMaxNormalizedQpForUpgrade;

    if (probe_backlogged || probe_frame_rate_too_low || probe_qp_too_high) {
      const int rollback_width =
          context->resolution_upgrade_probe_base_width;
      const int rollback_height =
          context->resolution_upgrade_probe_base_height;
      const int failed_width =
          context->resolution_upgrade_probe_target_width;
      const int failed_height =
          context->resolution_upgrade_probe_target_height;
      context->resolution_upgrade_probe_failure_count = std::min(
          context->resolution_upgrade_probe_failure_count + 1, 3);
      const int backoff_ms = std::min(
          kMaxUpgradeProbeBackoffMs,
          kUpgradeProbeBackoffBaseMs
              << context->resolution_upgrade_probe_failure_count);

      LOG_INFO(
          "Resolution upgrade probe failed: channel={} reason={} fps={} "
          "required_fps={} delay_avg_ms={} delay_p95_ms={} qp={} "
          "target={}x{} rollback={}x{} backoff_ms={}",
          channel_name,
          probe_backlogged
              ? "encode_backlog"
              : (probe_frame_rate_too_low ? "low_frame_rate" : "high_qp"),
          probe_frame_rate, minimum_upgrade_frame_rate,
          context->average_encode_queue_delay_ms,
          context->p95_encode_queue_delay_ms,
          context->last_encoder_quality_stats.qp, failed_width, failed_height,
          rollback_width, rollback_height, backoff_ms);
      context->target_width = rollback_width;
      context->target_height = rollback_height;
      context->last_resolution_change_ms = now_ms;
      context->next_resolution_upgrade_probe_ms = now_ms + backoff_ms;
      context->encode_exceed_count = 0;
      context->encode_below_threshold_count = 0;
      context->ClearResolutionUpgradeProbe();
      context->ResetEncodedFrameRateTracking();
      context->ResetEncoderQualityTracking();
      context->ResetEncodeQueueDelayTracking();
      context->post_upgrade_protection_until_ms = 0;
      return;
    }

    if (probe_duration_ms >= kUpgradeProbeMinDurationMs) {
      LOG_INFO(
          "Resolution upgrade probe succeeded: channel={} target={}x{} "
          "fps={} required_fps={} qp={} delay_avg_ms={} delay_p95_ms={} "
          "duration_ms={} samples={}",
          channel_name, context->resolution_upgrade_probe_target_width,
          context->resolution_upgrade_probe_target_height, probe_frame_rate,
          minimum_upgrade_frame_rate, context->last_encoder_quality_stats.qp,
          context->average_encode_queue_delay_ms,
          context->p95_encode_queue_delay_ms, probe_duration_ms,
          context->resolution_upgrade_probe_sample_count);
      context->ResetResolutionUpgradeProbe();
      context->last_resolution_change_ms = now_ms;
      context->post_upgrade_protection_until_ms =
          now_ms + kPostUpgradeProtectionMs;
      context->ResetEncodeQueueDelayTracking();
    }
    return;
  }

  const bool in_post_upgrade_protection =
      now_ms < context->post_upgrade_protection_until_ms;
  const bool protection_emergency =
      sustained_critical_encode_backlog ||
      (context->encoded_frame_rate_ready &&
       context->measured_encoded_frame_rate < kCriticalFrameRate &&
       sustained_encode_backlog);
  if (in_post_upgrade_protection && !protection_emergency) {
    return;
  }

  const bool should_downgrade = startup_critical_encode_backlog ||
                                sustained_encode_backlog ||
                                (maintain_frame_rate &&
                                 (sustained_low_encoded_frame_rate ||
                                  sustained_low_capture_frame_rate ||
                                  sustained_high_pacer_rejection ||
                                  sustained_high_encode_queue_drop));

  // Upgrade
  if (!should_downgrade) {
    // Static-content hold blocks bandwidth-driven downgrades. Encode-time
    // recovery is allowed only after the configured-frame-rate and QP health
    // checks above, remains gradual, and is capped by mapped_target_* below.
    if (!context->target_width || !context->target_height ||
        !context->encoded_frame_rate_ready ||
        !VideoAdaptationPolicy::IsUpgradeFrameRateHealthy(
            media_config_.max_frame_rate,
            context->measured_encoded_frame_rate) ||
        context->encoded_frame_rate_healthy_since_ms == 0 ||
        now_ms - context->encoded_frame_rate_healthy_since_ms <
            kFrameRateHealthyDurationMs ||
        (context->normalized_qp_ewma >= 0.0f &&
         context->normalized_qp_ewma > kMaxNormalizedQpForUpgrade))
      return;
    auto [bw, bh] = base();
    if (now_ms - context->last_resolution_change_ms < upgrade_cooldown_ms)
      return;
    if (now_ms < context->next_resolution_upgrade_probe_ms) return;

    auto [nw, nh] = resolution_adapter_
                        ? resolution_adapter_->GetNextHigherResolution(
                              bw, bh, context->source_width,
                              context->source_height)
                        : std::pair<int, int>{-1, -1};
    if (nw <= 0 || nh <= 0) return;

    if (context->mapped_target_width && context->mapped_target_height &&
        nw * nh > *context->mapped_target_width *
                      *context->mapped_target_height) {
      nw = *context->mapped_target_width;
      nh = *context->mapped_target_height;
    }
    if (nw * nh <= bw * bh) return;

    LOG_INFO("Resolution upgrade probe started: channel={} {}x{} -> {}x{}",
             channel_name, bw, bh, nw, nh);
    context->resolution_upgrade_probe_active = true;
    context->resolution_upgrade_probe_base_width = bw;
    context->resolution_upgrade_probe_base_height = bh;
    context->resolution_upgrade_probe_target_width = nw;
    context->resolution_upgrade_probe_target_height = nh;
    context->resolution_upgrade_probe_sample_count = 0;
    context->resolution_upgrade_probe_started_ms = now_ms;
    context->target_width = nw;
    context->target_height = nh;
    context->encode_below_threshold_count = 0;
    context->last_resolution_change_ms = now_ms;
    context->ResetEncodedFrameRateTracking();
    context->ResetEncoderQualityTracking();
    context->ResetEncodeQueueDelayTracking();
    context->post_upgrade_protection_until_ms = 0;
    return;
  }

  // Downgrade
  auto [bw, bh] = base();
  if (context->last_resolution_change_ms > 0 &&
      now_ms - context->last_resolution_change_ms < downgrade_cooldown_ms) {
    context->encode_exceed_count = 0;
    return;
  }

  int downgrade_steps = 1;
  if (!context->encoded_frame_rate_ready) {
    downgrade_steps = startup_critical_encode_backlog ? 3 : 1;
  } else if (context->measured_encoded_frame_rate < 10 ||
             sustained_critical_encode_backlog) {
    downgrade_steps = 3;
  } else if (context->measured_encoded_frame_rate < kCriticalFrameRate) {
    downgrade_steps = 2;
  }

  int nw = bw;
  int nh = bh;
  for (int step = 0; step < downgrade_steps; ++step) {
    const auto next = resolution_adapter_
                          ? resolution_adapter_->GetNextLowerResolution(
                                nw, nh, context->source_width,
                                context->source_height)
                          : std::pair<int, int>{-1, -1};
    if (next.first <= 0 || next.second <= 0) {
      break;
    }
    nw = next.first;
    nh = next.second;
  }
  if (nw <= 0 || nh <= 0 || nw >= bw || nh >= bh) {
    context->encode_exceed_count = 0;
    return;
  }

  LOG_INFO(
      "Adaptive resolution downgrade: channel={} policy={} delay_avg_ms={} "
      "delay_p95_ms={} budget_ms={} encoded_fps={} capture_fps={} "
      "pacer_reject_percent={} encode_queue_drop_percent={} "
      "low_encoded={} low_capture={} high_pacer_reject={} "
      "high_encode_queue_drop={} qp={} steps={} {}x{} -> {}x{}",
      channel_name, maintain_frame_rate ? "frame_rate" : "balanced",
      context->average_encode_queue_delay_ms,
      context->p95_encode_queue_delay_ms, frame_budget_ms,
      context->measured_encoded_frame_rate,
      context->measured_capture_input_frame_rate,
      context->measured_pacer_rejection_percent,
      context->measured_encode_queue_drop_percent,
      sustained_low_encoded_frame_rate, sustained_low_capture_frame_rate,
      sustained_high_pacer_rejection,
      sustained_high_encode_queue_drop,
      context->last_encoder_quality_stats.qp, downgrade_steps, bw, bh, nw, nh);
  context->target_width = nw;
  context->target_height = nh;
  context->last_resolution_change_ms = now_ms;
  context->encode_exceed_count = 0;
  context->ResetResolutionUpgradeProbe();
  context->ResetEncodedFrameRateTracking();
  context->ResetEncoderQualityTracking();
  context->ResetEncodeQueueDelayTracking();
  context->post_upgrade_protection_until_ms = 0;
}

void IceTransportController::FullIntraRequest() {
  FullIntraRequestAllVideoStreams();
}

void IceTransportController::FullIntraRequestAllVideoStreams() {
  std::vector<std::string> channel_names;
  {
    std::shared_lock lock(stream_senders_mutex_);
    for (const auto& stream_sender : stream_senders_) {
      const auto& context = stream_sender.second;
      if (context && context->type == StreamType::kVideo) {
        channel_names.push_back(stream_sender.first);
      }
    }
  }

  if (channel_names.empty()) {
    // Preserve an early request until the first video sender is available.
    b_force_i_frame_ = true;
    return;
  }

  std::lock_guard<std::mutex> lock(force_i_frame_streams_mutex_);
  for (const auto& channel_name : channel_names) {
    force_i_frame_streams_.insert(channel_name);
  }
}

void IceTransportController::FullIntraRequest(uint32_t media_ssrc) {
  if (media_ssrc == 0) {
    FullIntraRequestAllVideoStreams();
    return;
  }

  std::string channel_name;
  {
    std::shared_lock lock(stream_senders_mutex_);
    for (const auto& [name, context] : stream_senders_) {
      if (context && context->type == StreamType::kVideo &&
          context->ssrc.value_or(0) == media_ssrc) {
        channel_name = name;
        break;
      }
    }
  }

  if (channel_name.empty()) {
    LOG_WARN("Ignoring FIR for unknown media SSRC {}", media_ssrc);
    return;
  }
  FullIntraRequest(channel_name);
}

int IceTransportController::OnVideoEncoded(
    const std::string& channel_name,
    const std::shared_ptr<StreamContext>& context, int queue_delay_ms,
    bool measure_encode_delay, const EncodedFrame& encoded_frame) {
  if (!is_running_.load()) {
    return -1;
  }

  if (measure_encode_delay) {
    MaybeDegradeResolutionOnEncodeTime(channel_name, queue_delay_ms,
                                       encoded_frame);
  }

  std::shared_lock lock(stream_senders_mutex_);
  if (!is_running_.load()) {
    return -1;
  }

  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || it->second != context ||
      !context->transceiver) {
    return -1;
  }

  context->last_active_time = clock_->CurrentTimeMs();
  return context->transceiver->SendVideo(encoded_frame);
}

int IceTransportController::SendAudio(const MiniRtcAudioFrame* audio_frame,
                                      const std::string& channel_name) {
  if (!is_running_.load() || !audio_frame || !audio_frame->data ||
      audio_frame->size == 0) {
    return -1;
  }

  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) {
    if (!is_running_.load()) {
      return -1;
    }
    LOG_ERROR("Failed to find stream sender [{}]", channel_name);
    return -1;
  }
  auto& context = it->second;
  if (!CheckSteamContext(channel_name, context)) {
    return -1;
  }

  int ret = context->codec->Encode(
      reinterpret_cast<const uint8_t*>(audio_frame->data), audio_frame->size,
      [this, channel_name, context](char* encoded_audio_buffer, size_t size,
                                    uint32_t samples_per_channel) -> int {
        context->last_active_time = clock_->CurrentTimeMs();
        return context->transceiver->SendAudio(
            encoded_audio_buffer, size, samples_per_channel);
      });

  return ret;
}

int IceTransportController::SendData(const char* data, size_t size,
                                     const std::string& channel_name) {
  if (!is_running_.load()) {
    return -1;
  }

  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) {
    if (!is_running_.load()) {
      return -1;
    }
    LOG_ERROR("Failed to find stream sender [{}]", channel_name);
    return -1;
  }
  auto& context = it->second;
  if (!CheckSteamContext(channel_name, context)) {
    return -1;
  }

  context->last_active_time = clock_->CurrentTimeMs();

  return context->transceiver->SendData(data, size);
}

int IceTransportController::SendReliableData(const char* data, size_t size,
                                             const std::string& channel_name) {
  if (!is_running_.load()) {
    return -1;
  }

  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) {
    if (!is_running_.load()) {
      return -1;
    }
    LOG_ERROR("Failed to find stream sender [{}]", channel_name);
    return -1;
  }
  auto& context = it->second;
  if (!CheckSteamContext(channel_name, context)) {
    return -1;
  }

  context->last_active_time = clock_->CurrentTimeMs();

  return context->transceiver->SendReliableData(data, size);
}

void IceTransportController::UpdateNetworkAvaliablity(bool network_available) {
  ice_ready_.store(network_available);
  if (!network_available) {
    dtls_ready_.store(false);
  }
  UpdateMediaTransportState();
}

bool IceTransportController::CanProbeWithoutMedia() {
  std::shared_lock lock(stream_senders_mutex_);
  for (const auto& [_, context] : stream_senders_) {
    if (context && context->type == StreamType::kVideo &&
        context->transceiver && context->transceiver->CanGeneratePadding()) {
      return true;
    }
  }
  return false;
}

void IceTransportController::UpdateMediaTransportState() {
  const bool transport_ready =
      ice_ready_.load() && (!enable_srtp_ || dtls_ready_.load());
  const bool allow_probe_without_media =
      transport_ready && CanProbeWithoutMedia();
  const bool was_transport_ready =
      media_transport_ready_.exchange(transport_ready);

  if (task_queue_pacer_ && paced_sender_) {
    auto paced_sender = paced_sender_;
    task_queue_pacer_->PostTask(
        [paced_sender, allow_probe_without_media, transport_ready,
         was_transport_ready]() mutable {
          paced_sender->SetAllowProbeWithoutMediaPacket(
              allow_probe_without_media);
          paced_sender->SetTransportReady(transport_ready);
          if (transport_ready && !was_transport_ready) {
            paced_sender->EnsureStarted();
          }
        });
  }

  if (task_queue_cc_ && controller_) {
    task_queue_cc_->PostTask(
        [this, allow_probe_without_media, transport_ready,
         was_transport_ready]() mutable {
          if (!controller_) {
            return;
          }
          controller_->SetRepeatedInitialProbing(allow_probe_without_media);
          if (transport_ready != was_transport_ready) {
            webrtc::NetworkAvailability msg;
            msg.at_time = webrtc::Timestamp::Millis(
                webrtc_clock_->TimeInMilliseconds());
            msg.network_available = transport_ready;
            PostUpdates(controller_->OnNetworkAvailability(msg));
          }
        });
  }
}

bool IceTransportController::DecryptIncomingPacket(uint8_t* buffer, int* size,
                                                   uint32_t* out_ssrc) {
  if (!buffer || !size || *size < 12) {
    return false;
  }

  uint8_t version = (buffer[0] >> 6) & 0x03;
  if (version != 2) {
    LOG_WARN("Invalid RTP version {}", version);
    return false;
  }

  uint32_t ssrc = (static_cast<uint32_t>(buffer[8]) << 24) |
                  (static_cast<uint32_t>(buffer[9]) << 16) |
                  (static_cast<uint32_t>(buffer[10]) << 8) |
                  (static_cast<uint32_t>(buffer[11]));
  if (out_ssrc) {
    *out_ssrc = ssrc;
  }

  auto it = ssrc_to_srtp_receiver_.find(ssrc);
  if (it == ssrc_to_srtp_receiver_.end() || !it->second ||
      !it->second->valid()) {
    LOG_WARN("No SRTP receiver session for SSRC {}", ssrc);
    return false;
  }

  int len = *size;
  if (it->second->unprotectRtp(buffer, &len) < 0) {
    LOG_ERROR("SRTP unprotect failed for SSRC {}", ssrc);
    return false;
  }

  *size = len;
  return true;
}

int IceTransportController::OnReceiveVideoRtpPacket(const char* data,
                                                    size_t size,
                                                    uint32_t ssrc) {
  if (ssrc_to_name_.find(ssrc) != ssrc_to_name_.end()) {
    std::string channel_name = ssrc_to_name_[ssrc];
    std::shared_lock lock(stream_receivers_mutex_);
    if (stream_receivers_.find(channel_name) != stream_receivers_.end()) {
      return stream_receivers_[channel_name]->transceiver->OnReceiveRtpPacket(
          data, size);
    }
  }
  return -1;
}

int IceTransportController::OnReceiveAudioRtpPacket(const char* data,
                                                    size_t size,
                                                    uint32_t ssrc) {
  if (ssrc_to_name_.find(ssrc) != ssrc_to_name_.end()) {
    std::string channel_name = ssrc_to_name_[ssrc];
    std::shared_lock lock(stream_receivers_mutex_);
    if (stream_receivers_.find(channel_name) != stream_receivers_.end()) {
      return stream_receivers_[channel_name]->transceiver->OnReceiveRtpPacket(
          data, size);
    }
  }

  return -1;
}

int IceTransportController::OnReceiveDataRtpPacket(const char* data,
                                                   size_t size, uint32_t ssrc) {
  if (ssrc_to_name_.find(ssrc) != ssrc_to_name_.end()) {
    std::string channel_name = ssrc_to_name_[ssrc];
    std::shared_lock lock(stream_receivers_mutex_);
    if (stream_receivers_.find(channel_name) != stream_receivers_.end()) {
      return stream_receivers_[channel_name]->transceiver->OnReceiveRtpPacket(
          data, size);
    }
  } else {
    LOG_ERROR("Can not find ssrc {}", ssrc);
  }

  return -1;
}

int IceTransportController::OnReceiveDataAckRtpPacket(
    const char* data, size_t size, uint32_t ssrc,
    const std::string& channel_name) {
  if (stream_senders_.find(channel_name) != stream_senders_.end()) {
    auto data_sender_context = stream_senders_[channel_name];
    data_sender_context->transceiver->OnReceiveRtpPacket(data, size);
  }

  return -1;
}

void IceTransportController::OnReceiveCompleteFrame(
    std::unique_ptr<ReceivedFrame> received_frame,
    const std::string& channel_name) {
  if (!task_queue_decode_) {
    LOG_ERROR("Decode task queue is nullptr");
    return;
  }

  std::weak_ptr<IceTransportController> weak_self = shared_from_this();
  task_queue_decode_->PostTask([weak_self,
                                received_frame = std::move(received_frame),
                                channel_name]() mutable {
    auto self = weak_self.lock();
    if (!self) {
      return;
    }

    std::shared_ptr<MediaCodec> codec;
    std::shared_ptr<MediaChannel> transceiver;
    OnReceiveVideo on_receive_video = nullptr;
    std::string remote_user_id;
    void* user_data = nullptr;

    {
      std::shared_lock lock(self->stream_receivers_mutex_);
      auto it = self->stream_receivers_.find(channel_name);
      if (it == self->stream_receivers_.end() || !it->second) {
        LOG_ERROR("Failed to find stream receiver [{}]", channel_name);
        return;
      }

      auto& context = it->second;
      if (!self->CheckSteamContext(channel_name, context)) {
        return;
      }

      codec = context->codec;
      transceiver = context->transceiver;
      on_receive_video = self->on_receive_video_;
      remote_user_id = self->remote_user_id_;
      user_data = self->user_data_;
    }

    int num_frame_returned = codec->Decode(
        std::move(received_frame),
        [on_receive_video, remote_user_id, channel_name,
         user_data](const DecodedFrame* decoded_frame) {
          if (!on_receive_video || !decoded_frame) {
            return;
          }

          MiniRtcVideoFrame minirtc_video_frame{};
          minirtc_video_frame.data = (const char*)decoded_frame->Buffer();
          minirtc_video_frame.width = decoded_frame->DecodedWidth();
          minirtc_video_frame.height = decoded_frame->DecodedHeight();
          minirtc_video_frame.size = decoded_frame->Size();
          minirtc_video_frame.captured_timestamp = decoded_frame->CapturedTimestamp();
          minirtc_video_frame.received_timestamp = decoded_frame->ReceivedTimestamp();
          minirtc_video_frame.decoded_timestamp = decoded_frame->DecodedTimestamp();
          minirtc_video_frame.native_frame = decoded_frame->NativeFrame();
          on_receive_video(&minirtc_video_frame, remote_user_id.data(),
                           remote_user_id.size(), channel_name.data(),
                           channel_name.size(), user_data);
        });
    if (num_frame_returned < 0 && transceiver) {
      LOG_WARN("Decoder failed for stream [{}], requesting key frame",
               channel_name);
      transceiver->RequestKeyFrame();
    }
  });
}

void IceTransportController::OnReceiveCompleteAudio(
    const char* data, size_t size, const std::string& channel_name) {
  std::shared_lock lock(stream_receivers_mutex_);
  auto it = stream_receivers_.find(channel_name);
  if (it == stream_receivers_.end() || !it->second) {
    LOG_ERROR("Failed to find stream receiver [{}]", channel_name);
    return;
  }
  auto& context = it->second;
  if (!CheckSteamContext(channel_name, context)) {
    return;
  } else {
    int num_frame_returned = context->codec->Decode(
        (uint8_t*)data, size, [this, channel_name](uint8_t* data, int size) {
          if (on_receive_audio_) {
            on_receive_audio_((const char*)data, size, remote_user_id_.data(),
                              remote_user_id_.size(), channel_name.data(),
                              channel_name.size(), user_data_);
          }
        });
  }
}

void IceTransportController::OnReceiveCompleteData(
    const char* data, size_t size, const std::string& channel_name) {
  if (on_receive_data_) {
    on_receive_data_(data, size, remote_user_id_.data(), remote_user_id_.size(),
                     channel_name.data(), channel_name.size(), user_data_);
  }
}

// std::string toHex(const std::vector<uint8_t>& vec) {
//   std::ostringstream oss;
//   for (uint8_t b : vec) {
//     oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
//         << static_cast<int>(b);
//   }
//   return oss.str();
// }

void IceTransportController::OnDtlsHandshakeDone(void* user_ptr) {
  bool local_is_client_sender;

  ice_agent_->ExportSrtpKeys(local_key_, local_salt_, remote_key_, remote_salt_,
                             offer_peer_);

  // LOG_INFO(
  //     "SRTP keys exported: local key[{}], local salt[{}], remote key[{}], "
  //     "remote salt[{}]",
  //     toHex(local_key_), toHex(local_salt_), toHex(remote_key_),
  //     toHex(remote_salt_));

  // setup SRTP senders
  SrtpEngine::Params sender_params;
  memcpy(sender_params.key, local_key_.data(), 16);
  memcpy(sender_params.salt, local_salt_.data(), 12);
  auto add_sender_session = [&](uint32_t ssrc) {
    if (ssrc == 0) {
      return;
    }
    sender_params.ssrc = ssrc;
    sender_params.receiver_any_inbound = false;
    ssrc_to_srtp_sender_[ssrc] =
        SrtpEngine::CreateSenderPtr(sender_params);
  };
  for (auto& [channel_name, context] : stream_senders_) {
    if (context) {
      add_sender_session(context->ssrc.value_or(0));
      if (context->type == StreamType::kVideo && video_rtx_enabled_) {
        add_sender_session(context->rtx_ssrc.value_or(0));
      }
    }
  }

  // setup SRTP receivers
  SrtpEngine::Params receiver_params;
  memcpy(receiver_params.key, remote_key_.data(), 16);
  memcpy(receiver_params.salt, remote_salt_.data(), 12);
  auto add_receiver_session = [&](uint32_t ssrc) {
    if (ssrc == 0) {
      return;
    }
    receiver_params.ssrc = ssrc;
    receiver_params.receiver_any_inbound = false;
    ssrc_to_srtp_receiver_[ssrc] =
        SrtpEngine::CreateReceiverPtr(receiver_params);
  };
  for (auto& [channel_name, context] : stream_receivers_) {
    if (context) {
      add_receiver_session(context->ssrc.value_or(0));
      if (context->type == StreamType::kVideo) {
        add_receiver_session(context->rtx_ssrc.value_or(0));
      }
    }
  }

  dtls_ready_.store(true);
  UpdateMediaTransportState();
}

int IceTransportController::CreateStreamCodecs(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    VideoCodecType codec_type) {
  bool video_sender_init_first_time = true;
  bool audio_sender_init_first_time = true;
  bool video_receiver_init_first_time = true;
  bool audio_receiver_init_first_time = true;

  {
    std::shared_lock lock(stream_senders_mutex_);
    for (auto& [channel_name, context] : stream_senders_) {
      if (!context) {
        LOG_ERROR("Failed to find stream sender [{}]", channel_name);
        return -1;
      }

      if (context->type == StreamType::kVideo) {
        if (!context->codec) {
          context->codec =
              VideoEncoderFactory::CreateInitializedVideoEncoder(
                  clock, media_config_, hardware_acceleration, codec_type);
          if (!context->codec) {
            LOG_ERROR("Create and initialize encoder for [{}] failed",
                      channel_name);
            return -1;
          }
          if (media_config_.video_degradation_preference ==
                  VideoDegradationPreference::MaintainFrameRate &&
              context->codec->SupportsDynamicEncodingSpeedPriority() &&
              context->codec->SetPrioritizeEncodingSpeedOverQuality(true) ==
                  0) {
            context->encoding_speed_priority_enabled = true;
          }
          context->desired_target_bitrate.reset();
          context->applied_target_bitrate.reset();
          context->bitrate_update_queued = false;
          if (video_sender_init_first_time) {
            if (!stream_senders_.empty()) {
              LOG_INFO("Use video encoder [{}]",
                       context->codec->GetEncoderName());
              video_sender_init_first_time = false;
            }
          }
        }
      } else if (context->type == StreamType::kAudio) {
        if (!context->codec) {
          context->codec = std::make_shared<AudioEncoder>(48000, 1, 480);
          if (!context->codec || 0 != context->codec->Init(media_config_)) {
            LOG_ERROR("Audio encoder [{}] init failed", channel_name);
            return -1;
          }
          if (audio_sender_init_first_time) {
            LOG_INFO("Use audio encoder [{}]",
                     context->codec->GetEncoderName());
            audio_sender_init_first_time = false;
          }
        }
      }
    }
  }

  {
    std::shared_lock lock(stream_receivers_mutex_);
    for (auto& [channel_name, context] : stream_receivers_) {
      if (!context) {
        LOG_ERROR("Failed to find stream receiver [{}]", channel_name);
        return -1;
      }
      if (!context->codec) {
        if (context->type == StreamType::kVideo) {
          context->codec = VideoDecoderFactory::CreateVideoDecoder(
              clock, hardware_acceleration, codec_type, native_video_output_);
          if (!context->codec) {
            context->codec =
                VideoDecoderFactory::CreateVideoDecoder(
                    clock, false, VideoCodecType::H264,
                    native_video_output_);
            LOG_ERROR(
                "Create decoder for [{}] failed, try to use software H.264 "
                "decoder",
                channel_name);
          }
          if (!context->codec || context->codec->Init()) {
            LOG_ERROR("Decoder [{}] init failed", channel_name);
            return -1;
          }
          if (video_receiver_init_first_time) {
            LOG_INFO("Use video decoder [{}]",
                     context->codec->GetDecoderName());
            video_receiver_init_first_time = false;
          }
        } else if (context->type == StreamType::kAudio) {
          context->codec =
              std::make_shared<AudioDecoder>(AudioDecoder(48000, 1, 480));
          if (!context->codec || 0 != context->codec->Init()) {
            LOG_ERROR("Audio decoder [{}] init failed", channel_name);
            return -1;
          }
          if (audio_receiver_init_first_time) {
            LOG_INFO("Create audio decoder [{}] finish",
                     context->codec->GetDecoderName());
            audio_receiver_init_first_time = false;
          }
        }
      }
    }
  }

  return 0;
}

int IceTransportController::CreateCodecs(std::shared_ptr<SystemClock> clock,
                                         rtp::PAYLOAD_TYPE video_pt,
                                         bool hardware_acceleration) {
  if (video_codec_inited_) {
    return 0;
  }

  hardware_acceleration_ = hardware_acceleration;

  int ret = -1;

  if (rtp::PAYLOAD_TYPE::AV1 == video_pt) {
#if defined(__APPLE__)
    ret = CreateStreamCodecs(clock, hardware_acceleration_,
                             VideoCodecType::AV1);
#else
    if (hardware_acceleration_) {
      hardware_acceleration_ = false;
      LOG_WARN("Only support software codec for AV1");
    }
    ret = CreateStreamCodecs(clock, false, VideoCodecType::AV1);
#endif
  } else if (rtp::PAYLOAD_TYPE::H264 == video_pt) {
#if defined(__APPLE__)
    ret = CreateStreamCodecs(clock, hardware_acceleration_,
                             VideoCodecType::H264);
#elif USE_CUDA && !defined(__aarch64__) && !defined(__arm__)
    bool use_hardware = false;
    if (hardware_acceleration_ && LoadNvCodecDll() == 0) {
      use_hardware = true;
    } else if (hardware_acceleration_) {
      LOG_WARN(
          "Hardware accelerated codec not available, use default software "
          "codec");
    }
    ret = CreateStreamCodecs(clock, use_hardware, VideoCodecType::H264);
#else
    ret = CreateStreamCodecs(clock, false, VideoCodecType::H264);
#endif
  }

  if (ret == 0) {
    video_codec_inited_ = true;
  }

  return ret;
}

void IceTransportController::OnSenderReport(const SenderReport& sender_report) {
  std::shared_lock lock(stream_receivers_mutex_);
  auto name_it = ssrc_to_name_.find(sender_report.SenderSsrc());
  if (name_it == ssrc_to_name_.end()) {
    LOG_WARN("Ignoring sender report for unknown SSRC {}",
             sender_report.SenderSsrc());
    return;
  }
  auto receiver_it = stream_receivers_.find(name_it->second);
  if (receiver_it != stream_receivers_.end() && receiver_it->second &&
      receiver_it->second->transceiver) {
    receiver_it->second->transceiver->OnSenderReport(sender_report);
  }
}

void IceTransportController::OnReceiverReport(
    const std::vector<RtcpReportBlock>& report_block_datas) {
  webrtc::Timestamp now = webrtc_clock_->CurrentTime();
  if (report_block_datas.empty()) return;

  // The report block source SSRC identifies the local media sender whose SR
  // produced this RTT sample. Update its RTX history directly, then share the
  // lowest valid sample with video receivers because all streams use this ICE
  // path. This gives NACK a transport RTT before it has to bootstrap from RTX.
  std::optional<int64_t> transport_rtt_ms;
  {
    std::shared_lock lock(stream_senders_mutex_);
    for (const RtcpReportBlock& report_block : report_block_datas) {
      if (!report_block.HasRtt() || report_block.LastRtt() <= 0 ||
          report_block.LastRtt() > 2000) {
        continue;
      }

      transport_rtt_ms =
          transport_rtt_ms.has_value()
              ? std::min(*transport_rtt_ms, report_block.LastRtt())
              : report_block.LastRtt();
      for (const auto& [_, context] : stream_senders_) {
        if (context && context->type == StreamType::kVideo &&
            context->transceiver &&
            context->ssrc.value_or(0) == report_block.SourceSsrc()) {
          context->transceiver->OnRttUpdate(report_block.LastRtt());
          break;
        }
      }
    }
  }

  if (transport_rtt_ms.has_value()) {
    std::shared_lock lock(stream_receivers_mutex_);
    for (const auto& [_, context] : stream_receivers_) {
      if (context && context->type == StreamType::kVideo &&
          context->transceiver) {
        context->transceiver->OnRttUpdate(*transport_rtt_ms);
      }
    }
  }

  int total_packets_lost_delta = 0;
  int total_packets_delta = 0;

  for (const RtcpReportBlock& report_block : report_block_datas) {
    auto [it, inserted] =
        last_report_blocks_.try_emplace(report_block.SourceSsrc());
    LossReport& last_loss_report = it->second;
    if (!inserted) {
      total_packets_delta += report_block.ExtendedHighSeqNum() -
                             last_loss_report.extended_highest_sequence_number;
      total_packets_lost_delta +=
          report_block.CumulativeLost() - last_loss_report.cumulative_lost;
    }
    last_loss_report.extended_highest_sequence_number =
        report_block.ExtendedHighSeqNum();
    last_loss_report.cumulative_lost = report_block.CumulativeLost();
  }
  // Can only compute delta if there has been previous blocks to compare to.
  // If not, total_packets_delta will be unchanged and there's nothing more to
  // do.
  if (!total_packets_delta) return;
  int packets_received_delta = total_packets_delta - total_packets_lost_delta;
  // To detect lost packets, at least one packet has to be received. This
  // check is needed to avoid bandwith detection update in
  // VideoSendStreamTest.SuspendBelowMinBitrate

  if (packets_received_delta < 1) {
    return;
  }
  webrtc::TransportLossReport msg;
  msg.packets_lost_delta = total_packets_lost_delta;
  msg.packets_received_delta = packets_received_delta;
  msg.receive_time = now;
  msg.start_time = last_report_block_time_;
  msg.end_time = now;

  if (task_queue_cc_) {
    task_queue_cc_->PostTask([this, msg]() mutable {
      if (controller_) {
        PostUpdates(controller_->OnTransportLossReport(msg));
      }
    });
  }

  last_report_block_time_ = now;
}

void IceTransportController::OnCongestionControlFeedback(
    const webrtc::rtcp::CongestionControlFeedback& feedback) {
  task_queue_trans_fb_->PostTask([this, feedback]() mutable {
    std::optional<webrtc::TransportPacketsFeedback> feedback_msg;
    {
      std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
      feedback_msg =
          transport_feedback_adapter_.ProcessCongestionControlFeedback(
              feedback, Timestamp::Micros(clock_->CurrentTimeUs()));
    }
    if (feedback_msg.has_value() && task_queue_cc_) {
      task_queue_cc_->PostTask([this, feedback_msg]() mutable {
        if (controller_) {
          PostUpdates(
              controller_->OnTransportPacketsFeedback(feedback_msg.value()));
        }
      });

      UpdateCongestedState();
    }
  });
}

void IceTransportController::OnReceiveNack(
    uint32_t media_ssrc,
    const std::vector<uint16_t>& nack_sequence_numbers) {
  std::shared_lock lock(stream_senders_mutex_);
  for (auto& [channel_name, context] : stream_senders_) {
    if (context && context->type == StreamType::kVideo &&
        context->transceiver && context->ssrc.value_or(0) == media_ssrc) {
      context->transceiver->OnReceiveNack(nack_sequence_numbers);
      return;
    }
  }
  LOG_WARN("Ignoring NACK for unknown media SSRC {}", media_ssrc);
}

IceTransportController::PacketFeedbackRegistration
IceTransportController::RegisterPacketForFeedback(
    const webrtc::RtpPacketToSend& packet,
    const webrtc::PacedPacketInfo& pacing_info) {
  PacketFeedbackRegistration registration;
  registration.send_time_ms = clock_->CurrentTimeMs();
  const std::optional<int64_t> transport_seq =
      packet.transport_sequence_number();
  registration.tracked = transport_seq.has_value();
  if (!registration.tracked) {
    return registration;
  }

  rtc::SentPacket sent_packet;
  sent_packet.packet_id = static_cast<int>(*transport_seq);
  sent_packet.send_time_ms = registration.send_time_ms;
  sent_packet.info.included_in_feedback = true;
  sent_packet.info.included_in_allocation = true;
  sent_packet.info.packet_size_bytes = packet.size();
  sent_packet.info.packet_type = rtc::PacketType::kData;

  {
    std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
    transport_feedback_adapter_.AddPacket(
        packet, pacing_info, /*overhead_bytes=*/0,
        webrtc::Timestamp::Millis(registration.send_time_ms));
    transport_feedback_adapter_.ProcessSentPacket(sent_packet);
  }
  return registration;
}

void IceTransportController::RollbackPacketFeedback(
    const webrtc::RtpPacketToSend& packet,
    const PacketFeedbackRegistration& registration) {
  if (!registration.tracked) {
    return;
  }
  std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
  transport_feedback_adapter_.RemovePacket(packet);
}

void IceTransportController::OnSentPacket(
    const webrtc::RtpPacketToSend& packet,
    const PacketFeedbackRegistration& registration) {
  if (!registration.tracked) {
    LOG_WARN(
        "Sent packet without transport_sequence_number (ssrc={}, "
        "rtp_seq={}), falling back to untracked allocation.",
        packet.Ssrc(), packet.SequenceNumber());

    rtc::SentPacket sent_packet;
    sent_packet.packet_id = -1;
    sent_packet.send_time_ms = registration.send_time_ms;
    sent_packet.info.included_in_feedback = false;
    sent_packet.info.included_in_allocation = true;
    sent_packet.info.packet_size_bytes = packet.size();
    sent_packet.info.packet_type = rtc::PacketType::kData;

    std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
    transport_feedback_adapter_.ProcessSentPacket(sent_packet);
  }

  if (task_queue_cc_) {
    const size_t packet_size = packet.size();
    const webrtc::Timestamp sent_time =
        webrtc::Timestamp::Millis(registration.send_time_ms);
    task_queue_cc_->PostTask([this, packet_size, sent_time]() mutable {
      if (controller_) {
        controller_->OnSentPacket(packet_size, sent_time);
      }
    });
  }
}

void IceTransportController::PostUpdates(webrtc::NetworkControlUpdate update) {
  if (update.congestion_window) {
    congestion_window_size_ = *update.congestion_window;
    UpdateCongestedState();
  }

  if (update.pacer_config && task_queue_pacer_ && paced_sender_) {
    const DataRate data_rate = update.pacer_config->data_rate();
    const DataRate pad_rate = update.pacer_config->pad_rate();
    task_queue_pacer_->PostTask([this, data_rate, pad_rate]() {
      paced_sender_->SetPacingRates(data_rate, pad_rate);
    });
  }

  if (!update.probe_cluster_configs.empty() && task_queue_pacer_ &&
      paced_sender_) {
    auto probe_cluster_configs = std::move(update.probe_cluster_configs);
    task_queue_pacer_->PostTask([this, probe_cluster_configs = std::move(
                                           probe_cluster_configs)]() mutable {
      paced_sender_->CreateProbeClusters(std::move(probe_cluster_configs));
    });
  }

  if (update.target_rate) {
    available_transport_bitrate_ =
        update.target_rate.has_value()
            ? (update.target_rate->target_rate.bps() == 0
                   ? target_bitrate_
                   : update.target_rate->target_rate.bps())
            : target_bitrate_;
    target_bitrate_ = available_transport_bitrate_;

    std::unique_lock lock(stream_senders_mutex_);
    if (!stream_senders_.empty()) {
      // Count active video and data channels separately
      int video_count = 0;
      int data_count = 0;
      for (auto& [_, context] : stream_senders_) {
        if (context->last_active_time.has_value()) {
          if (clock_->CurrentTimeMs() - context->last_active_time.value() <
              100) {
            if (context->type == StreamType::kVideo) {
              video_count++;
            } else if (context->type == StreamType::kData) {
              data_count++;
            }
          }
        }
      }

      // Allocate bandwidth: reserve 10% for all data channels
      // The rest goes to video channels
      int64_t video_bitrate_total = available_transport_bitrate_;

      if (data_count > 0) {
        // All data channels together use 10% of total bandwidth
        video_bitrate_total =
            static_cast<int64_t>(available_transport_bitrate_ * 0.9);
      }

      // Allocate bandwidth to video channels
      if (video_count > 0) {
        int sub_target_bitrate = video_bitrate_total / video_count;
        const auto& network_estimate = update.target_rate->network_estimate;
        const bool maintain_frame_rate =
            media_config_.video_degradation_preference ==
            VideoDegradationPreference::MaintainFrameRate;
        const bool allow_spatial_downgrade =
            media_config_.video_degradation_preference !=
            VideoDegradationPreference::MaintainResolution;
        const bool is_screen_content =
            media_config_.video_content_type ==
            VideoContentType::ScreenContent;
        const int64_t network_rtt_ms =
            network_estimate.round_trip_time.ms();
        const bool static_content_candidate =
            VideoAdaptationPolicy::IsStaticContentCandidate(
                is_screen_content, network_estimate.in_alr,
                network_estimate.loss_rate_ratio, network_rtt_ms);
        const bool static_content_network_critical =
            VideoAdaptationPolicy::IsStaticContentNetworkCritical(
                network_estimate.loss_rate_ratio, network_rtt_ms);
        for (auto& [channel_name, context] : stream_senders_) {
          if (!context->codec || context->type != StreamType::kVideo) {
            continue;
          }

          int source_width = context->source_width;
          int source_height = context->source_height;
          if ((source_width <= 0 || source_height <= 0) &&
              context->codec->GetResolution(&source_width, &source_height) !=
                  0) {
            continue;
          }

          const int64_t now_ms = clock_->CurrentTimeMs();
          const bool was_frozen = context->freeze_resolution;
          if (!context->static_content_candidate_initialized ||
              context->static_content_candidate != static_content_candidate) {
            context->static_content_candidate = static_content_candidate;
            context->static_content_candidate_initialized = true;
            context->static_content_candidate_since_ms = now_ms;
          }

          const int64_t candidate_duration_ms =
              now_ms - context->static_content_candidate_since_ms;
          if (!context->freeze_resolution && static_content_candidate &&
              candidate_duration_ms >=
                  VideoAdaptationPolicy::kStaticContentEnterHoldMs) {
            context->freeze_resolution = true;
          } else if (context->freeze_resolution &&
                     (!is_screen_content ||
                      static_content_network_critical)) {
            // Severe loss or RTT deterioration overrides the exit hold. Small
            // estimate movements use the normal hysteresis below so a static
            // desktop does not oscillate between native and mapped sizes.
            context->freeze_resolution = false;
          } else if (context->freeze_resolution &&
                     !static_content_candidate &&
                     candidate_duration_ms >=
                         VideoAdaptationPolicy::
                             kStaticContentExitHoldMs) {
            context->freeze_resolution = false;
          }

          // Do not apply a bandwidth downgrade while static-content entry is
          // being confirmed. This avoids a downgrade immediately followed by
          // a quality restoration and a forced key frame.
          if (!context->freeze_resolution && static_content_candidate) {
            continue;
          }

          if (context->freeze_resolution) {
            if (allow_spatial_downgrade) {
              // Static desktops produce too little traffic for bandwidth
              // estimation. In frame-rate and balanced modes, pause bandwidth
              // downgrades without replacing a previously established recovery
              // ceiling with the configured quality maximum. If no stable
              // ceiling exists yet, freeze recovery at the current resolution
              // until non-ALR bandwidth mapping becomes available again.
              const int current_width =
                  context->target_width.value_or(source_width);
              const int current_height =
                  context->target_height.value_or(source_height);
              if (!context->mapped_target_width.has_value() ||
                  !context->mapped_target_height.has_value()) {
                context->mapped_target_width = current_width;
                context->mapped_target_height = current_height;
              }

              context->pending_mapped_width.reset();
              context->pending_mapped_height.reset();
              context->mapping_stability_count = 0;
              context->pending_mapped_since_ms = 0;
              if (!was_frozen) {
                LOG_INFO(
                    "Hold static-content bandwidth downgrades: channel={} "
                    "policy={} current={}x{} recovery_ceiling={}x{}",
                    channel_name,
                    maintain_frame_rate ? "frame_rate" : "balanced",
                    current_width, current_height,
                    context->mapped_target_width.value(),
                    context->mapped_target_height.value());
              }
              continue;
            }

            // Quality-priority static content can use the selected quality
            // ceiling even when its instantaneous bitrate is low. Still respect
            // Low/Medium caps instead of unconditionally restoring native.
            int quality_width = -1;
            int quality_height = -1;
            if (resolution_adapter_->GetResolution(
                    std::numeric_limits<int>::max(), source_width,
                    source_height, &quality_width, &quality_height) != 0) {
              continue;
            }

            context->mapped_target_width = quality_width;
            context->mapped_target_height = quality_height;
            context->pending_mapped_width.reset();
            context->pending_mapped_height.reset();
            context->mapping_stability_count = 0;
            context->pending_mapped_since_ms = 0;

            const bool use_native =
                static_cast<int64_t>(quality_width) * quality_height >=
                static_cast<int64_t>(source_width) * source_height;
            const bool target_changed =
                use_native
                    ? context->target_width.has_value()
                    : (!context->target_width.has_value() ||
                       !context->target_height.has_value() ||
                       context->target_width.value() != quality_width ||
                       context->target_height.value() != quality_height);
            if (target_changed) {
              if (use_native) {
                context->target_width.reset();
                context->target_height.reset();
              } else {
                context->target_width = quality_width;
                context->target_height = quality_height;
              }
              context->last_resolution_change_ms = now_ms;
              context->ResetEncodedFrameRateTracking();
              context->ResetEncoderQualityTracking();
              context->ResetEncodeQueueDelayTracking();
              context->post_upgrade_protection_until_ms = 0;
              LOG_INFO(
                  "Static-content resolution: channel={} target={}x{} native={}x{}",
                  channel_name, use_native ? source_width : quality_width,
                  use_native ? source_height : quality_height, source_width,
                  source_height);
            } else if (!was_frozen) {
              LOG_INFO("Freeze resolution for static content: channel={}",
                       channel_name);
            }
            continue;
          }

          if (was_frozen) {
            LOG_INFO(
                "Leave static-content resolution hold: channel={} reason={}",
                channel_name,
                !is_screen_content
                    ? "content_type"
                    : (static_content_network_critical
                           ? "network_conditions"
                           : "alr_exit_hysteresis"));
            context->pending_mapped_width.reset();
            context->pending_mapped_height.reset();
            context->mapping_stability_count = 0;
            context->pending_mapped_since_ms = 0;
            // Until a new non-ALR bandwidth ceiling is stable, prevent an
            // encode-time upgrade from using the optimistic static ceiling.
            context->mapped_target_width =
                context->target_width.value_or(source_width);
            context->mapped_target_height =
                context->target_height.value_or(source_height);
          }

          int target_width = -1;
          int target_height = -1;
          if (resolution_adapter_->GetResolution(
                  sub_target_bitrate, source_width, source_height,
                  &target_width, &target_height) != 0) {
            continue;
          }

          // Controller updates may arrive only milliseconds apart. Require a
          // duration, rather than a tick count, before accepting a bandwidth
          // ceiling so startup probes and short estimate dips cannot blur the
          // desktop immediately after connection.
          if (!context->pending_mapped_width.has_value() ||
              !context->pending_mapped_height.has_value() ||
              context->pending_mapped_width.value() != target_width ||
              context->pending_mapped_height.value() != target_height) {
            context->pending_mapped_width = target_width;
            context->pending_mapped_height = target_height;
            context->mapping_stability_count = 1;
            context->pending_mapped_since_ms = now_ms;
            continue;
          }

          ++context->mapping_stability_count;
          if (!VideoAdaptationPolicy::IsBandwidthMappingStable(
                  now_ms, context->pending_mapped_since_ms)) {
            continue;
          }

          context->mapped_target_width = target_width;
          context->mapped_target_height = target_height;
          context->mapping_stability_count = 0;
          context->pending_mapped_since_ms = 0;

          const int current_width =
              context->target_width.value_or(source_width);
          const int current_height =
              context->target_height.value_or(source_height);
          const int64_t current_area =
              static_cast<int64_t>(current_width) * current_height;
          const int64_t target_area =
              static_cast<int64_t>(target_width) * target_height;
          if (VideoAdaptationPolicy::
                  ShouldApplyBandwidthResolutionDowngrade(
                      allow_spatial_downgrade, now_ms,
                      context->source_resolution_initialized_ms,
                      context->last_resolution_change_ms, current_area,
                      target_area)) {
            // Network estimates cap the spatial ladder but only move one rung
            // at a time. Frame admission and the bounded pacer protect latency
            // while the estimate is confirmed.
            int downgrade_width = target_width;
            int downgrade_height = target_height;
            const auto next_resolution =
                resolution_adapter_->GetNextLowerResolution(
                    current_width, current_height, source_width,
                    source_height);
            if (next_resolution.first > 0 && next_resolution.second > 0) {
              const int64_t next_area =
                  static_cast<int64_t>(next_resolution.first) *
                  next_resolution.second;
              if (next_area >= target_area) {
                downgrade_width = next_resolution.first;
                downgrade_height = next_resolution.second;
              }
            }
            LOG_INFO(
                "Bandwidth resolution downgrade: channel={} bitrate={} mapped={}x{} {}x{} -> {}x{}",
                channel_name, sub_target_bitrate, target_width, target_height,
                current_width, current_height, downgrade_width,
                downgrade_height);
            context->target_width = downgrade_width;
            context->target_height = downgrade_height;
            context->last_resolution_change_ms = now_ms;
            context->encode_exceed_count = 0;
            context->encode_below_threshold_count = 0;
            context->ResetResolutionUpgradeProbe();
            context->ResetEncodedFrameRateTracking();
            context->ResetEncoderQualityTracking();
            context->ResetEncodeQueueDelayTracking();
            context->post_upgrade_protection_until_ms = 0;
          }
        }
      }
    }
    UpdateControlState();
  }

  // Stream activity can change while the network estimate stays constant.
  // Publish the latest desired per-stream allocation on every controller
  // update; the encode queue coalesces stale targets and applies the newest.
  UpdateVideoBitrateAllocation();
}

void IceTransportController::UpdateVideoBitrateAllocation() {
  if (target_bitrate_ <= 0 || !is_running_.load() || !task_queue_encode_) {
    return;
  }

  struct PendingBitrateTask {
    std::string channel_name;
    std::shared_ptr<StreamContext> context;
    std::shared_ptr<MediaCodec> codec;
  };

  std::vector<PendingBitrateTask> pending_tasks;
  const int64_t now_ms = clock_->CurrentTimeMs();
  {
    std::unique_lock lock(stream_senders_mutex_);

    auto is_active = [now_ms](const std::shared_ptr<StreamContext>& context) {
      constexpr int64_t kActiveStreamTimeoutMs = 100;
      return context && context->last_active_time.has_value() &&
             now_ms - context->last_active_time.value() <
                 kActiveStreamTimeoutMs;
    };

    int active_video_count = 0;
    int active_data_count = 0;
    for (const auto& [_, context] : stream_senders_) {
      if (!is_active(context)) {
        continue;
      }
      if (context->type == StreamType::kVideo && context->codec) {
        ++active_video_count;
      } else if (context->type == StreamType::kData) {
        ++active_data_count;
      }
    }

    if (active_video_count == 0) {
      return;
    }

    int64_t video_bitrate_total = target_bitrate_;
    if (active_data_count > 0) {
      video_bitrate_total = static_cast<int64_t>(target_bitrate_ * 0.9);
    }
    const int per_video_bitrate =
        static_cast<int>(video_bitrate_total / active_video_count);

    for (const auto& [channel_name, context] : stream_senders_) {
      if (!is_active(context) || context->type != StreamType::kVideo ||
          !context->codec) {
        continue;
      }

      context->desired_target_bitrate = per_video_bitrate;
      if (context->bitrate_update_queued) {
        continue;
      }
      if (context->applied_target_bitrate == per_video_bitrate) {
        continue;
      }

      context->bitrate_update_queued = true;
      pending_tasks.push_back({channel_name, context, context->codec});
    }
  }

  for (const auto& task : pending_tasks) {
    PostVideoBitrateUpdate(task.channel_name, task.context, task.codec);
  }
}

void IceTransportController::PostVideoBitrateUpdate(
    const std::string& channel_name,
    const std::shared_ptr<StreamContext>& context,
    const std::shared_ptr<MediaCodec>& codec) {
  if (!task_queue_encode_) {
    return;
  }

  std::weak_ptr<IceTransportController> weak_self = shared_from_this();
  std::weak_ptr<StreamContext> weak_context = context;
  std::weak_ptr<MediaCodec> weak_codec = codec;
  task_queue_encode_->PostTask(
      [weak_self, weak_context, weak_codec, channel_name]() mutable {
        auto self = weak_self.lock();
        auto context = weak_context.lock();
        auto codec = weak_codec.lock();
        if (!self || !context || !codec) {
          return;
        }

        self->ApplyVideoBitrateUpdateOnEncodeQueue(channel_name, context,
                                                    codec);
      });
}

void IceTransportController::ApplyVideoBitrateUpdateOnEncodeQueue(
    const std::string& channel_name,
    const std::shared_ptr<StreamContext>& context,
    const std::shared_ptr<MediaCodec>& codec) {
  int target_bitrate = 0;
  {
    std::unique_lock lock(stream_senders_mutex_);
    auto it = stream_senders_.find(channel_name);
    if (!is_running_.load() || it == stream_senders_.end() ||
        it->second != context || context->codec != codec) {
      return;
    }

    if (!context->desired_target_bitrate.has_value()) {
      context->bitrate_update_queued = false;
      return;
    }

    target_bitrate = context->desired_target_bitrate.value();
    if (context->applied_target_bitrate == target_bitrate) {
      context->bitrate_update_queued = false;
      return;
    }
  }

  // This runs on the same queue as ForceIdr(), resolution resets, and Encode(),
  // so OpenH264 is never reconfigured concurrently with frame processing.
  const int result = codec->SetTargetBitrate(target_bitrate);
  bool post_followup = false;
  {
    std::unique_lock lock(stream_senders_mutex_);
    auto it = stream_senders_.find(channel_name);
    if (!is_running_.load() || it == stream_senders_.end() ||
        it->second != context || context->codec != codec) {
      return;
    }

    if (result == 0) {
      context->applied_target_bitrate = target_bitrate;
    }

    if (context->desired_target_bitrate.has_value() &&
        context->desired_target_bitrate.value() != target_bitrate) {
      // Keep the in-flight flag set. Reposting at the tail gives frame tasks a
      // chance to run while still converging to the newest desired bitrate.
      post_followup = true;
    } else {
      context->bitrate_update_queued = false;
    }
  }

  if (result != 0) {
    LOG_WARN("Failed to apply video target bitrate: channel={} bitrate={}",
             channel_name, target_bitrate);
  }

  if (post_followup) {
    PostVideoBitrateUpdate(channel_name, context, codec);
  }
}

void IceTransportController::UpdateControlState() {
  if (controller_) {
  }
}

void IceTransportController::UpdateCongestedState() {
  if (auto update = GetCongestedStateUpdate()) {
    is_congested_ = update.value();
    if (task_queue_pacer_ && paced_sender_) {
      task_queue_pacer_->PostTask([this, update]() mutable {
        paced_sender_->SetCongested(update.value());
      });
    }
  }
}

std::optional<bool> IceTransportController::GetCongestedStateUpdate() const {
  webrtc::DataSize outstanding_data;
  {
    std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
    outstanding_data = transport_feedback_adapter_.GetOutstandingData();
  }
  bool congested = outstanding_data >= congestion_window_size_;
  if (congested != is_congested_) return congested;
  return std::nullopt;
}

bool IceTransportController::Process() {
  if (!is_running_.load()) {
    return false;
  }

  if (task_queue_cc_ && controller_) {
    task_queue_cc_->PostTask([this]() mutable {
      webrtc::ProcessInterval msg;
      msg.at_time = Timestamp::Millis(webrtc_clock_->TimeInMilliseconds());
      PostUpdates(controller_->OnProcessInterval(msg));
    });
  }

  return true;
}
}  // namespace minirtc
