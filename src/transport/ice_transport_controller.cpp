#include "ice_transport_controller.h"

#include <algorithm>
#include <cmath>
#include <future>
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
// At the normal 2.5x pacing rate this occupies at most 72 ms of the queue,
// leaving room for RTP overhead and recovery within the 350 ms deadline.
constexpr int64_t kDesktopKeyFrameBudgetMs = 180;
// Only the initial keyframe may borrow extra queue time (at most 240 ms at
// normal 2.5x pacing). Admission still pauses following frames as it drains.
constexpr int64_t kStartupKeyFrameBudgetMs = 600;
constexpr size_t kStartupKeyFrameMaxBytes = 128 * 1024;
constexpr int64_t kStartupKeyFrameTimeoutMs = 1500;

bool UsesBoundedVideoQueue(const MediaCodecConfig& config) {
  return config.video_content_type == VideoContentType::ScreenContent &&
         config.video_degradation_preference !=
             VideoDegradationPreference::MaintainResolution;
}

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
                                    std::optional<uint8_t>
                                        video_abs_send_time_ext_id,
                                    std::optional<uint8_t>
                                        video_abs_recv_time_ext_id,
                                    std::optional<uint8_t>
                                        audio_abs_send_time_ext_id,
                                    std::optional<uint8_t>
                                        audio_abs_recv_time_ext_id,
                                    OnReceiveVideo on_receive_video,
                                    OnReceiveAudio on_receive_audio,
                                    OnReceiveData on_receive_data,
                                    void* user_data) {
  offer_peer_ = offer_peer;
  remote_user_id_ = remote_user_id;
  video_rtx_enabled_ = video_rtx_enabled;
  video_abs_send_time_ext_id_ = video_abs_send_time_ext_id;
  video_abs_recv_time_ext_id_ = video_abs_recv_time_ext_id;
  audio_abs_send_time_ext_id_ = audio_abs_send_time_ext_id;
  audio_abs_recv_time_ext_id_ = audio_abs_recv_time_ext_id;
  on_receive_video_ = on_receive_video;
  on_receive_audio_ = on_receive_audio;
  on_receive_data_ = on_receive_data;
  user_data_ = user_data;
  native_video_output_ = native_video_output;

  if (video_fec_enabled_ && !video_codec_inited_) {
    target_bitrate_ = media_config_.init_bitrate;
    FecProtectionConfig initial;
    initial.source_ratio = fec_mode_.load() == FecMode::kOff ? 0 :
                           fec_mode_.load() == FecMode::kAdaptive ? 0.10 : 0.25;
    media_config_.init_bitrate = static_cast<int>(
        AllocateFecBudget(initial, target_bitrate_, 0.94).media_bitrate_bps);
    video_transport_bitrate_bps_.store(media_config_.init_bitrate);
  }
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
  const int relay_path_state =
      relay_path_state_.load(std::memory_order_acquire);
  if (relay_path_state >= 0) {
    controller_->SetRelayPath(
        relay_path_state == 1,
        webrtc::Timestamp::Millis(webrtc_clock_->TimeInMilliseconds()));
  }
  paced_sender_ = std::make_shared<PacedSender>(
      ice_agent_, webrtc_clock_, task_queue_pacer_,
      UsesBoundedVideoQueue(media_config_));
  paced_sender_->SetPacingRates(DataRate::BitsPerSec(300000), DataRate::Zero());
  paced_sender_->SetSendBurstInterval(TimeDelta::Millis(40));
  paced_sender_->SetQueueTimeLimit(TimeDelta::Millis(
      media_config_.video_content_type == VideoContentType::ScreenContent
          ? kDesktopPacerQueueLimitMs
          : 2000));
  paced_sender_->SetAllowProbeWithoutMediaPacket(false);
  std::weak_ptr<IceTransportController> weak_this = shared_from_this();
  // The pacer serializes this callback, and Send consumes its buffer before
  // returning, so the callback can retain and reuse its SRTP scratch space.
  paced_sender_->SetOnSentPacketFunc(
      [weak_this, protected_packet = std::vector<uint8_t>()](
          std::unique_ptr<webrtc::RtpPacketToSend> packet,
          const webrtc::PacedPacketInfo& pacing_info) mutable {
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

          std::optional<uint8_t> abs_send_time_ext_id =
              self->video_abs_send_time_ext_id_;
          if (packet->packet_type() == webrtc::RtpPacketMediaType::kAudio) {
            abs_send_time_ext_id = self->audio_abs_send_time_ext_id_;
          }
          if (abs_send_time_ext_id.has_value()) {
            const uint64_t send_time_ntp = self->clock_->CurrentNtpTime();
            if (!packet->UpdateAbsoluteSendTimestamp(
                    *abs_send_time_ext_id,
                    SystemClock::NtpToAbsoluteSendTime(send_time_ntp))) {
              LOG_ERROR("Failed updating RTP Absolute Send Time extension");
              notify_send_failure();
              return;
            }
          }

          const char* send_buffer = nullptr;
          size_t send_size = 0;

          if (self->enable_srtp_) {
            int len = packet->Size();

            const size_t protected_size = packet->Size() + 16;
            if (protected_packet.size() < protected_size) {
              protected_packet.resize(protected_size);
            }
            memcpy(protected_packet.data(), packet->Buffer().data(), len);

            auto srtp_it =
                self->ssrc_to_srtp_sender_.find(packet->Ssrc());
            if (srtp_it == self->ssrc_to_srtp_sender_.end() ||
                !srtp_it->second || !srtp_it->second->valid()) {
              LOG_ERROR("No SRTP sender session for SSRC {}", packet->Ssrc());
              notify_send_failure();
              return;
            }
            const int result =
                srtp_it->second->protectRtp(protected_packet.data(), &len);
            if (result < 0) {
              LOG_ERROR("SRTP protect failed for stream [{}]: {} ({})",
                        packet->Ssrc(),
                        SrtpEngine::ErrToStr(
                            static_cast<srtp_err_status_t>(-result)),
                        -result);
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
              self->RegisterPacketForFeedback(*packet, pacing_info, send_size);
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
          context->transceiver->SetFecEnabled(video_fec_enabled_);
          context->transceiver->SetAbsoluteSendTimeExtensionId(
              video_abs_send_time_ext_id_);
          std::static_pointer_cast<VideoChannelSend>(context->transceiver)
              ->Initialize(video_codec_payload_type, paced_sender_,
                           video_rtx_enabled_);
          // No repair generation before the first connection allocation.
          FecProtectionConfig initial;
          initial.source_ratio = 0;
          initial.fec_bitrate_bps = 0;
          context->transceiver->SetFecProtection(initial);
        } else if (context->type == StreamType::kAudio) {
          context->transceiver->SetAbsoluteSendTimeExtensionId(
              audio_abs_send_time_ext_id_);
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
          context->transceiver->SetFecEnabled(video_fec_enabled_);
          context->transceiver->SetAbsoluteSendTimeExtensionId(
              video_abs_recv_time_ext_id_);
          context->transceiver->Initialize(video_codec_payload_type);
        } else if (context->type == StreamType::kAudio) {
          context->transceiver->SetAbsoluteSendTimeExtensionId(
              audio_abs_recv_time_ext_id_);
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
        channel_name, clock_, ice_agent_, ice_io_statistics_);
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
        channel_name, ssrc, clock_, ice_agent_, ice_io_statistics_,
        [this, weak_self, channel_name](const char* data, size_t size,
                                        uint16_t sequence, uint32_t timestamp) {
          if (auto self = weak_self.lock()) {
            OnReceiveCompleteAudio(data, size, channel_name, sequence,
                                   timestamp);
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

  std::unique_lock lock(stream_senders_mutex_);
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

  context->last_capture_time = clock_->CurrentTimeMs();
  // Encoder minimum rates must not defeat an exhausted transport allocation.
  if (context->desired_target_bitrate && *context->desired_target_bitrate <= 0)
    return 0;

  // ICE can become usable before DTLS has installed the SRTP sessions. Do not
  // copy, encode or queue video while the pacer is unable to send it. Keep
  // pending key-frame requests intact for the first frame after readiness.
  if (!media_transport_ready_.load()) {
    return 0;
  }

  if (task_queue_encode_) {
    if (!video_frame_cadences_[channel_name].Accept(
            clock_->CurrentTimeUs(), media_config_.max_frame_rate))
      return 0;
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
      if (context->startup_keyframe_pending) {
        context->FinishStartupKeyframe();
      }
      context->source_width = raw_frame.Width();
      context->source_height = raw_frame.Height();
      context->source_resolution_initialized_ms = clock_->CurrentTimeMs();
      context->initial_resolution_recovery = true;
      context->native_resolution_probe_attempted = false;
      context->keyframe_limited_upgrade = false;
      context->keyframe_failed_pixels = 0;
      context->keyframe_failed_bytes = 0;
      context->keyframe_failure_ms = 0;
      context->keyframe_same_resolution_retry = false;
      context->keyframe_resolution_recovery.reset();
      context->ResetPendingBandwidthMapping();
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

    if (UsesBoundedVideoQueue(media_config_)) {
      int video_count = 0;
      const int64_t now_ms = clock_->CurrentTimeMs();
      for (const auto& [_, sender] : stream_senders_) {
        if (!sender) continue;
        if (sender->type == StreamType::kVideo && sender->codec &&
            (sender == context ||
             (sender->last_capture_time &&
              now_ms - *sender->last_capture_time < 100))) {
          ++video_count;
        }
      }
      const int64_t transport_bitrate = video_transport_bitrate_bps_.load();
      const int64_t video_bitrate = transport_bitrate;
      // Count captured streams even before their first encoded output, but do
      // not reserve startup bandwidth for configured, idle displays.
      const int64_t stream_bitrate = video_bitrate / std::max(1, video_count);
      const int64_t frame_bitrate = std::max<int64_t>(
          1, std::min<int64_t>(
                 stream_bitrate,
                 context->desired_target_bitrate.value_or(stream_bitrate)));
      const bool startup_keyframe =
          PrepareStartupKeyframe(context, frame_bitrate, &force_i_frame);

      if (!startup_keyframe && force_i_frame && resolution_adapter_) {
        int width = 0;
        int height = 0;
        if (resolution_adapter_->GetResolution(
                static_cast<int>(frame_bitrate), context->source_width,
                context->source_height, &width, &height) == 0 &&
            width < context->target_width.value_or(context->source_width) &&
            height < context->target_height.value_or(context->source_height)) {
          NoteKeyframeBudgetFailure(context, width, height);
          // This limits the next keyframe, not the confirmed network ceiling.
          context->last_resolution_change_ms = clock_->CurrentTimeMs();
          context->ResetEncodedFrameRateTracking();
          context->ResetEncoderQualityTracking();
          context->ResetEncodeQueueDelayTracking();
          context->post_upgrade_protection_until_ms = 0;
        }
      }
    }

    std::weak_ptr<IceTransportController> weak_self = shared_from_this();
    std::weak_ptr<StreamContext> weak_context = context;
    std::shared_ptr<TaskQueueLockFree> encode_queue = task_queue_encode_;

    const uint64_t settings_generation = context->video_settings_generation;
    const int target_width = context->target_width.value_or(0);
    const int target_height = context->target_height.value_or(0);
    auto post_encode = [weak_self, weak_context, encode_queue, channel_name,
                        force_i_frame, target_width, target_height,
                        settings_generation](RawFrame&& frame) mutable {
      encode_queue->PostTask([weak_self, weak_context, encode_queue,
                              channel_name, force_i_frame, target_width,
                              target_height, settings_generation,
                              frame = std::move(frame)]() mutable {
        auto self = weak_self.lock();
        auto context = weak_context.lock();
        if (!self || !context || !self->is_running_.load()) {
          return;
        }

        {
          std::shared_lock lock(self->stream_senders_mutex_);
          if (context->video_settings_generation != settings_generation) return;
        }
        // The transport may have become unavailable after frame admission.
        // Restore the request consumed above so the next frame can resync.
        if (!self->media_transport_ready_.load()) {
          self->FullIntraRequest(channel_name);
          return;
        }

        if (!context->codec) {
          return;
        }
        const bool needs_scaling =
            target_width > 0 && target_height > 0 &&
            target_width < frame.Width() && target_height < frame.Height();
        if (const auto* native_frame = frame.NativeFrame();
            native_frame &&
            (needs_scaling ||
             !context->codec->SupportsNativeFrameInput(native_frame->type))) {
          if (!frame.MaterializeNativeFrame()) {
            LOG_ERROR("Failed to materialize native video frame for stream [{}]",
                      channel_name);
            return;
          }
        }
        if (needs_scaling) {
          RawFrame scaled_frame(static_cast<size_t>(target_width) *
                                    target_height * 3 / 2);
          scaled_frame.SetCapturedTimestamp(frame.CapturedTimestamp());
          if (self->resolution_adapter_->ResolutionDowngrade(
                  frame, target_width, target_height, scaled_frame) != 0) {
            LOG_ERROR("Failed to scale video frame from [{}x{}] to [{}x{}]",
                      frame.Width(), frame.Height(), target_width,
                      target_height);
            return;
          }
          frame = std::move(scaled_frame);
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
             settings_generation, is_first_callback = true](
                const EncodedFrame& encoded_frame) mutable -> int {
              auto self = weak_self.lock();
              auto context = weak_context.lock();
              if (!self || !context || !self->is_running_.load()) {
                return -1;
              }

              {
                std::shared_lock lock(self->stream_senders_mutex_);
                if (context->video_settings_generation != settings_generation)
                  return 0;
              }
              const bool measure_encode_delay = is_first_callback;
              is_first_callback = false;
              return self->OnVideoEncoded(
                  channel_name, context, static_cast<int>(queue_delay_ms),
                  measure_encode_delay, encoded_frame, settings_generation);
            });
      });
    };

    post_encode(std::move(raw_frame));
  }

  return 0;
}

bool IceTransportController::PrepareStartupKeyframe(
    const std::shared_ptr<StreamContext>& context, int64_t frame_bitrate,
    bool* force_keyframe) {
  // Called under stream_senders_mutex_, after frame admission and transport
  // readiness. Stream lifetime, rather than source size changes, owns this
  // one startup opportunity.
  frame_bitrate = std::max<int64_t>(1, frame_bitrate);
  context->regular_keyframe_size_budget_bytes =
      std::max<int64_t>(MINIRTC_MAX_PAYLOAD_SIZE,
                        frame_bitrate * kDesktopKeyFrameBudgetMs / 8000);
  context->keyframe_size_budget_bytes =
      context->regular_keyframe_size_budget_bytes;
  const int64_t now_ms = clock_->CurrentTimeMs();
  if (!context->startup_resolution_attempted) {
    context->startup_resolution_attempted = true;
    int width = 0, height = 0;
    if (media_config_.video_content_type == VideoContentType::ScreenContent &&
        media_config_.video_degradation_preference ==
            VideoDegradationPreference::Balanced &&
        !context->resolution_upgrade_network_blocked && resolution_adapter_ &&
        resolution_adapter_->GetStartupResolution(context->source_width,
                                                  context->source_height,
                                                  &width, &height) == 0) {
      context->target_width = width;
      context->target_height = height;
      context->startup_keyframe_pending = true;
      context->startup_keyframe_started_ms = now_ms;
      context->awaiting_budget_keyframe = true;
      *force_keyframe = true;
    }
  }
  if (!context->startup_keyframe_pending) return false;
  if (context->resolution_upgrade_network_blocked ||
      now_ms - context->startup_keyframe_started_ms >=
          kStartupKeyFrameTimeoutMs) {
    context->FinishStartupKeyframe();
    *force_keyframe = true;
    return false;
  }
  context->keyframe_size_budget_bytes = std::max<size_t>(
      context->regular_keyframe_size_budget_bytes,
      std::min<int64_t>(kStartupKeyFrameMaxBytes,
                        frame_bitrate * kStartupKeyFrameBudgetMs / 8000));
  return true;
}

void IceTransportController::NoteKeyframeBudgetFailure(
    const std::shared_ptr<StreamContext>& context, int retry_width,
    int retry_height) {
  // The caller holds stream_senders_mutex_. A budget retry may resize several
  // times, but it is one failed trial and must retain the original backoff.
  const bool probing = context->resolution_upgrade_probe_active;
  const int failed_width =
      context->target_width.value_or(context->source_width);
  const int failed_height =
      context->target_height.value_or(context->source_height);
  context->target_width = std::min(retry_width, failed_width);
  context->target_height = std::min(retry_height, failed_height);
  if (probing) {
    context->target_width = std::min(
        *context->target_width, context->resolution_upgrade_probe_base_width);
    context->target_height = std::min(
        *context->target_height, context->resolution_upgrade_probe_base_height);
  }
  if ((!context->awaiting_budget_keyframe ||
       context->keyframe_same_resolution_retry) &&
      (probing || context->mapped_target_width.has_value())) {
    context->BackoffResolutionUpgrade(clock_->CurrentTimeMs());
  } else {
    context->ClearResolutionUpgradeProbe();
  }
  context->keyframe_limited_upgrade = true;
  context->keyframe_same_resolution_retry = false;
  context->awaiting_budget_keyframe = true;
}

void IceTransportController::MaybeDegradeResolutionOnEncodeTime(
    const std::string& channel_name, int queue_delay_ms,
    const EncodedFrame& encoded_frame, uint64_t settings_generation) {
  std::unique_lock lock(stream_senders_mutex_);
  const bool maintain_frame_rate =
      media_config_.video_degradation_preference ==
      VideoDegradationPreference::MaintainFrameRate;
  const bool maintain_resolution =
      media_config_.video_degradation_preference ==
      VideoDegradationPreference::MaintainResolution;
  const bool balanced = media_config_.video_degradation_preference ==
                        VideoDegradationPreference::Balanced;
  const int minimum_frame_rate = VideoAdaptationPolicy::MinimumFrameRate(
      media_config_.max_frame_rate, balanced);
  const int minimum_upgrade_frame_rate =
      VideoAdaptationPolicy::UpgradeFrameRate(media_config_.max_frame_rate,
                                              balanced);
  // Balanced output may intentionally coalesce 60 fps input into 30 fps.
  // Judge queue pressure against that output budget, while retaining the
  // separate absolute critical-delay limit below.
  const int frame_budget_ms =
      std::max(1, 1000 / std::max(1, balanced ? minimum_upgrade_frame_rate
                                              : media_config_.max_frame_rate));
  const int delay_threshold_ms =
      maintain_frame_rate
          ? std::max(2, frame_budget_ms / 3)
          : (balanced ? std::max(4, frame_budget_ms / 2) : 8);
  const bool balanced_screen_content =
      balanced &&
      media_config_.video_content_type == VideoContentType::ScreenContent;
  constexpr int kCriticalFrameRate = 20;
  constexpr int kUpgradeProbeInputWaitMs = 5000;
  constexpr int kQueueDelayWindowMs = 1000;
  constexpr int kQueueBacklogSustainMs = 500;
  constexpr int kCriticalQueueBacklogSustainMs = 250;
  constexpr int kCriticalQueueDelayMs = 100;
  constexpr float kMaxNormalizedQpForUpgrade = 0.60f;
  constexpr float kQpEwmaAlpha = 0.10f;
  const int downgrade_cooldown_ms =
      maintain_frame_rate ? 750 : (balanced ? 1500 : 3000);

  if (!is_running_.load()) return;
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) return;
  std::shared_ptr<StreamContext> context = it->second;
  if (context->video_settings_generation != settings_generation) return;
  const int64_t now_ms = clock_->CurrentTimeMs();
  if (context->keyframe_resolution_recovery &&
      now_ms >= context->keyframe_resolution_recovery->expires_ms) {
    context->keyframe_resolution_recovery.reset();
  }
  bool keyframe_recovery = false;
  if (balanced_screen_content && context->keyframe_resolution_recovery &&
      resolution_adapter_) {
    const auto& recovery = *context->keyframe_resolution_recovery;
    int budget_width = 0, budget_height = 0;
    const int bitrate = static_cast<int>(std::min<int64_t>(
        std::numeric_limits<int>::max(),
        static_cast<int64_t>(context->keyframe_size_budget_bytes) * 8000 /
            kDesktopKeyFrameBudgetMs));
    keyframe_recovery =
        recovery.projected_bytes > 0 &&
        recovery.projected_bytes <= context->keyframe_size_budget_bytes &&
        context->mapped_target_width.value_or(0) >= recovery.width &&
        context->mapped_target_height.value_or(0) >= recovery.height &&
        static_cast<int64_t>(context->target_width.value_or(0)) *
                context->target_height.value_or(0) <
            static_cast<int64_t>(recovery.width) * recovery.height &&
        resolution_adapter_->GetResolution(
            bitrate, context->source_width, context->source_height,
            &budget_width, &budget_height) == 0 &&
        budget_width >= recovery.width && budget_height >= recovery.height;
  }
  const bool initial_fast_recovery =
      balanced_screen_content && context->initial_resolution_recovery &&
      now_ms - context->source_resolution_initialized_ms <= 30000;
  // Native output skips scaling. Give it one direct trial even if bandwidth
  // confirmation arrives after the initial fast window or a smaller trial
  // failed. A prior native attempt, failure backoff, quality and send budget
  // still constrain this opportunity.
  bool late_native_recovery = false;
  if (balanced_screen_content && !initial_fast_recovery &&
      !context->native_resolution_probe_attempted && resolution_adapter_ &&
      context->source_width > 0 && context->source_height > 0 &&
      context->mapped_target_width.value_or(0) >= context->source_width &&
      context->mapped_target_height.value_or(0) >= context->source_height &&
      (context->target_width != context->source_width ||
       context->target_height != context->source_height)) {
    int admission_bitrate = std::numeric_limits<int>::max();
    if (context->keyframe_limited_upgrade &&
        context->keyframe_size_budget_bytes > 0) {
      admission_bitrate = static_cast<int>(std::min<int64_t>(
          admission_bitrate,
          static_cast<int64_t>(context->keyframe_size_budget_bytes) * 8000 /
              kDesktopKeyFrameBudgetMs));
    }
    int width = 0, height = 0;
    late_native_recovery = resolution_adapter_->GetResolution(
                               admission_bitrate, context->source_width,
                               context->source_height, &width, &height) == 0 &&
                           width == context->source_width &&
                           height == context->source_height;
  }
  const bool fast_recovery = initial_fast_recovery || late_native_recovery ||
                             keyframe_recovery ||
                             context->resolution_upgrade_probe_fast;
  const int kFrameRateWindowMs = fast_recovery ? 500 : 1000;
  const int kFrameRateHealthyDurationMs = fast_recovery ? 500 : 2000;
  const int kUpgradeProbeMinDurationMs =
      context->resolution_upgrade_probe_fast ? 1000 : 2000;
  // A completed trial already observes output at the new size.
  const int kPostUpgradeProtectionMs =
      fast_recovery ? 200 : (balanced_screen_content ? 1000 : 2000);
  const int upgrade_cooldown_ms =
      fast_recovery ? 200 : (balanced_screen_content ? 1000 : 3000);
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
    // This frame is the interval boundary; count subsequent arrivals only.
    context->encoded_frame_rate_window_frame_count = 0;
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
      if (context->measured_encoded_frame_rate >= minimum_upgrade_frame_rate) {
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
          context->encoded_frame_rate_valid_window_count, balanced);
  const auto frame_health = VideoAdaptationPolicy::EvaluateFrameHealth(
      media_config_.max_frame_rate, encoded_frame_rate_persistently_low,
      context->frame_admission_metrics_ready,
      context->measured_capture_input_frame_rate,
      context->frame_admission_capture_samples,
      context->frame_admission_pacer_rejected_samples,
      context->frame_admission_encode_queue_dropped_samples, balanced);
  const bool capture_limited = VideoAdaptationPolicy::IsCaptureLimited(
      media_config_.max_frame_rate, context->frame_admission_metrics_ready,
      context->measured_capture_input_frame_rate,
      context->measured_encoded_frame_rate, balanced);
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
            context->encoded_frame_rate_valid_window_count, balanced);
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
  if (context->resolution_upgrade_probe_active &&
      static_cast<int>(encoded_frame.EncodedWidth()) ==
          context->resolution_upgrade_probe_target_width &&
      static_cast<int>(encoded_frame.EncodedHeight()) ==
          context->resolution_upgrade_probe_target_height) {
    if (context->resolution_upgrade_probe_measurement_started_ms == 0) {
      // Measure capture and output over the same interval, starting after the
      // encoder has applied the new size and produced its first key frame.
      context->resolution_upgrade_probe_measurement_started_ms = now_ms;
      context->resolution_upgrade_probe_capture_start = capture_input_total;
    } else {
      ++context->resolution_upgrade_probe_sample_count;
    }
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
      context->measured_encoded_frame_rate >= minimum_upgrade_frame_rate &&
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
    const bool measurement_started =
        context->resolution_upgrade_probe_measurement_started_ms > 0;
    const int64_t probe_duration_ms =
        measurement_started
            ? now_ms - context->resolution_upgrade_probe_measurement_started_ms
            : 0;
    const bool probe_start_timed_out =
        !measurement_started &&
        now_ms - context->resolution_upgrade_probe_started_ms >=
            kUpgradeProbeInputWaitMs;
    const uint64_t probe_capture_samples =
        measurement_started
            ? capture_input_total -
                  context->resolution_upgrade_probe_capture_start
            : 0;
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
        sustained_severe_encode_backlog || sustained_critical_encode_backlog ||
        // A one-second trial finishes before the ordinary pressure sustain
        // timer; inspect its full queue window before accepting the new size.
        (context->resolution_upgrade_probe_fast &&
         probe_duration_ms >= kUpgradeProbeMinDurationMs &&
         severe_queue_pressure) ||
        (startup_critical_encode_backlog &&
         probe_duration_ms >= downgrade_cooldown_ms);
    const bool insufficient_capture =
        balanced_screen_content &&
        probe_capture_samples <
            VideoAdaptationPolicy::kMinimumPacerAdmissionSamples;
    const bool probe_frame_rate_too_low =
        probe_duration_ms >= kUpgradeProbeMinDurationMs &&
        !insufficient_capture && probe_frame_rate < minimum_upgrade_frame_rate;
    const bool probe_qp_too_high =
        probe_duration_ms >= kUpgradeProbeMinDurationMs &&
        context->normalized_qp_ewma >= 0.0f &&
        context->normalized_qp_ewma > kMaxNormalizedQpForUpgrade;

    if (probe_start_timed_out || probe_backlogged || probe_frame_rate_too_low ||
        probe_qp_too_high) {
      const int rollback_width =
          context->resolution_upgrade_probe_base_width;
      const int rollback_height =
          context->resolution_upgrade_probe_base_height;
      const int failed_width =
          context->resolution_upgrade_probe_target_width;
      const int failed_height =
          context->resolution_upgrade_probe_target_height;
      const int backoff_ms = context->BackoffResolutionUpgrade(now_ms);

      LOG_INFO(
          "Resolution upgrade probe failed: channel={} reason={} fps={} "
          "required_fps={} delay_avg_ms={} delay_p95_ms={} qp={} "
          "target={}x{} rollback={}x{} backoff_ms={}",
          channel_name,
          probe_start_timed_out
              ? "resolution_not_applied"
              : (probe_backlogged
                     ? "encode_backlog"
                     : (probe_frame_rate_too_low ? "low_frame_rate" : "high_qp")),
          probe_frame_rate, minimum_upgrade_frame_rate,
          context->average_encode_queue_delay_ms,
          context->p95_encode_queue_delay_ms,
          context->last_encoder_quality_stats.qp, failed_width, failed_height,
          rollback_width, rollback_height, backoff_ms);
      context->target_width = rollback_width;
      context->target_height = rollback_height;
      context->last_resolution_change_ms = now_ms;
      context->encode_exceed_count = 0;
      context->encode_below_threshold_count = 0;
      context->ResetEncodedFrameRateTracking();
      context->ResetEncoderQualityTracking();
      context->ResetEncodeQueueDelayTracking();
      context->post_upgrade_protection_until_ms = 0;
      return;
    }

    if (probe_duration_ms >= kUpgradeProbeMinDurationMs &&
        insufficient_capture) {
      if (probe_duration_ms < kUpgradeProbeInputWaitMs) return;
      // Sparse input cannot establish encoder capacity. Keep the trial size,
      // then require fresh frame health before another step; do not attribute
      // the missing input to the encoder or increase its failure backoff.
      context->ClearResolutionUpgradeProbe();
      context->next_resolution_upgrade_probe_ms = now_ms + upgrade_cooldown_ms;
      context->last_resolution_change_ms = now_ms;
      context->ResetEncodedFrameRateTracking();
      context->ResetEncodeQueueDelayTracking();
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
      if (context->target_width == context->source_width &&
          context->target_height == context->source_height) {
        context->initial_resolution_recovery = false;
      }
      context->ResetResolutionUpgradeProbe();
      context->last_resolution_change_ms = now_ms;
      context->post_upgrade_protection_until_ms =
          now_ms + kPostUpgradeProtectionMs;
      // Keep the successful trial's queue observations for the next recovery
      // decision. Clearing them adds another warmup interval at every rung.
      if (!balanced_screen_content) {
        context->ResetEncodeQueueDelayTracking();
      }
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
                                 VideoAdaptationPolicy::ShouldDowngradeForFrameHealth(
                                     sustained_low_encoded_frame_rate,
                                     capture_limited,
                                     sustained_high_pacer_rejection,
                                     sustained_high_encode_queue_drop));

  // Upgrade
  if (!should_downgrade) {
    if (!context->target_width || !context->target_height) return;
    if (!context->mapped_target_width || !context->mapped_target_height) {
      return;
    }
    if (context->resolution_upgrade_network_blocked) {
      return;
    }
    auto [bw, bh] = base();
    auto [nw, nh] = resolution_adapter_
                        ? resolution_adapter_->GetNextHigherResolution(
                              bw, bh, context->source_width,
                              context->source_height)
                        : std::pair<int, int>{-1, -1};
    if (nw <= 0 || nh <= 0 || nw * nh <= bw * bh) {
      return;
    }

    constexpr int64_t kFastRecoveryMaxPixels = 1280 * 720;
    const bool can_skip_small_rung =
        balanced_screen_content &&
        static_cast<int64_t>(bw) * bh < kFastRecoveryMaxPixels &&
        context->resolution_upgrade_probe_failure_count == 0 &&
        context->frame_admission_metrics_ready &&
        context->frame_admission_capture_samples >=
            VideoAdaptationPolicy::kMinimumPacerAdmissionSamples &&
        context->frame_admission_pacer_rejected_samples == 0 &&
        context->frame_admission_encode_queue_dropped_samples * 100 <
            context->frame_admission_capture_samples *
                VideoAdaptationPolicy::kEncodeQueueDropThresholdPercent &&
        context->encode_queue_delay_window_ready &&
        context->average_encode_queue_delay_ms <
            std::max(1, frame_budget_ms / 4) &&
        context->p95_encode_queue_delay_ms < frame_budget_ms &&
        context->normalized_qp_ewma >= 0.0f &&
        context->normalized_qp_ewma <= 0.5f;
    if (can_skip_small_rung) {
      const auto second = resolution_adapter_->GetNextHigherResolution(
          nw, nh, context->source_width, context->source_height);
      const int64_t second_area =
          static_cast<int64_t>(second.first) * second.second;
      if (second.first > 0 && second.second > 0 &&
          second_area <= kFastRecoveryMaxPixels &&
          second_area <= static_cast<int64_t>(*context->mapped_target_width) *
                             *context->mapped_target_height) {
        nw = second.first;
        nh = second.second;
      }
    }

    if (fast_recovery) {
      int quality_width = context->source_width,
          quality_height = context->source_height;
      if (resolution_adapter_->GetResolution(
              std::numeric_limits<int>::max(), context->source_width,
              context->source_height, &quality_width, &quality_height) == 0) {
        nw = std::min(*context->mapped_target_width, quality_width);
        nh = std::min(*context->mapped_target_height, quality_height);
      }
    }
    if (keyframe_recovery) {
      nw = context->keyframe_resolution_recovery->width;
      nh = context->keyframe_resolution_recovery->height;
    }
    if (context->mapped_target_width && context->mapped_target_height &&
        nw * nh > *context->mapped_target_width *
                      *context->mapped_target_height) {
      nw = *context->mapped_target_width;
      nh = *context->mapped_target_height;
    }
    if (nw * nh <= bw * bh) {
      return;
    }
    // Once a keyframe has demonstrated a budget limit, a preserved static
    // ceiling must not repeatedly launch trials the current send budget cannot
    // carry. Probing the network can raise this temporary admission ceiling.
    if (context->keyframe_limited_upgrade &&
        context->keyframe_size_budget_bytes > 0) {
      int budget_width = 0, budget_height = 0;
      const int bitrate = static_cast<int>(std::min<int64_t>(
          std::numeric_limits<int>::max(),
          static_cast<int64_t>(context->keyframe_size_budget_bytes) * 8000 /
              kDesktopKeyFrameBudgetMs));
      if (resolution_adapter_->GetResolution(
              bitrate, context->source_width, context->source_height,
              &budget_width, &budget_height) == 0 &&
          static_cast<int64_t>(budget_width) * budget_height <
              static_cast<int64_t>(nw) * nh) {
        nw = budget_width;
        nh = budget_height;
      }
      if (static_cast<int64_t>(nw) * nh <= static_cast<int64_t>(bw) * bh ||
          (!keyframe_recovery &&
           static_cast<int64_t>(nw) * nh >= context->keyframe_failed_pixels &&
           context->keyframe_size_budget_bytes <
               context->keyframe_failed_bytes &&
           now_ms - context->keyframe_failure_ms < 30000)) {
        return;
      }
    }
    if (nw * nh <= bw * bh) {
      return;
    }
    if (!context->encoded_frame_rate_ready ||
        context->measured_encoded_frame_rate < minimum_upgrade_frame_rate ||
        context->encoded_frame_rate_healthy_since_ms == 0 ||
        now_ms - context->encoded_frame_rate_healthy_since_ms <
            kFrameRateHealthyDurationMs ||
        context->normalized_qp_ewma > kMaxNormalizedQpForUpgrade ||
        now_ms < context->next_resolution_upgrade_probe_ms ||
        now_ms - context->last_resolution_change_ms < upgrade_cooldown_ms) {
      return;
    }

    LOG_INFO("Resolution upgrade probe started: channel={} {}x{} -> {}x{}",
             channel_name, bw, bh, nw, nh);
    // Consume the early return only when the trial starts. A failed trial must
    // fall back to the ordinary budget hold and increasing retry backoff.
    if (keyframe_recovery) context->keyframe_resolution_recovery.reset();
    if (nw == context->source_width && nh == context->source_height) {
      context->native_resolution_probe_attempted = true;
    }
    context->resolution_upgrade_probe_active = true;
    context->resolution_upgrade_probe_fast = fast_recovery;
    context->resolution_upgrade_probe_base_width = bw;
    context->resolution_upgrade_probe_base_height = bh;
    context->resolution_upgrade_probe_target_width = nw;
    context->resolution_upgrade_probe_target_height = nh;
    context->resolution_upgrade_probe_sample_count = 0;
    context->resolution_upgrade_probe_started_ms = now_ms;
    context->resolution_upgrade_probe_measurement_started_ms = 0;
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
  // A smaller output does not establish that the failed larger size works.
  context->ClearResolutionUpgradeProbe();
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
    bool measure_encode_delay, const EncodedFrame& encoded_frame,
    uint64_t settings_generation) {
  if (!is_running_.load()) {
    return -1;
  }

  // Asynchronous encoders can finish after the transport has stopped. Avoid
  // putting their output into a paused pacer or continuing its reference chain.
  if (!media_transport_ready_.load()) {
    FullIntraRequest(channel_name);
    return 0;
  }

  {
    std::unique_lock lock(stream_senders_mutex_);
    auto it = stream_senders_.find(channel_name);
    if (it == stream_senders_.end() || it->second != context ||
        !context->transceiver ||
        context->video_settings_generation != settings_generation) {
      return -1;
    }
    context->last_active_time = clock_->CurrentTimeMs();
    const bool keyframe =
        encoded_frame.FrameType() == VideoFrameType::kVideoFrameKey;
    size_t budget = context->keyframe_size_budget_bytes;
    if (context->discarded_keyframe_capture_us > 0 &&
        encoded_frame.CapturedTimestamp() > 0 &&
        encoded_frame.CapturedTimestamp() <=
            context->discarded_keyframe_capture_us) {
      return 0;
    }
    const int64_t now_ms = clock_->CurrentTimeMs();
    if (context->startup_keyframe_pending &&
        (context->resolution_upgrade_network_blocked ||
         now_ms - context->startup_keyframe_started_ms >=
             kStartupKeyFrameTimeoutMs)) {
      context->FinishStartupKeyframe();
      budget = context->keyframe_size_budget_bytes;
    }
    if (budget > 0 && keyframe && encoded_frame.Size() > budget) {
      bool startup_retry = false;
      if (context->startup_keyframe_pending) {
        startup_retry = ++context->startup_keyframe_retry_count == 1;
        if (!startup_retry) {
          context->FinishStartupKeyframe();
          budget = context->keyframe_size_budget_bytes;
        }
      }
      // One modest overshoot at a proven healthy size can be scene-dependent.
      // Discard it and request one fresh keyframe before changing resolution.
      // Never send over budget or grant this retry to an unproven upgrade.
      const bool retry_same_resolution =
          media_config_.video_degradation_preference ==
              VideoDegradationPreference::Balanced &&
          media_config_.video_content_type == VideoContentType::ScreenContent &&
          !context->resolution_upgrade_probe_active &&
          !context->awaiting_budget_keyframe &&
          !context->resolution_upgrade_network_blocked &&
          context->keyframe_budget_retry_count == 0 &&
          encoded_frame.EncodedWidth() == context->target_width &&
          encoded_frame.EncodedHeight() == context->target_height &&
          context->encoded_frame_rate_ready &&
          context->measured_encoded_frame_rate >=
              VideoAdaptationPolicy::UpgradeFrameRate(
                  media_config_.max_frame_rate, true) &&
          context->encoded_frame_rate_healthy_since_ms > 0 &&
          now_ms - context->encoded_frame_rate_healthy_since_ms >= 2000 &&
          now_ms - context->last_resolution_change_ms >= 2000 &&
          context->encode_queue_delay_window_ready &&
          context->p95_encode_queue_delay_ms <= 33 &&
          context->normalized_qp_ewma >= 0 &&
          context->normalized_qp_ewma <= 0.60f &&
          static_cast<double>(encoded_frame.Size()) <= budget * 1.25;
      if (retry_same_resolution) {
        context->keyframe_resolution_recovery =
            StreamContext::KeyframeResolutionRecovery{
                static_cast<int>(encoded_frame.EncodedWidth()),
                static_cast<int>(encoded_frame.EncodedHeight()),
                now_ms + 30000};
        context->keyframe_same_resolution_retry = true;
        context->keyframe_budget_retry_count = 1;
        context->awaiting_budget_keyframe = true;
        context->discarded_keyframe_capture_us =
            std::max(context->discarded_keyframe_capture_us,
                     encoded_frame.CapturedTimestamp());
        FullIntraRequest(channel_name);
        return 0;
      }
      // Preserve the hard send budget, but a small first overshoot only needs
      // a proportional resize. Repeated or large overshoots need more headroom
      // because size does not scale exactly with area across key frames.
      const bool small_first_overshoot =
          context->keyframe_budget_retry_count == 0 &&
          static_cast<double>(encoded_frame.Size()) <=
              static_cast<double>(budget) * 1.10;
      const double headroom = small_first_overshoot ? 0.98 : 0.8;
      double scale = std::sqrt(headroom * static_cast<double>(budget) /
                               encoded_frame.Size());
      if (startup_retry) {
        // The first screen gets one smaller attempt with its extra budget.
        // Leave enough headroom to avoid spending startup on tiny resizes.
        scale = std::min(scale, 0.75);
      }
      context->keyframe_budget_retry_count =
          std::min(1000, context->keyframe_budget_retry_count + 1);
      const int width = std::max(
          2, static_cast<int>(encoded_frame.EncodedWidth() * scale) & ~1);
      const int height = std::max(
          2, static_cast<int>(encoded_frame.EncodedHeight() * scale) & ~1);
      NoteKeyframeBudgetFailure(context, width, height);
      context->keyframe_failed_pixels =
          static_cast<int64_t>(encoded_frame.EncodedWidth()) *
          encoded_frame.EncodedHeight();
      context->keyframe_failed_bytes =
          encoded_frame.Size() + encoded_frame.Size() / 20;
      context->keyframe_failure_ms = clock_->CurrentTimeMs();
      context->last_resolution_change_ms = clock_->CurrentTimeMs();
      context->ResetEncodedFrameRateTracking();
      context->ResetEncoderQualityTracking();
      context->ResetEncodeQueueDelayTracking();
      context->post_upgrade_protection_until_ms = 0;
      context->discarded_keyframe_capture_us =
          std::max(context->discarded_keyframe_capture_us,
                   encoded_frame.CapturedTimestamp());
      FullIntraRequest(channel_name);
      return 0;
    }
    if (context->awaiting_budget_keyframe && !keyframe) {
      // These frames may reference the key frame we discarded, including
      // output already in flight in an asynchronous hardware encoder.
      FullIntraRequest(channel_name);
      return 0;
    }
    if (keyframe) {
      if (context->startup_keyframe_pending) {
        context->FinishStartupKeyframe();
      }
      if (context->keyframe_resolution_recovery) {
        auto& recovery = *context->keyframe_resolution_recovery;
        const int64_t pixels =
            static_cast<int64_t>(encoded_frame.EncodedWidth()) *
            encoded_frame.EncodedHeight();
        const int64_t target_pixels =
            static_cast<int64_t>(recovery.width) * recovery.height;
        if (encoded_frame.EncodedWidth() == recovery.width &&
            encoded_frame.EncodedHeight() == recovery.height) {
          context->keyframe_resolution_recovery.reset();
        } else if (pixels > 0 && pixels < target_pixels &&
                   pixels * 2 >= target_pixels &&
                   clock_->CurrentTimeMs() < recovery.expires_ms) {
          // Only extrapolate from a nearby, successfully encoded size. Keep
          // 25% margin and enforce the hard budget again at the trial size.
          recovery.projected_bytes = static_cast<double>(encoded_frame.Size()) *
                                     target_pixels / pixels * 1.25;
        }
      }
      context->awaiting_budget_keyframe = false;
      context->keyframe_same_resolution_retry = false;
      context->keyframe_budget_retry_count = 0;
    }
  }

  if (measure_encode_delay) {
    MaybeDegradeResolutionOnEncodeTime(channel_name, queue_delay_ms,
                                       encoded_frame, settings_generation);
  }

  std::shared_lock lock(stream_senders_mutex_);
  if (!is_running_.load()) {
    return -1;
  }

  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || it->second != context ||
      !context->transceiver ||
      context->video_settings_generation != settings_generation) {
    return -1;
  }

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

  const int64_t captured_timestamp_us =
      audio_frame->captured_timestamp != 0
          ? static_cast<int64_t>(audio_frame->captured_timestamp)
          : clock_->CurrentTimeUs();
  int ret = context->codec->Encode(
      reinterpret_cast<const uint8_t*>(audio_frame->data), audio_frame->size,
      [this, channel_name, context, captured_timestamp_us](
          char* encoded_audio_buffer, size_t size,
          uint32_t samples_per_channel) -> int {
        context->last_active_time = clock_->CurrentTimeMs();
        return context->transceiver->SendAudio(
            encoded_audio_buffer, size, samples_per_channel,
            captured_timestamp_us);
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

void IceTransportController::SetRelayPath(bool relay_path) {
  const bool changed = relay_path_state_.exchange(relay_path ? 1 : 0) != (relay_path ? 1 : 0);
  if (!task_queue_cc_ || !controller_) {
    return;
  }

  task_queue_cc_->PostTask([this, relay_path, changed]() mutable {
    if (!controller_) {
      return;
    }
    if (changed) ResetFecAdaptation();
    const webrtc::Timestamp now = webrtc::Timestamp::Millis(
        webrtc_clock_->TimeInMilliseconds());
    PostUpdates(controller_->SetRelayPath(relay_path, now));
  });
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
  if (transport_ready && !media_transport_ready_.load()) {
    // Arm every video stream before publishing readiness to capture threads.
    // Repeated ICE READY notifications must not request extra key frames.
    FullIntraRequestAllVideoStreams();
  }
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
            ResetFecAdaptation();
            webrtc::NetworkAvailability msg;
            msg.at_time = webrtc::Timestamp::Millis(
                webrtc_clock_->TimeInMilliseconds());
            msg.network_available = transport_ready;
            PostUpdates(controller_->OnNetworkAvailability(msg));
          }
        });
  }
}

int IceTransportController::DecryptIncomingPacket(uint8_t* buffer, int* size,
                                                  uint32_t* out_ssrc) {
  if (!buffer || !size || *size < 12) {
    return -static_cast<int>(srtp_err_status_bad_param);
  }

  uint8_t version = (buffer[0] >> 6) & 0x03;
  if (version != 2) {
    return -static_cast<int>(srtp_err_status_bad_param);
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
    return -static_cast<int>(srtp_err_status_no_ctx);
  }

  int len = *size;
  const int result = it->second->unprotectRtp(buffer, &len);
  if (result < 0) {
    return result;
  }

  *size = len;
  return 0;
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
    const char* data, size_t size, const std::string& channel_name,
    uint16_t sequence, uint32_t timestamp) {
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
    auto decoder = std::static_pointer_cast<AudioDecoder>(context->codec);
    int num_frame_returned = decoder->DecodePacket(
        (const uint8_t*)data, size, sequence, timestamp,
        [this, channel_name](uint8_t* data, int size) {
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

int IceTransportController::UpdateVideoSettings(
    VideoQuality quality, int frame_rate,
    VideoDegradationPreference preference) {
  if (!is_running_.load() || !task_queue_encode_) return -1;
  auto completion = std::make_shared<std::promise<int>>();
  auto result = completion->get_future();
  std::weak_ptr<IceTransportController> weak = shared_from_this();
  task_queue_encode_->PostTask([weak, quality, frame_rate, preference,
                                completion] {
    auto self = weak.lock();
    if (!self || !self->is_running_.load()) {
      completion->set_value(-1);
      return;
    }
    // Release replaced hardware encoders outside the adaptation lock: their
    // destructors wait for callbacks which acquire that same lock.
    std::vector<std::shared_ptr<MediaCodec>> retired_encoders;
    std::unique_lock lock(self->stream_senders_mutex_);
    if (self->video_quality_ == quality &&
        self->media_config_.max_frame_rate == frame_rate &&
        self->media_config_.video_degradation_preference == preference) {
      completion->set_value(0);
      return;
    }
    auto config = self->media_config_;
    config.max_frame_rate = frame_rate;
    config.video_degradation_preference = preference;
    ResolutionAdapter adapter(quality, frame_rate, config.video_content_type,
                              preference);
    std::unordered_map<std::string, std::shared_ptr<MediaCodec>> encoders;
    for (auto& [name, context] : self->stream_senders_) {
      if (!context || context->type != StreamType::kVideo || !context->codec)
        continue;
      config.init_width = context->target_width.value_or(config.init_width);
      config.init_height = context->target_height.value_or(config.init_height);
      config.init_bitrate =
          context->applied_target_bitrate.value_or(MINIRTC_INIT_BITRATE);
      if (context->source_width > 0 && context->source_height > 0) {
        adapter.GetResolution(config.init_bitrate, context->source_width,
                              context->source_height, &config.init_width,
                              &config.init_height);
      }
      auto codec = VideoEncoderFactory::CreateInitializedVideoEncoder(
          self->clock_, config, self->video_encoder_hardware_,
          self->video_codec_type_);
      if (!codec) {
        LOG_ERROR("Live video settings: encoder creation failed for [{}]",
                  name);
        completion->set_value(-1);
        return;
      }
      encoders.emplace(name, std::move(codec));
    }
    self->media_config_.max_frame_rate = frame_rate;
    self->media_config_.video_degradation_preference = preference;
    self->video_quality_ = quality;
    self->resolution_adapter_->SetPreferences(quality, frame_rate, preference);
    self->video_frame_cadences_.clear();
    for (auto& [name, codec] : encoders) {
      auto& context = self->stream_senders_.at(name);
      retired_encoders.push_back(std::move(context->codec));
      context->codec = std::move(codec);
      ++context->video_settings_generation;
      context->encoding_speed_priority_enabled =
          preference == VideoDegradationPreference::MaintainFrameRate &&
          context->codec->SupportsDynamicEncodingSpeedPriority() &&
          context->codec->SetPrioritizeEncodingSpeedOverQuality(true) == 0;
      int width = 0, height = 0;
      if (context->codec->GetResolution(&width, &height) == 0 && width > 0 &&
          height > 0) {
        context->target_width = width;
        context->target_height = height;
      }
      context->bitrate_update_queued = false;
      context->initial_resolution_recovery = true;
      context->native_resolution_probe_attempted = false;
      context->source_resolution_initialized_ms = self->clock_->CurrentTimeMs();
      context->last_resolution_change_ms = self->clock_->CurrentTimeMs();
      context->keyframe_resolution_recovery.reset();
      context->keyframe_limited_upgrade = false;
      context->keyframe_failed_pixels = 0;
      context->keyframe_failed_bytes = 0;
      context->keyframe_failure_ms = 0;
      context->keyframe_same_resolution_retry = false;
      context->startup_keyframe_pending = false;
      context->mapped_target_width.reset();
      context->mapped_target_height.reset();
      context->ResetPendingBandwidthMapping();
      context->ResetResolutionUpgradeProbe();
      context->ResetEncodedFrameRateTracking();
      context->ResetEncoderQualityTracking();
      context->ResetEncodeQueueDelayTracking();
      context->post_upgrade_protection_until_ms = 0;
      context->awaiting_budget_keyframe = false;
      context->keyframe_budget_retry_count = 0;
      context->discarded_keyframe_capture_us = 0;
      context->keyframe_size_budget_bytes = 0;
      context->regular_keyframe_size_budget_bytes = 0;
      context->resolution_upgrade_network_blocked = false;
      self->FullIntraRequest(name);
    }
    if (self->paced_sender_) {
      self->paced_sender_->SetDrainLargeQueues(
          !UsesBoundedVideoQueue(self->media_config_));
    }
    lock.unlock();
    retired_encoders.clear();
    LOG_INFO("Live video settings: quality={}, fps={}, preference={}",
             static_cast<int>(quality), frame_rate,
             static_cast<int>(preference));
    completion->set_value(0);
  });
  // PostTask may reject during shutdown. Drop our promise reference so a
  // discarded task yields broken_promise rather than waiting forever.
  completion.reset();
  try {
    return result.get();
  } catch (const std::future_error&) {
    return -1;
  }
}

int IceTransportController::CreateStreamCodecs(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    VideoCodecType codec_type) {
  video_codec_type_ = codec_type;
  video_encoder_hardware_ = hardware_acceleration;
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
          context->codec = std::make_shared<AudioDecoder>(48000, 1, 480);
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

void IceTransportController::OnTransportRtt(double rtt_ms) {
  if (!std::isfinite(rtt_ms) || rtt_ms < 0 || rtt_ms > 2000) return;
  if (ice_io_statistics_) ice_io_statistics_->RecordRtt(rtt_ms);
  const int64_t receiver_rtt_ms = std::llround(rtt_ms);
  std::shared_lock lock(stream_receivers_mutex_);
  for (const auto& [_, context] : stream_receivers_) {
    if (context && context->type == StreamType::kVideo && context->transceiver)
      context->transceiver->OnRttUpdate(receiver_rtt_ms);
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

  if (transport_rtt_ms.has_value()) OnTransportRtt(*transport_rtt_ms);

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
          UpdateCongestedState();
        }
      });
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
    const webrtc::PacedPacketInfo& pacing_info, size_t send_size) {
  PacketFeedbackRegistration registration;
  registration.send_time_ms = clock_->CurrentTimeMs();
  registration.send_size = send_size;
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
  sent_packet.info.packet_size_bytes = send_size;
  sent_packet.info.packet_type = rtc::PacketType::kData;

  {
    std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
    registration.fec_feedback_id = transport_feedback_adapter_.AddPacket(
        packet, pacing_info, send_size - packet.size(),
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
  transport_feedback_adapter_.RemoveFecSend(registration.fec_feedback_id);
  transport_feedback_adapter_.RemovePacket(packet);
}

void IceTransportController::OnSentPacket(
    const webrtc::RtpPacketToSend& packet,
    const PacketFeedbackRegistration& registration) {
  if (registration.tracked) {
    std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
    transport_feedback_adapter_.CommitFecSend(registration.fec_feedback_id);
  }
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
    sent_packet.info.packet_size_bytes = registration.send_size;
    sent_packet.info.packet_type = rtc::PacketType::kData;

    std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
    transport_feedback_adapter_.ProcessSentPacket(sent_packet);
  }

  if (task_queue_cc_) {
    const size_t packet_size = registration.send_size;
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
    available_transport_bitrate_ = std::max<int64_t>(0, update.target_rate->target_rate.bps());
    target_bitrate_ = available_transport_bitrate_;
    video_network_estimate_ = update.target_rate->network_estimate;
  }
  UpdateFecProtection();

  // A stable estimate need not produce another target-rate notification.
  // Advance confirmation timers on regular controller ticks as well.
  if (video_network_estimate_) {
    std::unique_lock lock(stream_senders_mutex_);
    if (!stream_senders_.empty()) {
      // Resolution decisions use the same media budget as encoder updates.
      int video_count = 0;
      for (auto& [_, context] : stream_senders_) {
        if (context->last_active_time.has_value()) {
          if (clock_->CurrentTimeMs() - context->last_active_time.value() <
              100) {
            if (context->type == StreamType::kVideo) {
              video_count++;
            }
          }
        }
      }
      // Allocate bandwidth to video channels
      if (video_count > 0) {
        const auto& network_estimate = *video_network_estimate_;
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
            network_estimate.round_trip_time.IsFinite()
                ? network_estimate.round_trip_time.ms()
                : std::numeric_limits<int64_t>::max();
        const bool static_content_candidate =
            VideoAdaptationPolicy::IsStaticContentCandidate(
                is_screen_content, network_estimate.in_alr,
                network_estimate.loss_rate_ratio, network_rtt_ms);
        const bool static_content_network_critical =
            VideoAdaptationPolicy::IsStaticContentNetworkCritical(
                network_estimate.loss_rate_ratio, network_rtt_ms);
        for (auto& [channel_name, context] : stream_senders_) {
          if (!context->codec || context->type != StreamType::kVideo ||
              !context->last_active_time ||
              clock_->CurrentTimeMs() - *context->last_active_time >= 100) {
            continue;
          }
          const int sub_target_bitrate = static_cast<int>(context->fec_protection.media_bitrate_bps);

          int source_width = context->source_width;
          int source_height = context->source_height;
          if ((source_width <= 0 || source_height <= 0) &&
              context->codec->GetResolution(&source_width, &source_height) !=
                  0) {
            continue;
          }

          const int64_t now_ms = clock_->CurrentTimeMs();
          context->resolution_upgrade_network_blocked =
              allow_spatial_downgrade && is_screen_content &&
              static_content_network_critical;
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

          int target_width = -1;
          int target_height = -1;
          if (resolution_adapter_->GetResolution(
                  sub_target_bitrate, source_width, source_height,
                  &target_width, &target_height) != 0) {
            continue;
          }
          // Do not apply a bandwidth downgrade while static-content entry is
          // being confirmed. This avoids a downgrade immediately followed by
          // a quality restoration and a forced key frame.
          if (!allow_spatial_downgrade && !context->freeze_resolution &&
              static_content_candidate) {
            continue;
          }

          if (context->freeze_resolution && allow_spatial_downgrade) {
            // Low traffic must not lower a static desktop's recovery ceiling.
            // A sustained higher estimate can still raise it below, including
            // estimates learned from probing while application-limited.
            const int current_width =
                context->target_width.value_or(source_width);
            const int current_height =
                context->target_height.value_or(source_height);
            if (!context->mapped_target_width.has_value() ||
                !context->mapped_target_height.has_value()) {
              context->mapped_target_width = current_width;
              context->mapped_target_height = current_height;
            }

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
          }

          if (context->freeze_resolution && !allow_spatial_downgrade) {
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
            context->ResetPendingBandwidthMapping();

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

          if (was_frozen && !context->freeze_resolution) {
            LOG_INFO(
                "Leave static-content resolution hold: channel={} reason={}",
                channel_name,
                !is_screen_content
                    ? "content_type"
                    : (static_content_network_critical
                           ? "network_conditions"
                           : "alr_exit_hysteresis"));
            context->ResetPendingBandwidthMapping();
            // Until a new non-ALR bandwidth ceiling is stable, prevent an
            // encode-time upgrade from using the optimistic static ceiling.
            context->mapped_target_width =
                context->target_width.value_or(source_width);
            context->mapped_target_height =
                context->target_height.value_or(source_height);
          }

          const int ceiling_width = context->mapped_target_width.value_or(
              context->target_width.value_or(source_width));
          const int ceiling_height = context->mapped_target_height.value_or(
              context->target_height.value_or(source_height));
          const int64_t ceiling_area =
              static_cast<int64_t>(ceiling_width) * ceiling_height;
          const bool raising_ceiling =
              static_cast<int64_t>(target_width) * target_height > ceiling_area;
          if (raising_ceiling && context->resolution_upgrade_network_blocked) {
            context->ResetPendingBandwidthMapping();
            continue;
          }
          if (allow_spatial_downgrade &&
              (context->freeze_resolution || static_content_candidate) &&
              !raising_ceiling) {
            context->ResetPendingBandwidthMapping();
            continue;
          }

          // Confirm the full bandwidth-supported ceiling once. Actual size
          // changes remain bounded by encode-time probes, so imposing a second
          // spatial ladder here only delays recovery. Mild static-candidate
          // changes must not reset confirmation while bandwidth still supports
          // the pending size; severe network pressure is handled above.
          const int64_t candidate_area =
              static_cast<int64_t>(target_width) * target_height;
          const int64_t pending_area =
              static_cast<int64_t>(context->pending_mapped_width.value_or(0)) *
              context->pending_mapped_height.value_or(0);
          const bool candidate_supported =
              pending_area > 0 &&
              (raising_ceiling
                   ? pending_area > ceiling_area &&
                         candidate_area >= pending_area
                   : context->pending_mapped_width == target_width &&
                         context->pending_mapped_height == target_height);
          if (!candidate_supported) {
            context->ResetPendingBandwidthMapping();
            context->pending_mapped_width = target_width;
            context->pending_mapped_height = target_height;
            context->mapping_stability_count = 1;
            context->pending_mapped_since_ms = now_ms;
            if (raising_ceiling) {
              context->pending_mapped_candidates.push_back(
                  {target_width, target_height, now_ms});
            }
            continue;
          }

          const int64_t increase_hold_ms =
              VideoAdaptationPolicy::kBandwidthMappingIncreaseStabilityMs;
          if (raising_ceiling) {
            auto& candidates = context->pending_mapped_candidates;
            int64_t supported_since_ms = now_ms;
            // Keep the original confirmation start while tracking which
            // higher sizes have remained supported. A brief spike disappears
            // on a retreat without erasing the lower candidate's progress.
            while (!candidates.empty() &&
                   static_cast<int64_t>(candidates.back().width) *
                           candidates.back().height >
                       candidate_area) {
              // A larger supported size also proves support for this smaller
              // one; carry that history through a partial retreat.
              supported_since_ms = candidates.back().since_ms;
              candidates.pop_back();
            }
            if (candidates.empty() || candidates.back().width != target_width ||
                candidates.back().height != target_height) {
              candidates.push_back(
                  {target_width, target_height, supported_since_ms});
            }
            // The front is the largest size supported throughout the recent
            // hold window. Older, smaller entries no longer affect admission.
            while (candidates.size() > 1 &&
                   now_ms - candidates[1].since_ms >= increase_hold_ms) {
              candidates.pop_front();
            }
          }

          ++context->mapping_stability_count;
          if (!VideoAdaptationPolicy::IsBandwidthMappingStable(
                  now_ms, context->pending_mapped_since_ms)) {
            continue;
          }

          if (raising_ceiling) {
            const auto& supported = context->pending_mapped_candidates.front();
            const bool latest_still_confirming =
                candidate_area >
                static_cast<int64_t>(supported.width) * supported.height;
            // A late increase may extend confirmation by at most one short
            // window. Continuous growth must not postpone all upgrades.
            if (latest_still_confirming &&
                now_ms - context->pending_mapped_since_ms <
                    VideoAdaptationPolicy::kBandwidthMappingStabilityMs +
                        increase_hold_ms) {
              continue;
            }
            target_width = supported.width;
            target_height = supported.height;
          } else {
            target_width = *context->pending_mapped_width;
            target_height = *context->pending_mapped_height;
          }
          context->mapped_target_width = target_width;
          context->mapped_target_height = target_height;
          context->ResetPendingBandwidthMapping();

          const int current_width =
              context->target_width.value_or(source_width);
          const int current_height =
              context->target_height.value_or(source_height);
          const int64_t current_area =
              static_cast<int64_t>(current_width) * current_height;
          const int64_t target_area =
              static_cast<int64_t>(target_width) * target_height;
          if (VideoAdaptationPolicy::ShouldApplyBandwidthResolutionDowngrade(
                  allow_spatial_downgrade && !context->freeze_resolution &&
                      !static_content_candidate,
                  now_ms, context->source_resolution_initialized_ms,
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
            context->ClearResolutionUpgradeProbe();
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

void IceTransportController::ResetFecAdaptation() {
  {
    std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
    transport_feedback_adapter_.ResetFecFeedback();
  }
  fec_feedback_snapshots_.clear();
  fec_snapshot_ms_ = -1;
  std::unique_lock lock(stream_senders_mutex_);
  for (const auto &item : stream_senders_) {
    if (!item.second)
      continue;
    item.second->fec_controller.Reset();
    item.second->rtx_bitrate_ewma = 0;
  }
}

void IceTransportController::UpdateFecProtection() {
  if (!task_queue_pacer_ || !paced_sender_)
    return;
  const int64_t now_ms = clock_->CurrentTimeMs();
  const int64_t rtt_ms =
      video_network_estimate_ &&
              video_network_estimate_->round_trip_time.IsFinite()
          ? std::clamp<int64_t>(video_network_estimate_->round_trip_time.ms(),
                                0, 2000)
          : 0;
  const bool new_snapshot =
      fec_snapshot_ms_ < 0 || now_ms - fec_snapshot_ms_ >= 200;
  if (new_snapshot) {
    std::lock_guard<std::mutex> lock(transport_feedback_adapter_mutex_);
    fec_feedback_snapshots_ =
        transport_feedback_adapter_.FecFeedback(now_ms, rtt_ms);
    fec_snapshot_ms_ = now_ms;
  }
  const int64_t queue_ms = std::max(paced_sender_->ExpectedQueueTime().ms(),
                                    paced_sender_->OldestPacketWaitTime().ms());
  const auto mode = video_fec_enabled_ ? fec_mode_.load() : FecMode::kOff;
  const bool log = fec_log_ms_ < 0 || now_ms - fec_log_ms_ >= 5000;
  bool publish = false;
  int64_t media_total = 0, fec_total = 0;
  {
    std::unique_lock lock(stream_senders_mutex_);
    const uint64_t version = ++fec_config_version_;
    auto active = [now_ms](const std::shared_ptr<StreamContext> &c) {
      return c &&
             ((c->last_active_time && now_ms - *c->last_active_time < 100) ||
              (c->type == StreamType::kVideo && c->last_capture_time &&
               now_ms - *c->last_capture_time < 100));
    };
    int videos = 0, audio = 0, data = 0;
    for (const auto &item : stream_senders_) {
      const auto &c = item.second;
      if (!active(c))
        continue;
      if (c->type == StreamType::kVideo && c->codec)
        ++videos;
      if (c->type == StreamType::kAudio)
        ++audio;
      if (c->type == StreamType::kData)
        ++data;
    }
    // Audio currently bypasses the video pacer. Reserve its 32 kbit/s Opus
    // payload plus 100 RTP packets/s headers conservatively, per active stream.
    const int64_t other =
        int64_t(audio) * 64000 + (data ? target_bitrate_ / 10 : 0);
    const int64_t share =
        std::max<int64_t>(0, target_bitrate_ - other) / std::max(1, videos);
    for (const auto &item : stream_senders_) {
      const auto &c = item.second;
      if (!c || c->type != StreamType::kVideo || !c->transceiver)
        continue;
      const uint32_t ssrc = c->ssrc.value_or(0), rtx = c->rtx_ssrc.value_or(0);
      FecNetworkSnapshot network;
      network.now_ms = now_ms;
      network.rtt_ms = rtt_ms;
      network.queue_ms = queue_ms;
      network.transport_bps = target_bitrate_;
      network.congested =
          is_congested_.load() || !media_transport_ready_.load();
      auto feedback = fec_feedback_snapshots_.find(ssrc);
      if (feedback != fec_feedback_snapshots_.end())
        network.feedback = feedback->second;
      if (new_snapshot) {
        auto auxiliary = fec_feedback_snapshots_.find(rtx);
        const int64_t actual_rtx =
            auxiliary == fec_feedback_snapshots_.end()
                ? 0
                : auxiliary->second
                      .sent_bps[static_cast<size_t>(FecPacketKind::kRtx)];
        // Reserve increases immediately and release them slowly.
        c->rtx_bitrate_ewma =
            std::max<double>(actual_rtx, c->rtx_bitrate_ewma * 0.8);
      }
      FecProtectionConfig config;
      if (active(c)) {
        c->fec_last_active_ms = now_ms;
        config = c->fec_controller.Update(mode, network);
        config = AllocateFecBudget(
            config, share - static_cast<int64_t>(c->rtx_bitrate_ewma),
            network.feedback.payload_fraction);
        media_total += config.media_bitrate_bps;
        fec_total += config.fec_bitrate_bps;
      } else {
        config.source_ratio = 0;
        config.fec_bitrate_bps = 0;
        if (c->fec_last_active_ms >= 0 &&
            now_ms - c->fec_last_active_ms > 1000) {
          c->fec_controller.Reset();
          c->rtx_bitrate_ewma = 0;
          c->fec_last_active_ms = -1;
        }
      }
      config.version = version;
      c->fec_protection = config;
      if (log && active(c)) {
        const auto auxiliary = fec_feedback_snapshots_.find(rtx);
        const int64_t sent_repairs =
            auxiliary == fec_feedback_snapshots_.end()
                ? 0
                : auxiliary->second
                      .sent_bps[static_cast<size_t>(FecPacketKind::kRepair)];
        LOG_INFO("FEC control: stream={} mode={} reason={} samples={} loss={} "
                 "rtt={} queue={} feedback_age={} ratio={} media_bps={} "
                 "fec_bps={} rtx_bps={} sent_media_bps={} sent_fec_bps={}",
                 item.first, int(mode), FecDecisionName(config.reason),
                 network.feedback.samples, network.feedback.loss_rate(), rtt_ms,
                 queue_ms,
                 network.feedback.last_feedback_ms < 0
                     ? -1
                     : now_ms - network.feedback.last_feedback_ms,
                 config.source_ratio, config.media_bitrate_bps,
                 config.fec_bitrate_bps,
                 static_cast<int64_t>(c->rtx_bitrate_ewma),
                 network.feedback
                     .sent_bps[static_cast<size_t>(FecPacketKind::kMedia)],
                 sent_repairs);
      }
    }
    // Keep a startup admission budget for the first captured frame of a new
    // stream. Idle streams still receive zero FEC allowance.
    if (!videos) {
      FecProtectionConfig startup;
      startup.source_ratio = mode == FecMode::kOff     ? 0
                             : mode == FecMode::kFixed ? 0.25
                                                       : 0.10;
      media_total = AllocateFecBudget(startup, target_bitrate_ - other, 0.94)
                        .media_bitrate_bps;
    }
    video_transport_bitrate_bps_.store(media_total);
    publish = !fec_update_queued_;
    fec_update_queued_ = true;
  }
  if (log) {
    fec_log_ms_ = now_ms;
    LOG_INFO("FEC pacer: dropped={} media_bps={} fec_bps={}",
             paced_sender_->FecDroppedPackets(), media_total, fec_total);
  }
  if (!publish)
    return;
  std::weak_ptr<IceTransportController> weak = shared_from_this();
  if (!task_queue_pacer_->PostTask([weak] {
        auto self = weak.lock();
        if (!self)
          return;
        std::unique_lock lock(self->stream_senders_mutex_);
        self->fec_update_queued_ = false;
        if (!self->is_running_.load())
          return;
        std::map<uint32_t, int64_t> rates;
        int64_t total = 0;
        for (const auto &item : self->stream_senders_) {
          const auto &c = item.second;
          if (!c || c->type != StreamType::kVideo || !c->transceiver)
            continue;
          if (c->rtx_ssrc)
            rates[*c->rtx_ssrc] = c->fec_protection.fec_bitrate_bps;
          total += std::max<int64_t>(0, c->fec_protection.fec_bitrate_bps);
          c->transceiver->SetFecProtection(c->fec_protection);
        }
        self->paced_sender_->SetFecBudgets(rates, total,
                                           self->fec_config_version_);
      })) {
    std::unique_lock lock(stream_senders_mutex_);
    fec_update_queued_ = false;
  }
}

void IceTransportController::UpdateVideoBitrateAllocation() {
  if (!is_running_.load() || !task_queue_encode_) {
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
      return context && ((context->last_active_time &&
             now_ms - *context->last_active_time < kActiveStreamTimeoutMs) ||
             (context->type == StreamType::kVideo && context->last_capture_time &&
              now_ms - *context->last_capture_time < kActiveStreamTimeoutMs));
    };

    int active_video_count = 0;
    for (const auto& [_, context] : stream_senders_) {
      if (!is_active(context)) {
        continue;
      }
      if (context->type == StreamType::kVideo && context->codec) {
        ++active_video_count;
      }
    }

    if (active_video_count == 0) {
      return;
    }

    for (const auto& [channel_name, context] : stream_senders_) {
      if (!is_active(context) || context->type != StreamType::kVideo ||
          !context->codec) {
        continue;
      }

      const int per_video_bitrate = static_cast<int>(context->fec_protection.media_bitrate_bps);
      context->desired_target_bitrate = per_video_bitrate;
      // Admission pauses at zero; avoid passing unsupported zero to codecs.
      if (per_video_bitrate <= 0) continue;
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
    if (target_bitrate <= 0) {
      context->bitrate_update_queued = false;
      return;
    }
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
