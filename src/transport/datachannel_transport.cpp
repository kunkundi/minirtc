#include "datachannel_transport.h"

#include <algorithm>
#include <future>
#include <sstream>
#include <vector>

#include "log.h"
#include "native_video_frame.h"
#include "nlohmann/json.hpp"
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
    : track_(std::move(track)), ssrc_(sender->rtpConfig->ssrc) {}

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
  media_handler_ = std::make_shared<WebMediaHandler>();
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
  video_quality_ = video_quality;
  // Use a conservative fallback until the controller negotiates live settings.
  media_config_.max_frame_rate =
      video_frame_rate > 0 ? std::min(video_frame_rate, 30) : 30;
  video_frame_rate_limit_ = media_config_.max_frame_rate;
  media_config_.video_content_type = video_content_type;
  media_config_.video_degradation_preference = video_degradation_preference;
  media_config_.init_bitrate = media_config_.average_bitrate =
      media_handler_->Budget().video_bps;
  media_config_.min_bitrate = 16000;
  media_config_.audio_fec_enabled = true;
  // Quality priority lets large desktop updates drain while capture is paused.
  media_handler_->SetFrameDeadline(
      video_degradation_preference ==
              VideoDegradationPreference::MaintainResolution
          ? 1000
          : 250);
  resolution_adapter_ = std::make_unique<ResolutionAdapter>(
      video_quality, media_config_.max_frame_rate, video_content_type,
      video_degradation_preference);
}

int DataChannelTransport::UpdateVideoSettings(
    VideoQuality quality, int frame_rate,
    VideoDegradationPreference preference) {
  if (quality < QualityLow || quality > QualityHigh ||
      (frame_rate != 30 && frame_rate != 60) ||
      preference > VideoDegradationPreference::Balanced)
    return -1;
  std::future<int> result;
  {
    std::lock_guard settings_lock(video_settings_mutex_);
    if (shutdown_ || !task_queue_encode_) return -1;
    auto task = std::make_shared<std::packaged_task<int()>>(
        [weak = weak_from_this(), quality, frame_rate, preference] {
          auto self = weak.lock();
          if (!self || self->shutdown_) return -1;
          const bool reconfigure_codec =
              self->media_config_.max_frame_rate != frame_rate ||
              self->media_config_.video_degradation_preference != preference;
          if (self->video_quality_ == quality && !reconfigure_codec) return 0;
          auto config = self->media_config_;
          config.max_frame_rate = frame_rate;
          config.video_degradation_preference = preference;
          // Prepare replacements atomically. A quality-only change affects the
          // resolution adapter and does not need to recreate any encoder.
          std::vector<
              std::pair<std::shared_ptr<Stream>, std::shared_ptr<MediaCodec>>>
              encoders;
          std::unique_lock lock(self->video_streams_mutex_);
          for (const auto& [id, stream] : self->video_streams_) {
            if (!stream || !stream->codec_) continue;
            auto codec = stream->codec_;
            if (reconfigure_codec) {
              auto encoder_config = config;
              if (stream->adapted_width_ > 0 && stream->adapted_height_ > 0) {
                encoder_config.init_width = stream->adapted_width_;
                encoder_config.init_height = stream->adapted_height_;
              }
              encoder_config.init_bitrate = encoder_config.average_bitrate =
                  self->media_handler_->Budget(stream->ssrc_).video_bps;
              codec = VideoEncoderFactory::CreateInitializedVideoEncoder(
                  self->clock_, encoder_config, self->hardware_acceleration_,
                  self->video_codec_type_);
              if (!codec) return -1;
            }
            encoders.emplace_back(stream, std::move(codec));
          }
          if (self->shutdown_) return -1;
          self->media_config_ = config;
          self->video_quality_ = quality;
          self->media_handler_->SetFrameDeadline(
              preference == VideoDegradationPreference::MaintainResolution
                  ? 1000
                  : 250);
          for (auto& [stream, codec] : encoders) {
            stream->codec_.swap(codec);
            stream->applied_bitrate_ = stream->requested_frame_rate_ = 0;
            stream->adapted_frame_rate_ = frame_rate;
            stream->adapted_width_ = stream->adapted_height_ = 0;
            std::lock_guard admission_lock(stream->video_admission_mutex_);
            stream->video_frame_cadence_ = VideoFrameCadence();
          }
          self->video_frame_rate_limit_ = frame_rate;
          lock.unlock();
          self->RequestAllVideoKeyFrames();
          // Release replaced encoders outside the stream lock.
          encoders.clear();
          LOG_INFO(
              "Web negotiated video settings: quality={} fps={} preference={}",
              static_cast<int>(quality), frame_rate,
              static_cast<int>(preference));
          return 0;
        });
    result = task->get_future();
    task_queue_encode_->PostTask([task] { (*task)(); });
  }
  // A stopped queue breaks the future; packaged_task also propagates failures
  // immediately instead of leaving the caller waiting on an unset promise.
  try {
    return result.get();
  } catch (const std::exception& error) {
    LOG_WARN("Could not apply web video settings: {}", error.what());
    return -1;
  }
}

