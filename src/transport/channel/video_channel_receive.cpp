#include "video_channel_receive.h"

#include "log.h"
#include "rtp_packet.h"

namespace minirtc {

VideoChannelReceive::VideoChannelReceive() {}

VideoChannelReceive::VideoChannelReceive(
    const std::string& channel_name, uint32_t ssrc, uint32_t rtx_ssrc,
    std::shared_ptr<SystemClock> clock, std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics,
    std::function<void(std::unique_ptr<ReceivedFrame>)>
        on_receive_complete_frame)
    : channel_name_(channel_name),
      ssrc_(ssrc),
      rtx_ssrc_(rtx_ssrc),
      ice_agent_(ice_agent),
      ice_io_statistics_(ice_io_statistics),
      on_receive_complete_frame_(on_receive_complete_frame),
      clock_(clock) {}

VideoChannelReceive::~VideoChannelReceive() {}

void VideoChannelReceive::Initialize(rtp::PAYLOAD_TYPE payload_type) {
  rtp_video_receiver_ =
      std::make_unique<RtpVideoReceiver>(clock_, ice_io_statistics_);
  rtp_video_receiver_->SetMediaConfig(ssrc_, rtx_ssrc_, payload_type);
  rtp_video_receiver_->SetAbsoluteSendTimeExtensionId(
      abs_send_time_ext_id_);
  rtp_video_receiver_->SetOnReceiveCompleteFrame(
      [this](std::unique_ptr<ReceivedFrame> received_frame) -> void {
        on_receive_complete_frame_(std::move(received_frame));
      });

  rtp_video_receiver_->SetSendDataFunc([this](const char* data,
                                              size_t size) -> int {
    if (!ice_agent_) {
      LOG_ERROR("ice_agent_ is nullptr");
      return -1;
    }

    auto ice_state = ice_agent_->GetIceState();

    if (ICE_STATE_NULLPTR == ice_state || ICE_STATE_DESTROYED == ice_state) {
      LOG_ERROR("Ice is not connected, state = [{}]", (int)ice_state);
      return -2;
    }

    ice_io_statistics_->UpdateVideoOutboundBytes((uint32_t)size);
    return ice_agent_->Send(data, size);
  });

  rtp_video_receiver_->Start();
}

void VideoChannelReceive::SetAbsoluteSendTimeExtensionId(
    std::optional<uint8_t> extension_id) {
  abs_send_time_ext_id_ = extension_id;
  if (rtp_video_receiver_) {
    rtp_video_receiver_->SetAbsoluteSendTimeExtensionId(extension_id);
  }
}

void VideoChannelReceive::Destroy() {
  if (rtp_video_receiver_) {
    rtp_video_receiver_->StopRtcp();
    rtp_video_receiver_->Stop();
  }
}

int VideoChannelReceive::OnReceiveRtpPacket(const char* data, size_t size) {
  if (ice_io_statistics_) {
    ice_io_statistics_->UpdateVideoInboundBytes((uint32_t)size);
  }

  if (rtp_video_receiver_) {
    if (size < kFixedHeaderSize) {
      LOG_ERROR("Received RTP packet is too small, size={}", size);
      return -1;
    }
    RtpPacket rtp_packet;
    if (!rtp_packet.Build((uint8_t*)data, (uint32_t)size)) {
      return -1;
    }
    rtp_video_receiver_->InsertRtpPacket(rtp_packet);
  }

  return 0;
}

void VideoChannelReceive::RequestKeyFrame() {
  if (rtp_video_receiver_) {
    rtp_video_receiver_->RequestKeyFrame();
  }
}

void VideoChannelReceive::OnRttUpdate(int64_t rtt_ms) {
  if (rtp_video_receiver_) {
    rtp_video_receiver_->OnRttUpdate(rtt_ms);
  }
}
}  // namespace minirtc
