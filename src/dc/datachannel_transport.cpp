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
}

int DataChannelTransport::SendVideoFrame(const XVideoFrame *video_frame,
                                         const std::string &stream_id) {
  for (auto &it : video_streams_) {
    if (it.first == stream_id) {
      auto &track = it.second->track_;
      auto &codec = it.second->codec_;

      if (!codec) {
        LoadNvCodecDll();
        codec = VideoEncoderFactory::CreateVideoEncoder(clock_, true, false);
        if (!codec || 0 != codec->Init()) {
          LOG_ERROR("Encoder [{}] init failed", stream_id);
          return -1;
        }
      }

      if (task_queue_encode_) {
        RawFrame raw_frame((const uint8_t *)video_frame->data,
                           video_frame->size, video_frame->width,
                           video_frame->height);
        raw_frame.SetCapturedTimestamp(clock_->CurrentTimeUs());

        task_queue_encode_->PostTask([this, raw_frame = std::move(raw_frame),
                                      stream_id, track, codec]() mutable {
          int ret = codec->Encode(
              std::move(raw_frame),
              [this, stream_id,
               track](const EncodedFrame &encoded_frame) -> int {
                track->sendFrame(
                    reinterpret_cast<const std::byte *>(encoded_frame.Buffer()),
                    encoded_frame.Size(),
                    std::chrono::duration<double, std::micro>(90000));
                LOG_ERROR("[{}] Send video frame size {}", local_id_,
                          encoded_frame.Size());

                return 0;
              });
        });
      }
      return 0;
    }
  }
  return -1;
}

int DataChannelTransport::SendAudioFrame(const char *data, size_t size,
                                         const std::string &stream_id) {
  for (auto &it : audio_streams_) {
    if (it.first == stream_id) {
      auto &track = it.second->track_;
      track->sendFrame(reinterpret_cast<const std::byte *>(data), size,
                       std::chrono::duration<double, std::micro>(90000));
      LOG_ERROR("[{}] Send audio frame size {}", local_id_, size);
      return 0;
    }
  }
  return -1;
}

int DataChannelTransport::SendDataFrame(const char *data, size_t size,
                                        const std::string &stream_id) {
  for (auto &it : data_streams_) {
    if (it.first == stream_id) {
      auto &data_channel = it.second;
      data_channel->send(reinterpret_cast<const std::byte *>(data), size);
      LOG_ERROR("[{}] Send data frame size {}", local_id_, size);
      return 0;
    }
  }
  return -1;
}

}  // namespace minirtc