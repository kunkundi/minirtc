#include "ice_transport_controller.h"

#include <memory>
#include <vector>

#include "data_channel_send.h"
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

IceTransportController::IceTransportController(
    std::shared_ptr<SystemClock> clock, std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics, bool enable_srtp,
    VideoQuality video_quality)
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
      load_nvcodec_dll_success_(false),
      hardware_acceleration_(false),
      is_running_(true),
      congestion_window_size_(DataSize::PlusInfinity()) {
  SetPeriod(std::chrono::milliseconds(25));
  SetThreadName("IceTransportController");
}

IceTransportController::~IceTransportController() {
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

#if USE_CUDA && !defined(__aarch64__) && !defined(__arm__) && \
    !defined(__APPLE__)
  if (hardware_acceleration_ && load_nvcodec_dll_success_) {
    ReleaseNvCodecDll();
  }
#endif
  load_nvcodec_dll_success_ = false;
}

void IceTransportController::Create(bool offer_peer, std::string remote_user_id,
                                    rtp::PAYLOAD_TYPE video_codec_payload_type,
                                    bool hardware_acceleration, bool enable_fec,
                                    OnReceiveVideo on_receive_video,
                                    OnReceiveAudio on_receive_audio,
                                    OnReceiveData on_receive_data,
                                    void* user_data) {
  offer_peer_ = offer_peer;
  remote_user_id_ = remote_user_id;
  enable_fec_ = enable_fec && video_codec_payload_type == rtp::PAYLOAD_TYPE::H264;
  on_receive_video_ = on_receive_video;
  on_receive_audio_ = on_receive_audio;
  on_receive_data_ = on_receive_data;
  user_data_ = user_data;

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
  paced_sender_->SetQueueTimeLimit(TimeDelta::Millis(2000));
  paced_sender_->SetAllowProbeWithoutMediaPacket(true);
  std::weak_ptr<IceTransportController> weak_this = shared_from_this();
  paced_sender_->SetOnSentPacketFunc(
      [weak_this](std::unique_ptr<webrtc::RtpPacketToSend> packet) {
        if (auto self = weak_this.lock()) {
          if (self->ice_agent_) {
            webrtc::Timestamp now = self->webrtc_clock_->CurrentTime();

            if (self->enable_srtp_) {
              int len = packet->Size();

              std::vector<uint8_t> srtp_packet_buf(len + 16);
              memcpy(srtp_packet_buf.data(), packet->Buffer().data(), len);

              auto srtp_session = self->ssrc_to_srtp_sender_[packet->Ssrc()];
              if (srtp_session && srtp_session->valid()) {
                if (srtp_session->protectRtp(srtp_packet_buf.data(), &len) <
                    0) {
                  LOG_ERROR("SRTP protect failed for stream [{}]",
                            packet->Ssrc());
                  return;
                }
              }

              self->ice_agent_->Send(
                  reinterpret_cast<const char*>(srtp_packet_buf.data()), len);
            } else {
              self->ice_agent_->Send((const char*)packet->Buffer().data(),
                                     packet->Size());
            }

            self->OnSentPacket(*packet);

            if (packet->packet_type().has_value()) {
              switch (packet->packet_type().value()) {
                case webrtc::RtpPacketMediaType::kVideo:
                case webrtc::RtpPacketMediaType::kForwardErrorCorrection:
                case webrtc::RtpPacketMediaType::kRetransmission: {
                  self->last_active_stream_ = packet->get_stream_name();
                  std::shared_lock lock(self->stream_senders_mutex_);
                  if (self->stream_senders_.find(self->last_active_stream_) !=
                      self->stream_senders_.end()) {
                    self->stream_senders_[self->last_active_stream_]
                        ->transceiver->OnSentRtpPacket(std::move(packet));
                  }
                } break;
                default:
                  break;
              }
            }
          }
        }
      });

  paced_sender_->SetGeneratePaddingFunc(
      [weak_this](uint32_t size, int64_t captured_timestamp_us)
          -> std::vector<std::unique_ptr<RtpPacket>> {
        if (auto self = weak_this.lock()) {
          std::shared_lock lock(self->stream_senders_mutex_);
          auto it = self->stream_senders_.find(self->last_active_stream_);
          if (it != self->stream_senders_.end() && it->second &&
              it->second->type == StreamType::kVideo &&
              it->second->transceiver) {
            return it->second->transceiver->GeneratePadding(
                size, captured_timestamp_us);
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
                size, captured_timestamp_us);
          }
          return {};
        } else {
          return {};
        }
      });

  resolution_adapter_ = std::make_unique<ResolutionAdapter>(video_quality_);

  {
    std::shared_lock lock(stream_senders_mutex_);
    for (auto& [channel_name, context] : stream_senders_) {
      if (context) {
        if (context->type == StreamType::kVideo) {
          FecConfig fec_config;
          fec_config.enabled = enable_fec_;
          context->transceiver->SetFecConfig(fec_config);
          context->transceiver->Initialize(video_codec_payload_type,
                                           paced_sender_);
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
}

void IceTransportController::Destroy() {
  is_running_.store(false);

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
  }
  return context->transceiver ? context->transceiver->GetSsrc() : 0;
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
    context->type = StreamType::kVideo;
    context->direction = StreamDirection::kReceive;
    context->ssrc = ssrc;
    ssrc_to_name_[ssrc] = channel_name;
  }

  if (!context->transceiver) {
    std::weak_ptr<IceTransportController> weak_self = shared_from_this();
    context->transceiver = std::make_shared<VideoChannelReceive>(
        channel_name, ssrc, clock_, ice_agent_, ice_io_statistics_,
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

int IceTransportController::SendVideo(const XVideoFrame* video_frame,
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

  if (task_queue_encode_) {
    RawFrame raw_frame((const uint8_t*)video_frame->data, video_frame->size,
                       video_frame->width, video_frame->height);
    raw_frame.SetCapturedTimestamp(clock_->CurrentTimeUs());

    // Save the original capture resolution so later resolution changes keep the
    // same aspect ratio.
    if (context->source_width <= 0 || context->source_height <= 0 ||
        context->source_width != raw_frame.Width() ||
        context->source_height != raw_frame.Height()) {
      context->source_width = raw_frame.Width();
      context->source_height = raw_frame.Height();
    }

    if (task_queue_encode_->PendingTasks() > 0) {
      return 0;
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
        int64_t queue_delay_ms = encode_queue->CurrentTaskQueueDelayMs();
        if (force_i_frame) {
          context->codec->ForceIdr();
          LOG_INFO("Force I frame");
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
      RawFrame scaled_frame(context->target_width.value() *
                            context->target_height.value() * 3 / 2);

      scaled_frame.SetWidth(context->target_width.value());
      scaled_frame.SetHeight(context->target_height.value());
      scaled_frame.SetCapturedTimestamp(clock_->CurrentTimeUs());

      resolution_adapter_->ResolutionDowngrade(
          raw_frame, context->target_width.value(),
          context->target_height.value(), scaled_frame);

      post_encode(std::move(scaled_frame));
    } else {
      post_encode(std::move(raw_frame));
    }
  }

  return 0;
}

void IceTransportController::MaybeDegradeResolutionOnEncodeTime(
    const std::string& channel_name, int queue_delay_ms, uint32_t encoded_w,
    uint32_t encoded_h) {
  constexpr int kDelayThresholdMs = 8;
  constexpr int kDowngradeStreak = 8;
  constexpr int kUpgradeStreak = 15;
  constexpr int kUpgradeCooldownMs = 5000;
  constexpr int kDowngradeCooldownMs = 3000;

  if (!is_running_.load()) return;

  std::unique_lock lock(stream_senders_mutex_);
  if (!is_running_.load()) return;
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) return;
  auto& ctx = it->second;

  if (queue_delay_ms >= kDelayThresholdMs) {
    ++ctx->encode_exceed_count;
    ctx->encode_below_threshold_count = 0;
  } else {
    ctx->encode_exceed_count = 0;
    ++ctx->encode_below_threshold_count;
  }

  auto base = [&]() -> std::pair<int, int> {
    if (ctx->target_width && ctx->target_height)
      return {*ctx->target_width, *ctx->target_height};
    return {(int)encoded_w, (int)encoded_h};
  };

  // Upgrade
  if (ctx->encode_exceed_count < kDowngradeStreak) {
    if (ctx->freeze_resolution || !ctx->target_width || !ctx->target_height ||
        ctx->encode_below_threshold_count < kUpgradeStreak)
      return;
    auto [bw, bh] = base();
    int64_t now = clock_->CurrentTimeMs();
    if (now - ctx->last_resolution_change_ms < kUpgradeCooldownMs) return;

    auto [nw, nh] = resolution_adapter_
                        ? resolution_adapter_->GetNextHigherResolution(
                              bw, bh, ctx->source_width, ctx->source_height)
                        : std::pair<int, int>{-1, -1};
    if (nw <= 0 || nh <= 0) return;

    if (ctx->mapped_target_width && ctx->mapped_target_height &&
        nw * nh > *ctx->mapped_target_width * *ctx->mapped_target_height) {
      nw = *ctx->mapped_target_width;
      nh = *ctx->mapped_target_height;
    }
    if (nw * nh <= bw * bh) return;

    LOG_INFO("Resolution upgrade: channel={} {}x{} -> {}x{}", channel_name, bw,
             bh, nw, nh);
    ctx->target_width = nw;
    ctx->target_height = nh;
    ctx->encode_below_threshold_count = 0;
    ctx->last_resolution_change_ms = now;
    FullIntraRequest(channel_name);
    return;
  }

  // Downgrade
  auto [bw, bh] = base();
  int64_t now = clock_->CurrentTimeMs();
  if (ctx->last_resolution_change_ms > 0 &&
      now - ctx->last_resolution_change_ms < kDowngradeCooldownMs) {
    ctx->encode_exceed_count = 0;
    return;
  }

  auto [nw, nh] = resolution_adapter_
                      ? resolution_adapter_->GetNextLowerResolution(
                            bw, bh, ctx->source_width, ctx->source_height)
                      : std::pair<int, int>{-1, -1};
  if (nw <= 0 || nh <= 0) {
    ctx->encode_exceed_count = 0;
    return;
  }

  LOG_INFO("Resolution downgrade: channel={} {}x{} -> {}x{}", channel_name, bw,
           bh, nw, nh);
  ctx->target_width = nw;
  ctx->target_height = nh;
  ctx->last_resolution_change_ms = now;
  ctx->encode_exceed_count = 0;
  FullIntraRequest(channel_name);
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
    FullIntraRequest();
    return;
  }

  std::lock_guard<std::mutex> lock(force_i_frame_streams_mutex_);
  for (const auto& channel_name : channel_names) {
    force_i_frame_streams_.insert(channel_name);
  }
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
                                       encoded_frame.EncodedWidth(),
                                       encoded_frame.EncodedHeight());
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

int IceTransportController::SendAudio(const char* data, size_t size,
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

  int ret = context->codec->Encode(
      (uint8_t*)data, size,
      [this, channel_name, context](char* encoded_audio_buffer,
                                    size_t size) -> int {
        context->last_active_time = clock_->CurrentTimeMs();
        return context->transceiver->SendAudio(encoded_audio_buffer, size);
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
  if (controller_) {
    webrtc::NetworkAvailability msg;
    msg.at_time =
        webrtc::Timestamp::Millis(webrtc_clock_->TimeInMilliseconds());
    msg.network_available = network_available;
    controller_->OnNetworkAvailability(msg);
  }

  if (task_queue_pacer_) {
    task_queue_pacer_->PostTask([this]() mutable {
      if (paced_sender_) {
        paced_sender_->EnsureStarted();
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

          XVideoFrame x_video_frame;
          x_video_frame.data = (const char*)decoded_frame->Buffer();
          x_video_frame.width = decoded_frame->DecodedWidth();
          x_video_frame.height = decoded_frame->DecodedHeight();
          x_video_frame.size = decoded_frame->Size();
          x_video_frame.captured_timestamp = decoded_frame->CapturedTimestamp();
          x_video_frame.received_timestamp = decoded_frame->ReceivedTimestamp();
          x_video_frame.decoded_timestamp = decoded_frame->DecodedTimestamp();

          on_receive_video(&x_video_frame, remote_user_id.data(),
                           remote_user_id.size(), channel_name.data(),
                           channel_name.size(), user_data);
        });
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
  for (auto& [channel_name, context] : stream_senders_) {
    if (context) {
      sender_params.ssrc = context->ssrc.value_or(0);
      sender_params.receiver_any_inbound = false;
      ssrc_to_srtp_sender_[sender_params.ssrc] =
          SrtpEngine::CreateSenderPtr(sender_params);
    }
  }

  // setup SRTP receivers
  SrtpEngine::Params receiver_params;
  memcpy(receiver_params.key, remote_key_.data(), 16);
  memcpy(receiver_params.salt, remote_salt_.data(), 12);
  for (auto& [channel_name, context] : stream_receivers_) {
    if (context) {
      receiver_params.ssrc = context->ssrc.value_or(0);
      receiver_params.receiver_any_inbound = false;
      ssrc_to_srtp_receiver_[receiver_params.ssrc] =
          SrtpEngine::CreateReceiverPtr(receiver_params);
    }
  }
}

int IceTransportController::CreateStreamCodecs(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    bool av1_encoding) {
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
          context->codec = VideoEncoderFactory::CreateVideoEncoder(
              clock, hardware_acceleration, av1_encoding);
          if (!context->codec) {
            context->codec =
                VideoEncoderFactory::CreateVideoEncoder(clock, false, false);
            LOG_ERROR(
                "Create encoder for [{}] failed, try to use software H.264 "
                "encoder",
                channel_name);
          }
          if (!context->codec || 0 != context->codec->Init(media_config_)) {
            LOG_ERROR("Encoder [{}] init failed", channel_name);
            return -1;
          }
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
              clock, hardware_acceleration, av1_encoding);
          if (!context->codec) {
            context->codec =
                VideoDecoderFactory::CreateVideoDecoder(clock, false, false);
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
    if (hardware_acceleration_) {
      hardware_acceleration_ = false;
      LOG_WARN("Only support software codec for AV1");
    }
    ret = CreateStreamCodecs(clock, false, true);
  } else if (rtp::PAYLOAD_TYPE::H264 == video_pt) {
#if defined(__APPLE__)
    ret = CreateStreamCodecs(clock, hardware_acceleration_, false);
#elif USE_CUDA && !defined(__aarch64__) && !defined(__arm__)
    bool use_hardware = false;
    if (hardware_acceleration_ && LoadNvCodecDll() == 0) {
      load_nvcodec_dll_success_ = true;
      use_hardware = true;
    } else if (hardware_acceleration_) {
      LOG_WARN(
          "Hardware accelerated codec not available, use default software "
          "codec");
    }
    ret = CreateStreamCodecs(clock, use_hardware, false);
#else
    ret = CreateStreamCodecs(clock, false, false);
#endif
  }

  if (ret == 0) {
    video_codec_inited_ = true;
  }

  return ret;
}

void IceTransportController::OnSenderReport(const SenderReport& sender_report) {
  std::shared_lock lock(stream_receivers_mutex_);
  for (auto& [_, context] : stream_receivers_) {
    if (context && context->transceiver) {
      context->transceiver->OnSenderReport(sender_report);
    }
  }
}

void IceTransportController::OnReceiverReport(
    const std::vector<RtcpReportBlock>& report_block_datas) {
  webrtc::Timestamp now = webrtc_clock_->CurrentTime();
  if (report_block_datas.empty()) return;

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
    std::optional<webrtc::TransportPacketsFeedback> feedback_msg =
        transport_feedback_adapter_.ProcessCongestionControlFeedback(
            feedback, Timestamp::Micros(clock_->CurrentTimeUs()));
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
    const std::vector<uint16_t>& nack_sequence_numbers) {
  std::shared_lock lock(stream_senders_mutex_);
  for (auto& [_, context] : stream_senders_) {
    if (context && context->type == StreamType::kVideo &&
        context->transceiver) {
      context->transceiver->OnReceiveNack(nack_sequence_numbers);
    }
  }
}

void IceTransportController::OnSentPacket(
    const webrtc::RtpPacketToSend& packet) {
  task_queue_trans_fb_->PostTask([this, packet]() mutable {
    webrtc::PacedPacketInfo pacing_info;
    size_t transport_overhead_bytes_per_packet_ = 0;
    int64_t send_time_ms = clock_->CurrentTimeMs();
    webrtc::Timestamp creation_time = webrtc::Timestamp::Millis(send_time_ms);
    rtc::SentPacket sent_packet;
    const std::optional<int64_t> transport_seq =
        packet.transport_sequence_number();
    if (transport_seq.has_value()) {
      transport_feedback_adapter_.AddPacket(
          packet, pacing_info, transport_overhead_bytes_per_packet_,
          creation_time);
      sent_packet.packet_id = static_cast<int>(*transport_seq);
      sent_packet.info.included_in_feedback = true;
    } else {
      // Some packets (e.g. padding/FEC or other non-media packets) may not be
      // assigned a transport sequence number. Avoid crashing and avoid
      // polluting send history with a fake sequence number.
      LOG_WARN(
          "Sent packet without transport_sequence_number (ssrc={}, "
          "rtp_seq={}), "
          "falling back to untracked allocation.",
          packet.Ssrc(), packet.SequenceNumber());
      sent_packet.packet_id = -1;
      sent_packet.info.included_in_feedback = false;
    }
    sent_packet.send_time_ms = send_time_ms;
    sent_packet.info.included_in_allocation = true;
    sent_packet.info.packet_size_bytes = packet.size();
    sent_packet.info.packet_type = rtc::PacketType::kData;

    if (task_queue_cc_) {
      size_t packet_size = packet.size();
      webrtc::Timestamp sent_time = webrtc::Timestamp::Millis(send_time_ms);
      task_queue_cc_->PostTask([this, packet_size, sent_time]() mutable {
        if (controller_) {
          controller_->OnSentPacket(packet_size, sent_time);
        }
      });
    }

    transport_feedback_adapter_.ProcessSentPacket(sent_packet);
  });
}

void IceTransportController::PostUpdates(webrtc::NetworkControlUpdate update) {
  if (update.congestion_window) {
    congestion_window_size_ = *update.congestion_window;
    UpdateCongestedState();
  }

  if (update.pacer_config && task_queue_pacer_ && paced_sender_) {
    task_queue_pacer_->PostTask([this, update = std::move(update)]() mutable {
      paced_sender_->SetPacingRates(update.pacer_config->data_rate(),
                                    update.pacer_config->pad_rate());
    });
  }

  if (!update.probe_cluster_configs.empty() && task_queue_pacer_ &&
      paced_sender_) {
    task_queue_pacer_->PostTask([this, update = std::move(update)]() mutable {
      paced_sender_->CreateProbeClusters(
          std::move(update.probe_cluster_configs));
    });
  }

  if (update.target_rate) {
    available_transport_bitrate_ =
        update.target_rate.has_value()
            ? (update.target_rate->target_rate.bps() == 0
                   ? target_bitrate_
                   : update.target_rate->target_rate.bps())
            : target_bitrate_;
    std::shared_lock lock(stream_senders_mutex_);
    if (available_transport_bitrate_ != target_bitrate_ &&
        !stream_senders_.empty()) {
      target_bitrate_ = available_transport_bitrate_;

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

      if (video_count == 0 && data_count == 0) {
        return;
      }

      // Allocate bandwidth: reserve 10% for all data channels
      // The rest goes to video channels
      int64_t data_bitrate_total = 0;
      int64_t video_bitrate_total = available_transport_bitrate_;

      if (data_count > 0) {
        // All data channels together use 10% of total bandwidth
        data_bitrate_total =
            static_cast<int64_t>(available_transport_bitrate_ * 0.1);
        video_bitrate_total = available_transport_bitrate_ - data_bitrate_total;
      }

      // Allocate bandwidth to video channels
      if (video_count > 0) {
        int sub_target_bitrate = video_bitrate_total / video_count;
        bool freeze_resolution = false;
        if (update.target_rate.has_value()) {
          auto& ne = update.target_rate->network_estimate;
          freeze_resolution =
              ne.in_alr && ne.loss_rate_ratio <= 0.01f &&
              ne.round_trip_time <= webrtc::TimeDelta::Millis(40);
        }
        if (freeze_resolution) {
          LOG_INFO(
              "Freeze resolution due to ALR: target_bps={} rtt_ms={} loss={}",
              available_transport_bitrate_,
              update.target_rate->network_estimate.round_trip_time.ms(),
              update.target_rate->network_estimate.loss_rate_ratio);
        }
        for (auto& [channel_name, context] : stream_senders_) {
          if (context->codec && context->type == StreamType::kVideo) {
            if (freeze_resolution) {
              context->freeze_resolution = true;
              if (context->target_width.has_value() &&
                  context->target_height.has_value()) {
                LOG_INFO("Channel [{}] freeze: clear res_map {}x{}",
                         channel_name, context->target_width.value(),
                         context->target_height.value());
                context->target_width.reset();
                context->target_height.reset();
              }
            } else {
              context->freeze_resolution = false;
              int width, height, target_width, target_height;
              if (!context->codec->GetResolution(&width, &height)) {
                if (0 == resolution_adapter_->GetResolution(
                             sub_target_bitrate, width, height, &target_width,
                             &target_height)) {
                  // Bitrate mapping must be stable for 5 ticks
                  const int kStableThreshold = 5;
                  if (!context->pending_mapped_width.has_value() ||
                      !context->pending_mapped_height.has_value() ||
                      context->pending_mapped_width.value() != target_width ||
                      context->pending_mapped_height.value() != target_height) {
                    context->pending_mapped_width = target_width;
                    context->pending_mapped_height = target_height;
                    context->mapping_stability_count = 1;
                  } else {
                    context->mapping_stability_count += 1;
                  }
                  int64_t now_ms = clock_->CurrentTimeMs();
                  // 5s cooldown for bitrate-mapped resolution
                  const int kMinIntervalMs = 5000;
                  if (context->mapping_stability_count >= kStableThreshold &&
                      (now_ms - context->last_resolution_change_ms >=
                       kMinIntervalMs)) {
                    context->mapped_target_width = target_width;
                    context->mapped_target_height = target_height;
                    context->mapping_stability_count = 0;
                  }
                } else if (context->target_width.has_value() &&
                           context->target_height.has_value()) {
                  context->target_width.reset();
                  context->target_height.reset();
                }
              }
            }
            context->codec->SetTargetBitrate(sub_target_bitrate);
            // LOG_WARN("Set target bitrate [{}]bps", sub_target_bitrate);
          }
        }
      }
    }
    UpdateControlState();
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
  bool congested = transport_feedback_adapter_.GetOutstandingData() >=
                   congestion_window_size_;
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
