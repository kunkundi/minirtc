#include "ice_transport_controller.h"

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
                                    bool hardware_acceleration,
                                    OnReceiveVideo on_receive_video,
                                    OnReceiveAudio on_receive_audio,
                                    OnReceiveData on_receive_data,
                                    void* user_data) {
  offer_peer_ = offer_peer;
  remote_user_id_ = remote_user_id;
  on_receive_video_ = on_receive_video;
  on_receive_audio_ = on_receive_audio;
  on_receive_data_ = on_receive_data;
  user_data_ = user_data;

  CreateCodecs(clock_, video_codec_payload_type, hardware_acceleration);

  if (enable_srtp_) {
    SrtpEngine::GlobalInit();
  }

  task_queue_cc_ = std::make_shared<TaskQueue>("congest control");
  task_queue_encode_ = std::make_shared<TaskQueueLockFree>("encode");
  task_queue_decode_ = std::make_shared<TaskQueueLockFree>("decode");
  task_queue_trans_fb_ =
      std::make_shared<TaskQueueLockFree>("transport feedback adapter");

  controller_ = std::make_unique<CongestionControl>();
  paced_sender_ =
      std::make_shared<PacedSender>(ice_agent_, webrtc_clock_, task_queue_cc_);
  paced_sender_->SetPacingRates(DataRate::BitsPerSec(300000), DataRate::Zero());
  paced_sender_->SetSendBurstInterval(TimeDelta::Millis(40));
  paced_sender_->SetQueueTimeLimit(TimeDelta::Millis(2000));
  std::weak_ptr<IceTransportController> weak_this = shared_from_this();
  paced_sender_->SetOnSentPacketFunc(
      [weak_this](std::unique_ptr<webrtc::RtpPacketToSend> packet) {
        if (auto self = weak_this.lock()) {
          if (self->ice_agent_) {
            self->last_active_stream_ = packet->get_stream_name();
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
                case webrtc::RtpPacketMediaType::kRetransmission: {
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
          if (self->stream_senders_.find(self->last_active_stream_) !=
              self->stream_senders_.end()) {
            return self->stream_senders_[self->last_active_stream_]
                ->transceiver->GeneratePadding(size, captured_timestamp_us);
          } else {
            return {};
          }
        } else {
          return {};
        }
      });

  resolution_adapter_ = std::make_unique<ResolutionAdapter>(video_quality_);

  {
    std::shared_lock lock(stream_senders_mutex_);
    for (auto& [_, context] : stream_senders_) {
      if (context) {
        if (context->type == StreamType::kVideo) {
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

  task_queue_cc_->Stop();
  task_queue_encode_->Stop();
  task_queue_decode_->Stop();
  task_queue_trans_fb_->Stop();

  {
    std::shared_lock lock(stream_senders_mutex_);
    for (auto& [_, context] : stream_senders_) {
      if (context) {
        if (context->type == StreamType::kVideo) {
          context->transceiver->Destroy();
        } else if (context->type == StreamType::kAudio) {
          context->transceiver->Destroy();
        } else if (context->type == StreamType::kData) {
          context->transceiver->Destroy();
        }
      }
    }
    stream_senders_.clear();
  }

  {
    std::shared_lock lock(stream_receivers_mutex_);
    for (auto& [_, context] : stream_receivers_) {
      if (context) {
        if (context->type == StreamType::kVideo) {
          context->transceiver->Destroy();
        } else if (context->type == StreamType::kAudio) {
          context->transceiver->Destroy();
        } else if (context->type == StreamType::kData) {
          context->transceiver->Destroy();
        }
      }
    }
    stream_receivers_.clear();
  }

  Stop();
}

uint32_t IceTransportController::AddVideoSendChannel(
    const std::string& channel_name) {
  std::shared_lock lock(stream_senders_mutex_);
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
  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it != stream_senders_.end() && !it->second) {
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
  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it != stream_senders_.end() && !it->second) {
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
  std::shared_lock lock(stream_receivers_mutex_);
  auto it = stream_receivers_.find(channel_name);
  if (it != stream_receivers_.end() && !it->second) {
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
  std::shared_lock lock(stream_receivers_mutex_);
  auto it = stream_receivers_.find(channel_name);
  if (it != stream_receivers_.end() && !it->second) {
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
  std::shared_lock lock(stream_receivers_mutex_);
  auto it = stream_receivers_.find(channel_name);
  if (it != stream_receivers_.end() && !it->second) {
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
  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) {
    LOG_ERROR("Failed to find stream sender [{}]", channel_name);
    return -1;
  }
  auto& context = it->second;
  if (!CheckSteamContext(channel_name, context)) {
    return -1;
  }

  if (b_force_i_frame_) {
    context->codec->ForceIdr();
    LOG_INFO("Force I frame");
    b_force_i_frame_ = false;
  }

  if (task_queue_encode_) {
    RawFrame raw_frame((const uint8_t*)video_frame->data, video_frame->size,
                       video_frame->width, video_frame->height);
    raw_frame.SetCapturedTimestamp(clock_->CurrentTimeUs());

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

      task_queue_encode_->PostTask([this,
                                    scaled_frame = std::move(scaled_frame),
                                    channel_name, context]() mutable {
        int ret = context->codec->Encode(
            std::move(scaled_frame),
            [this, channel_name,
             context](const EncodedFrame& encoded_frame) -> int {
              context->last_active_time = clock_->CurrentTimeMs();
              return context->transceiver->SendVideo(encoded_frame);
            });
      });
    } else {
      task_queue_encode_->PostTask([this, raw_frame = std::move(raw_frame),
                                    channel_name, context]() mutable {
        int ret = context->codec->Encode(
            std::move(raw_frame),
            [this, channel_name,
             context](const EncodedFrame& encoded_frame) -> int {
              context->last_active_time = clock_->CurrentTimeMs();
              return context->transceiver->SendVideo(encoded_frame);
            });
      });
    }
  }

  return 0;
}

int IceTransportController::SendAudio(const char* data, size_t size,
                                      const std::string& channel_name) {
  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) {
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
  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) {
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
  std::shared_lock lock(stream_senders_mutex_);
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) {
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

  if (paced_sender_) {
    paced_sender_->EnsureStarted();
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
  task_queue_decode_->PostTask([this,
                                received_frame = std::move(received_frame),
                                channel_name]() mutable {
    uint64_t t = clock_->CurrentTime();
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
          std::move(received_frame), [this](const DecodedFrame* decoded_frame) {
            if (on_receive_video_ && decoded_frame) {
              XVideoFrame x_video_frame;
              x_video_frame.data = (const char*)decoded_frame->Buffer();
              x_video_frame.width = decoded_frame->DecodedWidth();
              x_video_frame.height = decoded_frame->DecodedHeight();
              x_video_frame.size = decoded_frame->Size();
              x_video_frame.captured_timestamp =
                  decoded_frame->CapturedTimestamp();
              x_video_frame.received_timestamp =
                  decoded_frame->ReceivedTimestamp();
              x_video_frame.decoded_timestamp =
                  decoded_frame->DecodedTimestamp();

              if (on_receive_video_) {
                on_receive_video_(&x_video_frame, remote_user_id_.data(),
                                  remote_user_id_.size(), user_data_);
              }
            }
          });
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
        (uint8_t*)data, size, [this](uint8_t* data, int size) {
          if (on_receive_audio_) {
            on_receive_audio_((const char*)data, size, remote_user_id_.data(),
                              remote_user_id_.size(), user_data_);
          }
        });
  }
}

void IceTransportController::OnReceiveCompleteData(
    const char* data, size_t size, const std::string& channel_name) {
  if (on_receive_data_) {
    on_receive_data_(data, size, remote_user_id_.data(), remote_user_id_.size(),
                     user_data_);
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
    webrtc::Timestamp creation_time =
        webrtc::Timestamp::Millis(clock_->CurrentTimeMs());
    transport_feedback_adapter_.AddPacket(packet, pacing_info,
                                          transport_overhead_bytes_per_packet_,
                                          creation_time);

    rtc::SentPacket sent_packet;
    sent_packet.packet_id = packet.transport_sequence_number().value();
    sent_packet.send_time_ms = clock_->CurrentTimeMs();
    sent_packet.info.included_in_feedback = true;
    sent_packet.info.included_in_allocation = true;
    sent_packet.info.packet_size_bytes = packet.size();
    sent_packet.info.packet_type = rtc::PacketType::kData;

    transport_feedback_adapter_.ProcessSentPacket(sent_packet);
  });
}

void IceTransportController::PostUpdates(webrtc::NetworkControlUpdate update) {
  if (update.congestion_window) {
    congestion_window_size_ = *update.congestion_window;
    UpdateCongestedState();
  }

  if (update.pacer_config && paced_sender_) {
    paced_sender_->SetPacingRates(update.pacer_config->data_rate(),
                                  update.pacer_config->pad_rate());
  }

  if (!update.probe_cluster_configs.empty() && paced_sender_) {
    paced_sender_->CreateProbeClusters(std::move(update.probe_cluster_configs));
  }

  if (update.target_rate) {
    int target_bitrate = update.target_rate.has_value()
                             ? (update.target_rate->target_rate.bps() == 0
                                    ? target_bitrate_
                                    : update.target_rate->target_rate.bps())
                             : target_bitrate_;
    std::shared_lock lock(stream_senders_mutex_);
    if (target_bitrate != target_bitrate_ && !stream_senders_.empty()) {
      target_bitrate_ = target_bitrate;

      int count = 0;
      for (auto& [_, context] : stream_senders_) {
        if (context->last_active_time.has_value()) {
          if (clock_->CurrentTimeMs() - context->last_active_time.value() <
              100) {
            count++;
          }
        }
      }

      if (count == 0) {
        return;
      }

      int sub_target_bitrate = target_bitrate / count;
      for (auto& [channel_name, context] : stream_senders_) {
        if (context->codec && context->type == StreamType::kVideo) {
          int width, height, target_width, target_height;
          if (!context->codec->GetResolution(&width, &height)) {
            if (0 == resolution_adapter_->GetResolution(
                         sub_target_bitrate, width, height, &target_width,
                         &target_height)) {
              if (target_width != context->target_width ||
                  target_height != context->target_height) {
                context->target_width = target_width;
                context->target_height = target_height;
                b_force_i_frame_ = true;
              }
            } else if (context->target_width.has_value() &&
                       context->target_height.has_value()) {
              context->target_width.reset();
              context->target_height.reset();
            }
          }
          context->codec->SetTargetBitrate(sub_target_bitrate);
          // LOG_WARN("Set target bitrate [{}]bps", sub_target_bitrate);
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
    if (paced_sender_) {
      paced_sender_->SetCongested(update.value());
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