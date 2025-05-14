/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-03
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DATA_CHANNEL_SEND_H_
#define _DATA_CHANNEL_SEND_H_

#include "ice_agent.h"
#include "media_channel.h"
#include "paced_sender.h"
#include "rtp_data_sender.h"
#include "rtp_packetizer.h"

class DataChannelSend : public MediaChannel {
 public:
  DataChannelSend();
  DataChannelSend(const std::string& channel_name,
                  std::shared_ptr<IceAgent> ice_agent,
                  std::shared_ptr<IOStatistics> ice_io_statistics);
  virtual ~DataChannelSend();

 public:
  void Initialize(rtp::PAYLOAD_TYPE payload_type,
                  std::shared_ptr<PacedSender> packet_sender);
  void Destroy();

  uint32_t GetSsrc() {
    if (rtp_data_sender_) {
      return rtp_data_sender_->GetSsrc();
    }
    return 0;
  }

  int SendData(const char* data, size_t size);

  void OnReceiverReport(const ReceiverReport& receiver_report) {}

 private:
  std::string channel_name_;
  std::shared_ptr<PacedSender> paced_sender_ = nullptr;
  std::shared_ptr<IceAgent> ice_agent_ = nullptr;
  std::shared_ptr<IOStatistics> ice_io_statistics_ = nullptr;
  std::unique_ptr<RtpPacketizer> rtp_packetizer_ = nullptr;
  std::unique_ptr<RtpDataSender> rtp_data_sender_ = nullptr;
};

#endif