#include "ice_transport_controller.h"

#include "video_frame_wrapper.h"
#if __APPLE__
#else
#include "nvcodec_api.h"
#endif

#include "api/transport/network_types.h"

IceTransportController::IceTransportController(
    std::shared_ptr<SystemClock> clock, std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics)
    : clock_(clock),
      ice_agent_(ice_agent),
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
    task_queue_cc_->ClearTasks();
  }
  if (task_queue_encode_) {
    task_queue_encode_->ClearTasks();
  }
  if (task_queue_decode_) {
    task_queue_decode_->ClearTasks();
  }

  user_data_ = nullptr;
  video_codec_inited_ = false;
  audio_codec_inited_ = false;
  load_nvcodec_dll_success_ = false;

#ifdef __APPLE__
#else
  if (hardware_acceleration_ && load_nvcodec_dll_success_) {
    ReleaseNvCodecDll();
  }
#endif
}

void IceTransportController::Create(std::string remote_user_id,
                                    rtp::PAYLOAD_TYPE video_codec_payload_type,
                                    bool hardware_acceleration,
                                    OnReceiveVideo on_receive_video,
                                    OnReceiveAudio on_receive_audio,
                                    OnReceiveData on_receive_data,
                                    void* user_data) {
  remote_user_id_ = remote_user_id;
  on_receive_video_ = on_receive_video;
  on_receive_audio_ = on_receive_audio;
  on_receive_data_ = on_receive_data;
  user_data_ = user_data;

  CreateCodecs(clock_, video_codec_payload_type, hardware_acceleration);

  task_queue_cc_ = std::make_shared<TaskQueue>("congest control");
  task_queue_encode_ = std::make_shared<TaskQueue>("encode");
  task_queue_decode_ = std::make_shared<TaskQueue>("decode");

  controller_ = std::make_unique<CongestionControl>();
  paced_sender_ =
      std::make_shared<PacedSender>(ice_agent_, webrtc_clock_, task_queue_cc_);
  paced_sender_->SetPacingRates(DataRate::BitsPerSec(300000), DataRate::Zero());
  paced_sender_->SetSendBurstInterval(TimeDelta::Millis(40));
  paced_sender_->SetQueueTimeLimit(TimeDelta::Millis(2000));
  paced_sender_->SetOnSentPacketFunc(
      [this](std::unique_ptr<webrtc::RtpPacketToSend> packet) {
        if (ice_agent_) {
          last_active_stream_ = packet->get_stream_name();
          webrtc::Timestamp now = webrtc_clock_->CurrentTime();
          ice_agent_->Send((const char*)packet->Buffer().data(),
                           packet->Size());
          OnSentPacket(*packet);

          if (packet->packet_type().has_value()) {
            switch (packet->packet_type().value()) {
              case webrtc::RtpPacketMediaType::kVideo:
              case webrtc::RtpPacketMediaType::kRetransmission:
                if (stream_senders_.find(last_active_stream_) !=
                    stream_senders_.end()) {
                  stream_senders_[last_active_stream_]
                      ->transceiver->OnSentRtpPacket(std::move(packet));
                }
                break;
              default:
                break;
            }
          }
        }
      });

  paced_sender_->SetGeneratePaddingFunc(
      [this](uint32_t size, int64_t captured_timestamp_us)
          -> std::vector<std::unique_ptr<RtpPacket>> {
        if (stream_senders_.find(last_active_stream_) !=
            stream_senders_.end()) {
          return stream_senders_[last_active_stream_]
              ->transceiver->GeneratePadding(size, captured_timestamp_us);
        } else {
          return {};
        }
      });

  resolution_adapter_ = std::make_unique<ResolutionAdapter>();

  for (auto& [_, context] : stream_senders_) {
    if (context) {
      if (context->type == StreamType::kVideo) {
        context->transceiver->Initialize(video_codec_payload_type,
                                         paced_sender_);
      } else if (context->type == StreamType::kAudio) {
        context->transceiver->Initialize(rtp::PAYLOAD_TYPE::OPUS,
                                         paced_sender_);
      } else if (context->type == StreamType::kData) {
        context->transceiver->Initialize(rtp::PAYLOAD_TYPE::DATA,
                                         paced_sender_);
      }
    }
  }

  for (auto& [_, context] : stream_receivers_) {
    if (context) {
      if (context->type == StreamType::kVideo) {
        context->transceiver->Initialize(video_codec_payload_type);
      } else if (context->type == StreamType::kAudio) {
        context->transceiver->Initialize(rtp::PAYLOAD_TYPE::OPUS);
      } else if (context->type == StreamType::kData) {
        context->transceiver->Initialize(rtp::PAYLOAD_TYPE::DATA);
      }
    }
  }
}

