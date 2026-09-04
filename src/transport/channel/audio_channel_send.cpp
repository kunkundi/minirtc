#include "audio_channel_send.h"

#include "common.h"
#include "log.h"

namespace minirtc {

AudioChannelSend::AudioChannelSend()
    : rtp_timestamp_generator_(GenerateRandomRtpTimestamp()) {}

AudioChannelSend::~AudioChannelSend() {}

AudioChannelSend::AudioChannelSend(
    const std::string &channel_name, std::shared_ptr<SystemClock> clock,
    std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics)
    : channel_name_(channel_name),
      ice_agent_(ice_agent),
      ice_io_statistics_(ice_io_statistics),
      rtp_audio_sender_(
          std::make_unique<RtpAudioSender>(clock, ice_io_statistics)),
      rtp_timestamp_generator_(GenerateRandomRtpTimestamp()) {}

void AudioChannelSend::Initialize(rtp::PAYLOAD_TYPE payload_type,
                                  std::shared_ptr<PacedSender> packet_sender) {
  paced_sender_ = packet_sender;
  rtp_packetizer_ =
      RtpPacketizer::Create(payload_type, rtp_audio_sender_->GetSsrc());
  rtp_packetizer_->SetAbsoluteSendTimeExtensionId(
      abs_send_time_ext_id_);
  rtp_audio_sender_->SetAbsoluteSendTimeExtensionId(
      abs_send_time_ext_id_);

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

void AudioChannelSend::SetAbsoluteSendTimeExtensionId(
    std::optional<uint8_t> extension_id) {
  abs_send_time_ext_id_ = extension_id;
  if (rtp_packetizer_) {
    rtp_packetizer_->SetAbsoluteSendTimeExtensionId(extension_id);
  }
  if (rtp_audio_sender_) {
    rtp_audio_sender_->SetAbsoluteSendTimeExtensionId(extension_id);
  }
}

void AudioChannelSend::Destroy() {
  if (rtp_audio_sender_) {
    rtp_audio_sender_->Stop();
  }
}

int AudioChannelSend::SendAudio(char* data, size_t size,
                                uint32_t samples_per_channel,
                                int64_t captured_timestamp_us) {
  if (rtp_audio_sender_ && rtp_packetizer_) {
    const auto timestamp_sample = rtp_timestamp_generator_.NextTimestamp(
        samples_per_channel, captured_timestamp_us);
    std::vector<std::unique_ptr<RtpPacket>> rtp_packets =
        rtp_packetizer_->Build((uint8_t *)data, (uint32_t)size,
                               timestamp_sample.rtp_timestamp, true);
    // paced_sender_->EnqueueRtpPackets(rtp_packets, 0);
    rtp_audio_sender_->Enqueue(rtp_packets, timestamp_sample.media_time_us);
  }

  return 0;
}
}