void DataChannelTransport::Shutdown() {
  std::lock_guard settings_lock(video_settings_mutex_);
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
  if (media_handler_) media_handler_->Stop();
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
  if (!media_handler_->CanEncode(stream->ssrc_)) return 0;
  const auto budget = media_handler_->Budget(stream->ssrc_);
  // Shared capture can run at 60 fps; each controller negotiates its own cap.
  {
    std::lock_guard lock(stream->video_admission_mutex_);
    if (!stream->video_frame_cadence_.Accept(
            clock_->CurrentTimeUs(),
            std::min({video_frame_rate_limit_.load(), budget.max_fps,
                      stream->adapted_frame_rate_.load()}))) {
      return 0;
    }
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
  const bool should_force_i_frame =
      force_i_frame || force_stream_i_frame ||
      media_handler_->TakeKeyFrameRequest(stream->ssrc_);

  task_queue_encode_->PostTask([weak_self, weak_track, codec,
                                raw_frame = std::move(raw_frame), stream_id,
                                stream, should_force_i_frame]() mutable {
    auto self = weak_self.lock();
    if (!self) return -1;
    // A settings transaction may replace the encoder after frame admission.
    if (codec != stream->codec_) return 0;

    auto track_ptr = weak_track.lock();
    if (!track_ptr || track_ptr->isClosed()) {
      return -1;
    }

    const auto budget = self->media_handler_->Budget(stream->ssrc_);
    if (stream->applied_bitrate_ != budget.video_bps &&
        codec->SetTargetBitrate(budget.video_bps) == 0) {
      stream->applied_bitrate_ = budget.video_bps;
    }
    const auto now_ms = self->clock_->CurrentTimeMs();
    const bool keep_resolution =
        self->media_config_.video_degradation_preference ==
        VideoDegradationPreference::MaintainResolution;
    const int fps_limit =
        std::min(self->media_config_.max_frame_rate, budget.max_fps);
    // Match native quality priority: reduce temporal load under decoder/pacer
    // pressure instead of mapping a conservative bitrate to tiny desktop pixels.
    const int effective_fps = keep_resolution
        ? std::max(1, static_cast<int>(fps_limit * budget.decode_scale))
        : fps_limit;
    stream->adapted_frame_rate_ = effective_fps;
    if (stream->requested_frame_rate_ != effective_fps) {
      // Optional encoder hint: unsupported/failed settings must not be retried
      // on every frame. Admission still enforces FPS, regardless of support.
      stream->requested_frame_rate_ = effective_fps;
      codec->SetTargetFrameRate(effective_fps);
    }
    // The bandwidth mapping must use the admitted FPS, not capture's 60 fps.
    // Keep the negotiated preference intact. MaintainResolution already maps
    // to the source dimensions, bounded by the selected quality tier.
    self->resolution_adapter_->SetPreferences(
        self->video_quality_, effective_fps,
        self->media_config_.video_degradation_preference);
    int target_width = raw_frame.Width(), target_height = raw_frame.Height();
    self->resolution_adapter_->GetResolution(
        std::max(16000,
                 static_cast<int>(budget.video_bps * budget.decode_scale)),
        raw_frame.Width(), raw_frame.Height(), &target_width, &target_height);
    // Only repeated IDRs that cannot fit the bounded pacer may temporarily
    // reduce quality-priority pixels. Keep at least a readable 720p desktop,
    // rather than mapping a transient bitrate estimate straight down to 180p.
    const bool keyframe_recovery = keep_resolution && budget.keyframe_scale < 1;
    if (keyframe_recovery) {
      int floor_width = 0, floor_height = 0;
      self->resolution_adapter_->GetStartupResolution(
          raw_frame.Width(), raw_frame.Height(), &floor_width, &floor_height);
      const int64_t floor_pixels = int64_t(floor_width) * floor_height;
      const int64_t desired = std::max(floor_pixels, static_cast<int64_t>(
          target_width * int64_t(target_height) * budget.keyframe_scale));
      while (int64_t(target_width) * target_height > desired) {
        const auto next = self->resolution_adapter_->GetNextLowerResolution(
            target_width, target_height, raw_frame.Width(), raw_frame.Height());
        if (next.first <= 0 || next.second <= 0 ||
            int64_t(next.first) * next.second >= int64_t(target_width) * target_height)
          break;
        if (int64_t(next.first) * next.second < floor_pixels) {
          target_width = floor_width;
          target_height = floor_height;
          break;
        }
        target_width = next.first;
        target_height = next.second;
      }
    }
    const bool new_source =
        stream->source_width_ != static_cast<int>(raw_frame.Width()) ||
        stream->source_height_ != static_cast<int>(raw_frame.Height());
    if (new_source || !stream->adapted_width_) {
      // A conservative initial bandwidth estimate is not evidence that the
      // desktop needs 180p. Start with readable pixels; bounded keyframe/queue
      // recovery can still reduce them immediately on genuinely weak links.
      if (!keep_resolution)
        self->resolution_adapter_->GetStartupResolution(
            raw_frame.Width(), raw_frame.Height(), &target_width, &target_height);
      stream->source_started_ms_ = now_ms;
      stream->downscale_since_ms_ = -1;
    }
    const int64_t current_pixels =
        int64_t(stream->adapted_width_) * stream->adapted_height_;
    const int64_t desired_pixels = int64_t(target_width) * target_height;
    const bool downscale = desired_pixels < current_pixels * 0.75;
    const bool upscale = desired_pixels > current_pixels * 1.35;
    if (!downscale)
      stream->downscale_since_ms_ = -1;
    else if (stream->downscale_since_ms_ < 0)
      stream->downscale_since_ms_ = now_ms;
    const bool pipeline_pressure = !keep_resolution && budget.decode_scale < 0.95;
    const bool bandwidth_downscale =
        !keep_resolution && now_ms - stream->source_started_ms_ >= 5000 &&
        stream->downscale_since_ms_ >= 0 &&
        now_ms - stream->downscale_since_ms_ >= 2000;
    if (target_width > 0 && target_height > 0 &&
        (new_source || !current_pixels ||
         (downscale &&
          (pipeline_pressure || bandwidth_downscale || keyframe_recovery) &&
          now_ms - stream->last_resize_ms_ >= 1000) ||
         (upscale && now_ms - stream->last_resize_ms_ >= 5000))) {
      LOG_INFO(
          "Web video adaptation: peer={} stream={} {}x{} -> {}x{} "
          "video_bps={} fps={} scale={} reason={}",
          self->remote_id_, stream_id, stream->adapted_width_,
          stream->adapted_height_, target_width, target_height,
          budget.video_bps, effective_fps,
          budget.decode_scale,
          new_source || !current_pixels ? "startup"
          : upscale                     ? "bandwidth_recovery"
          : keyframe_recovery           ? "keyframe_budget"
          : pipeline_pressure           ? "pipeline_pressure"
                                        : "sustained_bandwidth");
      stream->adapted_width_ = target_width;
      stream->adapted_height_ = target_height;
      stream->source_width_ = raw_frame.Width();
      stream->source_height_ = raw_frame.Height();
      stream->last_resize_ms_ = now_ms;
    }
    const bool scale =
        stream->adapted_width_ > 0 && stream->adapted_height_ > 0 &&
        (stream->adapted_width_ != static_cast<int>(raw_frame.Width()) ||
         stream->adapted_height_ != static_cast<int>(raw_frame.Height()));
    if (const auto* native_frame = raw_frame.NativeFrame();
        native_frame &&
        (scale || !codec->SupportsNativeFrameInput(native_frame->type))) {
      if (!raw_frame.MaterializeNativeFrame()) {
        LOG_ERROR("[{}] Failed to materialize native video frame", stream_id);
        return -1;
      }
    }
    if (scale) {
      RawFrame scaled;
      if (self->resolution_adapter_->ResolutionDowngrade(
              raw_frame, stream->adapted_width_, stream->adapted_height_,
              scaled) != 0)
        return -1;
      scaled.SetCapturedTimestamp(raw_frame.CapturedTimestamp());
      raw_frame = std::move(scaled);
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

          try {
            track_shared->sendFrame(
                reinterpret_cast<const std::byte*>(encoded_frame.Buffer()),
                encoded_frame.Size(),
                std::chrono::duration<double, std::micro>(
                    encoded_frame.CapturedTimestamp()));
          } catch (const std::exception& error) {
            self_track->RequestVideoKeyFrame(stream_id);
            LOG_WARN("Web video send failed: {}", error.what());
            return -1;
          }
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
      [track, stream, media = media_handler_](
          char* encoded_audio_buffer, size_t encoded_size,
          uint32_t samples_per_channel) -> int {
        if (!track->isOpen()) {
          return -1;
        }

        const double timestamp_seconds =
            static_cast<double>(stream->audio_sample_count_) /
            ::rtc::OpusRtpPacketizer::DefaultClockRate;
        stream->audio_sample_count_ += samples_per_channel;
        // Preserve the media clock when stale audio cannot fit the link.
        if (!media->AdmitAudio(encoded_size)) return 0;
        track->sendFrame(
            reinterpret_cast<const std::byte*>(encoded_audio_buffer),
            encoded_size, std::chrono::duration<double>(timestamp_seconds));
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

void DataChannelTransport::ProcessBrowserFeedback(const std::string& message) {
  if (message.size() > 4096 || shutdown_.load()) return;
  const auto report = nlohmann::json::parse(message, nullptr, false);
  if (!report.is_object()) return;
  try {
    if (report.value("type", std::string()) != "web_qos" ||
        report.value("version", 0) != 1)
      return;
    const auto sequence = report.at("sequence").get<uint64_t>();
    if (!sequence || sequence > 9007199254740991ULL) return;
    std::lock_guard lock(feedback_mutex_);
    const auto now = clock_->CurrentTimeMs();
    if (sequence <= feedback_sequence_ ||
        (last_feedback_ms_ >= 0 && now - last_feedback_ms_ < 200))
      return;
    const int64_t expected = report.value("packets", int64_t{0});
    const int64_t lost = report.value("lost", int64_t{0});
    const int rtt_ms = report.value("rtt_ms", 0);
    const double jitter_ms = report.value("jitter_ms", 0.0);
    const double decode_ms = report.value("decode_ms", 0.0);
    const double fps = report.value("fps", 0.0);
    const bool frozen = report.value("frozen", false);
    if (!media_handler_->OnTelemetry(lost, expected, rtt_ms, jitter_ms,
                                     decode_ms, fps, frozen,
                                     report.value("received_bps", 0)))
      return;
    feedback_sequence_ = sequence;
    last_feedback_ms_ = now;
    if (last_receiver_log_ms_ < 0 || now - last_receiver_log_ms_ >= 5000) {
      ::rtc::Candidate local, remote;
      std::ostringstream local_route, remote_route;
      if (peer_connection_->getSelectedCandidatePair(&local, &remote)) {
        local_route << local.type() << '/' << local.transportType();
        remote_route << remote.type() << '/' << remote.transportType();
      }
      auto relay_protocol = report.value("relay_protocol", std::string());
      if (relay_protocol != "udp" && relay_protocol != "tcp" &&
          relay_protocol != "tls")
        relay_protocol = "unknown";
      LOG_DEBUG(
          "Web receiver: peer={} fps={} size={}x{} jitter_ms={} decode_ms={} "
          "frozen={} packets={} lost={} ice_local={} ice_remote={} "
          "browser_relay_protocol={}",
          remote_id_, fps, report.value("width", 0), report.value("height", 0),
          jitter_ms, decode_ms, frozen, expected, lost, local_route.str(),
          remote_route.str(), relay_protocol);
      last_receiver_log_ms_ = now;
    }
    std::shared_ptr<::rtc::DataChannel> channel;
    {
      std::shared_lock data_lock(data_streams_mutex_);
      const auto it = data_streams_.find("crossdesk-qos-v1");
      if (it != data_streams_.end()) channel = it->second;
    }
    if (channel && channel->isOpen() && channel->bufferedAmount() < 4096) {
      const auto budget = media_handler_->Budget();
      const int frame_rate_limit = video_frame_rate_limit_.load();
      const nlohmann::json status = {
          {"type", "web_qos_status"},
          {"version", 1},
          {"network_bps", budget.network_bps},
          {"video_bps", budget.video_bps},
          {"max_fps", std::min(frame_rate_limit, budget.max_fps)},
          {"frame_rate_limit", frame_rate_limit},
          {"queue_ms", media_handler_->QueueDelayMs()},
          {"decode_scale", budget.decode_scale},
          {"loss_ratio", budget.loss_ratio},
          {"rtt_ms", budget.rtt_ms}};
      try {
        channel->send(status.dump());
      } catch (const std::exception&) {
      }
    }
  } catch (const nlohmann::json::exception&) {
    // The receiver is remote input; malformed telemetry must not stop media.
  }
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

bool DataChannelTransport::IsControlChannelOpen() {
  std::shared_lock lock(data_streams_mutex_);
  const auto it = data_streams_.find("data-stream");
  return it != data_streams_.end() && it->second && it->second->isOpen();
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
#if defined(_WIN32) || defined(__APPLE__)
    ret = CreateStreamCodecs(clock, hardware_acceleration_,
                             VideoCodecType::H264);
#elif USE_CUDA && !defined(__aarch64__) && !defined(__arm__)
    ret = CreateStreamCodecs(clock, hardware_acceleration_, VideoCodecType::H264);
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
  video_codec_type_ = codec_type;
  hardware_acceleration_ = hardware_acceleration;
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