void IceTransportController::Destroy() {
  is_running_.store(false);

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

  stream_senders_.clear();
  stream_receivers_.clear();

  Stop();
}

uint32_t IceTransportController::AddVideoSendChannel(
    const std::string& channel_name) {
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
    const std::string& channel_name) {
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
  }
  if (!context->transceiver) {
    context->transceiver = std::make_shared<DataChannelSend>(
        channel_name, ice_agent_, ice_io_statistics_);
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
    const std::string& channel_name, uint32_t ssrc) {
  auto it = stream_receivers_.find(channel_name);
  if (it == stream_receivers_.end() || !it->second) {
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
        });
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
    auto video_frame_copy = std::make_shared<XVideoFrame>(*video_frame);
    task_queue_encode_->PostTask(
        [this, video_frame_copy, channel_name, context]() mutable {
          XVideoFrame new_frame;
          new_frame.data = nullptr;
          new_frame.width = video_frame_copy->width;
          new_frame.height = video_frame_copy->height;
          new_frame.size = video_frame_copy->size;
          new_frame.captured_timestamp = video_frame_copy->captured_timestamp;
          if (context->target_width.has_value() &&
              context->target_height.has_value() &&
              context->target_width.value() < video_frame_copy->width &&
              context->target_height.value() < video_frame_copy->height) {
            resolution_adapter_->ResolutionDowngrade(
                video_frame_copy.get(), context->target_width.value(),
                context->target_height.value(), &new_frame);
          } else {
            new_frame.data = new char[video_frame_copy->size];
            memcpy((void*)new_frame.data, video_frame_copy->data,
                   video_frame_copy->size);
          }

          RawFrame raw_frame((const uint8_t*)new_frame.data, new_frame.size,
                             new_frame.width, new_frame.height);
          raw_frame.SetCapturedTimestamp(video_frame_copy->captured_timestamp);
          delete[] new_frame.data;

          int ret = context->codec->Encode(
              std::move(raw_frame),
              [this, channel_name,
               context](const EncodedFrame& encoded_frame) -> int {
                return context->transceiver->SendVideo(encoded_frame);
              });
        });
  }

  return 0;
}

int IceTransportController::SendAudio(const char* data, size_t size,
                                      const std::string& channel_name) {
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
        return context->transceiver->SendAudio(encoded_audio_buffer, size);
      });

  return ret;
}

