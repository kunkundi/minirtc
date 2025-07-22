#include "audio_channel_send.h"

#include "log.h"

namespace minirtc {

AudioChannelSend::AudioChannelSend() {}

AudioChannelSend::~AudioChannelSend() {}

AudioChannelSend::AudioChannelSend(
    const std::string &channel_name, std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics)
    : channel_name_(channel_name),
      ice_agent_(ice_agent),
      ice_io_statistics_(ice_io_statistics),
      rtp_audio_sender_(std::make_unique<RtpAudioSender>(ice_io_statistics)) {}

void AudioChannelSend::Initialize(rtp::PAYLOAD_TYPE payload_type,
                                  std::shared_ptr<PacedSender> packet_sender) {
  paced_sender_ = packet_sender;
  rtp_packetizer_ =
      RtpPacketizer::Create(payload_type, rtp_audio_sender_->GetSsrc());

  rtp_audio_sender_->SetSendDataFunc(
      [this](const char *data, size_t size) -> int {
        if (!ice_agent_) {
          LOG_ERROR("ice_agent_ is nullptr");
          return -1;
        }

        auto ice_state = ice_agent_->GetIceState();

        if (ICE_STATE_DESTROYED == ice_state) {
          return -2;
        }

        ice_io_statistics_->UpdateAudioOutboundBytes((uint32_t)size);
        return ice_agent_->Send(data, size);
      });

  rtp_audio_sender_->Start();
}

void AudioChannelSend::Destroy() {
  if (rtp_audio_sender_) {
    rtp_audio_sender_->Stop();
  }
}

int AudioChannelSend::SendAudio(char *data, size_t size) {
  if (rtp_audio_sender_ && rtp_packetizer_) {
    std::vector<std::unique_ptr<RtpPacket>> rtp_packets =
        rtp_packetizer_->Build((uint8_t *)data, (uint32_t)size, 0, true);
    // paced_sender_->EnqueueRtpPackets(rtp_packets, 0);
    rtp_audio_sender_->Enqueue(rtp_packets);
  }

  return 0;
}
}