#include "datachannel_transport.h"

#include "log.h"
#include "nvcodec_api.h"

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
      offer_peer_(offer_peer) {
  task_queue_encode_ = std::make_shared<TaskQueueLockFree>("encode");
  task_queue_decode_ = std::make_shared<TaskQueueLockFree>("decode");
}

DataChannelTransport::~DataChannelTransport() {
  if (task_queue_encode_) {
    task_queue_encode_->Stop();
  }
  if (task_queue_decode_) {
    task_queue_decode_->Stop();
  }
  video_streams_.clear();
  audio_streams_.clear();
  data_streams_.clear();
}

int DataChannelTransport::SendVideoFrame(const XVideoFrame* video_frame,
                                         const std::string& stream_id) {
  for (auto& it : video_streams_) {
    if (it.first == stream_id) {
      auto& track = it.second->track_;
      auto& codec = it.second->codec_;

      if (!codec) {
        LoadNvCodecDll();
        codec = VideoEncoderFactory::CreateVideoEncoder(clock_, false, false);
        if (!codec || 0 != codec->Init()) {
          LOG_ERROR("Encoder [{}] init failed", stream_id);
          return -1;
        }
      }

      if (!task_queue_encode_) {
        LOG_ERROR("Encoder task queue not init");
        return -1;
      }

      RawFrame raw_frame((const uint8_t*)video_frame->data, video_frame->size,
                         video_frame->width, video_frame->height);
      raw_frame.SetCapturedTimestamp(clock_->CurrentTimeUs());

      std::weak_ptr<DataChannelTransport> weak_this = shared_from_this();
      auto track_ptr = track;
      auto codec_ptr = codec;

      task_queue_encode_->PostTask([weak_this, raw_frame = std::move(raw_frame),
                                    stream_id, track_ptr, codec_ptr]() mutable {
        auto self = weak_this.lock();
        if (!self) {
          LOG_ERROR("[{}] DataChannelTransport is released", self->local_id_);
          return -1;
        }

        if (track_ptr && track_ptr->isClosed()) {
          LOG_ERROR("[{}] Track is closed, drop raw frame size {}",
                    self->local_id_, raw_frame.Size());
          return -1;
        }

        int ret = codec_ptr->Encode(
            std::move(raw_frame),
            [weak_self = weak_this, stream_id,
             track_ptr](const EncodedFrame& encoded_frame) -> int {
              auto self2 = weak_self.lock();
              if (!self2) {
                LOG_ERROR("DataChannelTransport is released");
                return -1;
              }
              if (!track_ptr || !track_ptr->isOpen()) {
                LOG_ERROR("Track is closed, drop encoded frame size");
                return -1;
              }

              track_ptr->sendFrame(
                  reinterpret_cast<const std::byte*>(encoded_frame.Buffer()),
                  encoded_frame.Size(),
                  std::chrono::duration<double, std::micro>(
                      encoded_frame.EncodedTimestamp()));
              LOG_ERROR("Send video frame size {}", encoded_frame.Size());

              return 0;
            });
        return 0;
      });

      return 0;
    }
  }
  return -1;
}

int DataChannelTransport::SendAudioFrame(const char* data, size_t size,
                                         const std::string& stream_id) {
  for (auto& it : audio_streams_) {
    if (it.first == stream_id) {
      auto& track = it.second->track_;
      if (!track->isOpen()) {
        LOG_ERROR("[{}] Track is closed, drop audio frame size {}", local_id_,
                  size);
        break;
      }

      track->sendFrame(reinterpret_cast<const std::byte*>(data), size,
                       std::chrono::duration<double, std::micro>(90000));
      LOG_ERROR("[{}] Send audio frame size {}", local_id_, size);
      return 0;
    }
  }
  return -1;
}

int DataChannelTransport::SendDataFrame(const char* data, size_t size,
                                        const std::string& stream_id) {
  for (auto& it : data_streams_) {
    if (it.first == stream_id) {
      auto& data_channel = it.second;
      if (!data_channel->isOpen()) {
        LOG_ERROR("[{}] DataChannel is closed, drop data frame size", local_id_,
                  size);
        break;
      }

      data_channel->send(reinterpret_cast<const std::byte*>(data), size);
      LOG_ERROR("[{}] Send data frame size {}", local_id_, size);
      return 0;
    }
  }
  return -1;
}

}  // namespace minirtc