int IceTransportController::SendData(const char* data, size_t size,
                                     const std::string& channel_name) {
  auto it = stream_senders_.find(channel_name);
  if (it == stream_senders_.end() || !it->second) {
    LOG_ERROR("Failed to find stream sender [{}]", channel_name);
    return -1;
  }
  auto& context = it->second;
  if (!CheckSteamContext(channel_name, context)) {
    return -1;
  }

  return context->transceiver->SendData(data, size);

  return 0;
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

int IceTransportController::OnReceiveVideoRtpPacket(const char* data,
                                                    size_t size,
                                                    uint32_t ssrc) {
  if (ssrc_to_name_.find(ssrc) != ssrc_to_name_.end()) {
    std::string channel_name = ssrc_to_name_[ssrc];
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
    if (stream_receivers_.find(channel_name) != stream_receivers_.end()) {
      return stream_receivers_[channel_name]->transceiver->OnReceiveRtpPacket(
          data, size);
    }
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

int IceTransportController::CreateStreamCodecs(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    bool av1_encoding) {
  bool video_sender_first_time = true;
  bool audio_sender_first_time = true;
  bool video_receiver_first_time = true;
  bool audio_receiver_first_time = true;
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
        if (!context->codec || 0 != context->codec->Init()) {
          LOG_ERROR("Encoder [{}] init failed", channel_name);
          return -1;
        }
        if (video_sender_first_time) {
          if (!stream_senders_.empty()) {
            LOG_INFO("Use video encoder [{}]",
                     context->codec->GetEncoderName());
            video_sender_first_time = false;
          }
        }
      }
    } else if (context->type == StreamType::kAudio) {
      if (!context->codec) {
        context->codec =
            std::make_shared<AudioEncoder>(AudioEncoder(48000, 1, 480));
        if (!context->codec || 0 != context->codec->Init()) {
          LOG_ERROR("Audio encoder [{}] init failed", channel_name);
          return -1;
        }
        if (audio_receiver_first_time) {
          LOG_INFO("Use audio encoder [{}]", context->codec->GetEncoderName());
          audio_receiver_first_time = false;
        }
      }
    }
  }

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
        if (video_receiver_first_time) {
          LOG_INFO("Use video decoder [{}]", context->codec->GetEncoderName());
        }
      } else if (context->type == StreamType::kAudio) {
        context->codec =
            std::make_shared<AudioDecoder>(AudioDecoder(48000, 1, 480));
        if (!context->codec || 0 != context->codec->Init()) {
          LOG_ERROR("Audio decoder [{}] init failed", channel_name);
          return -1;
        }
        if (audio_receiver_first_time) {
          LOG_INFO("Create audio decoder [{}] finish",
                   context->codec->GetDecoderName());
          audio_receiver_first_time = false;
        }
      }
    }
  }

  if (!stream_receivers_.empty()) {
    LOG_INFO("Use video decoder [{}]",
             stream_receivers_.begin()->second->codec->GetEncoderName());
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
#ifdef __APPLE__
    if (hardware_acceleration_) {
      hardware_acceleration_ = false;
      LOG_WARN(
          "MacOS not support hardware acceleration, use default software "
          "codec");
    } else {
    }
    ret = CreateStreamCodecs(clock, false, false);
#else
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
#endif
  }

  return ret;
}

void IceTransportController::OnSenderReport(const SenderReport& sender_report) {
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
}

void IceTransportController::OnReceiveNack(
    const std::vector<uint16_t>& nack_sequence_numbers) {
  for (auto& [_, context] : stream_senders_) {
    if (context && context->type == StreamType::kVideo &&
        context->transceiver) {
      context->transceiver->OnReceiveNack(nack_sequence_numbers);
    }
  }
}

void IceTransportController::OnSentPacket(
    const webrtc::RtpPacketToSend& packet) {
  webrtc::PacedPacketInfo pacing_info;
  size_t transport_overhead_bytes_per_packet_ = 0;
  webrtc::Timestamp creation_time =
      webrtc::Timestamp::Millis(clock_->CurrentTimeMs());
  transport_feedback_adapter_.AddPacket(
      packet, pacing_info, transport_overhead_bytes_per_packet_, creation_time);

  rtc::SentPacket sent_packet;
  sent_packet.packet_id = packet.transport_sequence_number().value();
  sent_packet.send_time_ms = clock_->CurrentTimeMs();
  sent_packet.info.included_in_feedback = true;
  sent_packet.info.included_in_allocation = true;
  sent_packet.info.packet_size_bytes = packet.size();
  sent_packet.info.packet_type = rtc::PacketType::kData;

  transport_feedback_adapter_.ProcessSentPacket(sent_packet);
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

    if (target_bitrate != target_bitrate_ && !stream_senders_.empty()) {
      target_bitrate_ = target_bitrate;
      int sub_target_bitrate = target_bitrate / stream_senders_.size();
      for (auto& [channel_name, context] : stream_senders_) {
        if (context->codec) {
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