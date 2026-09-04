#include "datachannel_transport.h"

#include <vector>

#include "log.h"
#include "native_video_frame.h"
#include "resolution_adapter.h"
#include "video_frame_wrapper.h"

#if defined(__APPLE__)
#if USE_CUDA
#pragma message("Warning: CUDA is ignored on macOS.")
#endif
#elif USE_CUDA && !defined(__aarch64__) && !defined(__arm__)
#include "nvcodec_api.h"
#endif

namespace minirtc {

Stream::Stream(std::shared_ptr<::rtc::Track> track,
               std::shared_ptr<::rtc::RtcpSrReporter> sender)
    : track_(track), sender_(sender) {}

DataChannelTransport::DataChannelTransport(
    std::shared_ptr<SystemClock> clock,
    std::shared_ptr<::rtc::PeerConnection> peer_connection,
    std::string local_id, std::string remote_id, bool offer_peer)
    : clock_(clock),
      peer_connection_(peer_connection),
      local_id_(local_id),
      remote_id_(remote_id),
      offer_peer_(offer_peer),
      b_force_i_frame_(true),
      video_codec_inited_(false),
      audio_codec_inited_(false),
      hardware_acceleration_(false) {
  task_queue_encode_ = std::make_shared<TaskQueueLockFree>("encode");
  task_queue_decode_ = std::make_shared<TaskQueueLockFree>("decode");
}

DataChannelTransport::~DataChannelTransport() {
  Shutdown();

  {
    std::unique_lock lock_video(video_streams_mutex_);
    video_streams_.clear();
  }

  {
    std::unique_lock lock_audio(audio_streams_mutex_);
    audio_streams_.clear();
  }

  {
    std::unique_lock lock_data(data_streams_mutex_);
    data_streams_.clear();
  }

  video_codec_inited_ = false;
  audio_codec_inited_ = false;
}

void DataChannelTransport::SetVideoConfig(
    VideoQuality video_quality, int video_frame_rate,
    VideoContentType video_content_type,
    VideoDegradationPreference video_degradation_preference) {
  media_config_.max_frame_rate = video_frame_rate == 30 ? 30 : 60;
  media_config_.video_content_type = video_content_type;
  media_config_.video_degradation_preference = video_degradation_preference;
  resolution_adapter_ = std::make_unique<ResolutionAdapter>(
      video_quality, media_config_.max_frame_rate, video_content_type,
      video_degradation_preference);
}

void DataChannelTransport::Shutdown() {
  bool expected = false;
  if (!shutdown_.compare_exchange_strong(expected, true)) {
    return;
  }

  if (task_queue_encode_) {
    task_queue_encode_->Stop();
    task_queue_encode_.reset();
  }
  if (task_queue_decode_) {
    task_queue_decode_->Stop();
    task_queue_decode_.reset();
  }
}

int DataChannelTransport::SendVideoFrame(const MiniRtcVideoFrame* video_frame,
                                         const std::string& stream_id) {
  const MiniRtcNativeVideoFrame* native_frame =
      GetNativeVideoFrameInput(video_frame);
  size_t required_cpu_size = 0;
  const bool valid_cpu_frame =
      video_frame &&
      GetNv12FrameSize(video_frame->width, video_frame->height,
                       &required_cpu_size) &&
      video_frame->data && video_frame->size >= required_cpu_size;
  if (!native_frame && !valid_cpu_frame) {
    LOG_ERROR("Invalid video frame for stream [{}]", stream_id);
    return -1;
  }

  std::shared_ptr<Stream> stream;
  std::shared_ptr<MediaCodec> codec;
  std::shared_ptr<::rtc::Track> track;

  {
    std::shared_lock lock(video_streams_mutex_);
    auto it_stream = video_streams_.find(stream_id);
    if (it_stream == video_streams_.end()) {
      LOG_ERROR("Video stream [{}] not found", stream_id);
      return -1;
    }

    stream = it_stream->second;
    if (!stream) {
      LOG_ERROR("[{}] Stream is null", stream_id);
      return -1;
    }

    codec = stream->codec_;
    track = stream->track_;
  }

  if (!codec) {
    LOG_ERROR("[{}] Codec not found", stream_id);
    return -1;
  }
  if (!track) {
    LOG_ERROR("[{}] Track not found", stream_id);
    return -1;
  }
  if (!task_queue_encode_) {
    LOG_ERROR("Encoder task queue not init");
    return -1;
  }
  if (task_queue_encode_->PendingTasks() > 0) {
    return 0;
  }

  RawFrame raw_frame = native_frame
                           ? RawFrame(*native_frame)
                           : RawFrame(reinterpret_cast<const uint8_t*>(
                                          video_frame->data),
                                      video_frame->size, video_frame->width,
                                      video_frame->height);
  raw_frame.SetCapturedTimestamp(
      video_frame->captured_timestamp != 0
          ? static_cast<int64_t>(video_frame->captured_timestamp)
          : clock_->CurrentTimeUs());
  const bool force_i_frame = b_force_i_frame_.exchange(false);
  bool force_stream_i_frame = false;
  {
    std::lock_guard<std::mutex> lock(force_i_frame_streams_mutex_);
    auto it_force = force_i_frame_streams_.find(stream_id);
    if (it_force != force_i_frame_streams_.end()) {
      force_stream_i_frame = true;
      force_i_frame_streams_.erase(it_force);
    }
  }

  std::weak_ptr<DataChannelTransport> weak_self = shared_from_this();
  std::weak_ptr<::rtc::Track> weak_track = track;
  const bool should_force_i_frame = force_i_frame || force_stream_i_frame;

  task_queue_encode_->PostTask([weak_self, weak_track, codec,
                                raw_frame = std::move(raw_frame),
                                stream_id, should_force_i_frame]() mutable {
    auto self = weak_self.lock();
    if (!self) return -1;

    auto track_ptr = weak_track.lock();
    if (!track_ptr || track_ptr->isClosed()) {
      return -1;
    }

    if (!codec) {
      LOG_WARN("[{}] Codec is null, drop frame", stream_id);
      return -1;
    }

    if (const auto* native_frame = raw_frame.NativeFrame();
        native_frame &&
        !codec->SupportsNativeFrameInput(native_frame->type)) {
      if (!raw_frame.MaterializeNativeFrame()) {
        LOG_ERROR("[{}] Failed to materialize native video frame", stream_id);
        return -1;
      }
    }

    std::shared_ptr<::rtc::Track> track_shared = track_ptr;

    if (should_force_i_frame) {
      codec->ForceIdr();
      LOG_INFO("Force I frame");
    }

    codec->Encode(
        std::move(raw_frame),
        [weak_self, track_shared,
         stream_id](const EncodedFrame& encoded_frame) -> int {
          auto self_track = weak_self.lock();
          if (!self_track) return -1;

          if (!track_shared || !track_shared->isOpen()) {
            LOG_WARN("[{}] Track closed, drop encoded frame size {}", stream_id,
                     encoded_frame.Size());
            return -1;
          }

          track_shared->sendFrame(
              reinterpret_cast<const std::byte*>(encoded_frame.Buffer()),
              encoded_frame.Size(),
              std::chrono::duration<double, std::micro>(
                  encoded_frame.EncodedTimestamp()));
          return 0;
        });
    return 0;
  });
  return 0;
}

int DataChannelTransport::SendAudioFrame(const MiniRtcAudioFrame* audio_frame,
                                         const std::string& stream_id) {
  if (!audio_frame || !audio_frame->data || audio_frame->size == 0) {
    LOG_ERROR("Invalid audio frame for stream [{}]", stream_id);
    return -1;
  }

  std::shared_ptr<Stream> stream;
  std::shared_ptr<MediaCodec> codec;
  std::shared_ptr<::rtc::Track> track;
  {
    std::shared_lock lock(audio_streams_mutex_);
    auto it = audio_streams_.find(stream_id);
    if (it == audio_streams_.end()) {
      LOG_ERROR("Audio stream [{}] not found", stream_id);
      return -1;
    }
    stream = it->second;
    if (!stream) {
      LOG_ERROR("[{}] Audio stream is null", stream_id);
      return -1;
    }

    codec = stream->codec_;
    track = stream->track_;
  }

  if (!codec) {
    LOG_ERROR("[{}] Audio codec not found", stream_id);
    return -1;
  }
  if (!track) {
    LOG_ERROR("[{}] Audio track not found", stream_id);
    return -1;
  }
  if (!track->isOpen()) {
    LOG_ERROR("[{}] Track is closed, drop audio frame size {}", local_id_,
              audio_frame->size);
    return -1;
  }

  std::lock_guard<std::mutex> encode_lock(stream->audio_encode_mutex_);
  const int ret = codec->Encode(
      reinterpret_cast<const uint8_t*>(audio_frame->data), audio_frame->size,
      [track, stream](char* encoded_audio_buffer, size_t encoded_size,
                      uint32_t samples_per_channel) -> int {
        if (!track->isOpen()) {
          return -1;
        }

        const double timestamp_seconds =
            static_cast<double>(stream->audio_sample_count_) /
            ::rtc::OpusRtpPacketizer::DefaultClockRate;
        track->sendFrame(
            reinterpret_cast<const std::byte*>(encoded_audio_buffer),
            encoded_size, std::chrono::duration<double>(timestamp_seconds));
        stream->audio_sample_count_ += samples_per_channel;
        return 0;
      });
  return ret;
}

int DataChannelTransport::SendDataFrame(const char* data, size_t size,
                                        const std::string& stream_id) {
  std::shared_ptr<::rtc::DataChannel> data_channel;
  {
    std::shared_lock lock(data_streams_mutex_);
    auto it = data_streams_.find(stream_id);
    if (it == data_streams_.end()) {
      return -1;
    }
    data_channel = it->second;
  }

  if (!data_channel) {
    return -1;
  }

  if (!data_channel->isOpen()) {
    LOG_ERROR("[{}] DataChannel is closed, drop data frame size", local_id_,
              size);
    return -1;
  }

  data_channel->send(reinterpret_cast<const std::byte*>(data), size);
  // LOG_ERROR("[{}] Send data frame size {}", local_id_, size);
  return 0;
}

void DataChannelTransport::AddVideoStream(std::string stream_id,
                                          std::shared_ptr<Stream> stream) {
  std::unique_lock lock(video_streams_mutex_);
  video_streams_.emplace(stream_id, stream);
}

void DataChannelTransport::RequestAllVideoKeyFrames() {
  std::vector<std::string> stream_ids;
  {
    std::shared_lock lock(video_streams_mutex_);
    for (const auto& video_stream : video_streams_) {
      if (video_stream.second) {
        stream_ids.push_back(video_stream.first);
      }
    }
  }

  if (stream_ids.empty()) {
    b_force_i_frame_ = true;
    return;
  }

  std::lock_guard<std::mutex> lock(force_i_frame_streams_mutex_);
  for (const auto& stream_id : stream_ids) {
    force_i_frame_streams_.insert(stream_id);
  }
}

void DataChannelTransport::AddAudioStream(std::string stream_id,
                                          std::shared_ptr<Stream> stream) {
  std::unique_lock lock(audio_streams_mutex_);
  audio_streams_.emplace(stream_id, stream);
}

void DataChannelTransport::AddDataStream(
    std::string stream_id, std::shared_ptr<::rtc::DataChannel> stream) {
  std::unique_lock lock(data_streams_mutex_);
  data_streams_.emplace(stream_id, stream);
}

int DataChannelTransport::CreateCodecs(std::shared_ptr<SystemClock> clock,
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
    ret = CreateStreamCodecs(clock, false, VideoCodecType::AV1);
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

int DataChannelTransport::CreateStreamCodecs(std::shared_ptr<SystemClock> clock,
                                             bool hardware_acceleration,
                                             VideoCodecType codec_type) {
  bool video_streams_init_first_time = true;
  bool audio_streams_init_first_time = true;

  {
    std::shared_lock video_lock(video_streams_mutex_);
    for (auto& [stream_id, stream] : video_streams_) {
      if (!stream) {
        LOG_ERROR("Failed to find video stream [{}]", stream_id);
        return -1;
      }

      if (!stream->codec_) {
        stream->codec_ = VideoEncoderFactory::CreateInitializedVideoEncoder(
            clock, media_config_, hardware_acceleration, codec_type);
        if (!stream->codec_) {
          LOG_ERROR("Create and initialize encoder for [{}] failed", stream_id);
          return -1;
        }
        if (video_streams_init_first_time) {
          if (!video_streams_.empty()) {
            LOG_INFO("Use video encoder [{}]",
                     stream->codec_->GetEncoderName());
            video_streams_init_first_time = false;
          }
        }
      }
    }
  }

  {
    std::shared_lock audio_lock(audio_streams_mutex_);
    for (auto& [stream_id, stream] : audio_streams_) {
      if (!stream) {
        LOG_ERROR("Failed to find audio stream [{}]", stream_id);
        return -1;
      }

      if (!stream->codec_) {
        stream->codec_ = std::make_shared<AudioEncoder>(48000, 1, 480);
        if (!stream->codec_ || 0 != stream->codec_->Init(media_config_)) {
          LOG_ERROR("Audio encoder [{}] init failed", stream_id);
          return -1;
        }
        if (audio_streams_init_first_time) {
          LOG_INFO("Use audio encoder [{}]", stream->codec_->GetEncoderName());
          audio_streams_init_first_time = false;
        }
      }
    }
  }

  return 0;
}

}  // namespace minirtc
