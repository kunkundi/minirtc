/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-03
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _AUDIO_CHANNEL_SEND_H_
#define _AUDIO_CHANNEL_SEND_H_

#include "ice_agent.h"
#include "media_channel.h"
#include "paced_sender.h"
#include "rtp_audio_sender.h"
#include "rtp_packetizer.h"

namespace minirtc {

class AudioChannelSend : public MediaChannel {
 public:
  AudioChannelSend();
  AudioChannelSend(const std::string& channel_name,
                   std::shared_ptr<IceAgent> ice_agent,
                   std::shared_ptr<IOStatistics> ice_io_statistics);
  virtual ~AudioChannelSend();

 public:
  void Initialize(rtp::PAYLOAD_TYPE payload_type,
                  std::shared_ptr<PacedSender> packet_sender);
  void Destroy();

  uint32_t GetSsrc() {
    if (rtp_audio_sender_) {
      return rtp_audio_sender_->GetSsrc();
    }
    return 0;
  }

  int SendAudio(char* data, size_t size);

  void OnReceiverReport(const ReceiverReport& receiver_report) {}

 private:
  std::string channel_name_;
  std::shared_ptr<PacedSender> paced_sender_ = nullptr;
  std::shared_ptr<IceAgent> ice_agent_ = nullptr;
  std::shared_ptr<IOStatistics> ice_io_statistics_ = nullptr;
  std::unique_ptr<RtpPacketizer> rtp_packetizer_ = nullptr;
  std::unique_ptr<RtpAudioSender> rtp_audio_sender_ = nullptr;
};
}  // namespace minirtc

#endif