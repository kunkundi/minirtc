/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-03
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DATA_CHANNEL_RECEIVE_H_
#define _DATA_CHANNEL_RECEIVE_H_

#include "ice_agent.h"
#include "ikcp.h"
#include "media_channel.h"
#include "rtp_data_receiver.h"

namespace minirtc {

class DataChannelReceive : public MediaChannel {
 public:
  DataChannelReceive();
  DataChannelReceive(const std::string &channel_name, uint32_t ssrc,
                     std::shared_ptr<IceAgent> ice_agent,
                     std::shared_ptr<IOStatistics> ice_io_statistics,
                     std::function<void(const char *, size_t)> on_receive_data);
  virtual ~DataChannelReceive();

 public:
  void Initialize(rtp::PAYLOAD_TYPE payload_type);
  void Destroy();

  int OnReceiveRtpPacket(const char *data, size_t size);

  void OnSenderReport(const SenderReport &sender_report) {
    if (rtp_data_receiver_) {
      rtp_data_receiver_->OnSenderReport(sender_report);
    }
  }

 private:
  std::string channel_name_;
  uint32_t ssrc_ = 0;
  std::shared_ptr<IceAgent> ice_agent_ = nullptr;
  std::shared_ptr<IOStatistics> ice_io_statistics_ = nullptr;
  std::unique_ptr<RtpDataReceiver> rtp_data_receiver_ = nullptr;
  std::function<void(const char *, size_t)> on_receive_data_ = nullptr;
  ikcpcb *kcp_ = nullptr;

 private:
  bool InitKcp();
  int OnKcpOutput(const char *data, int len);
  static int KcpOutputCallback(const char *buf, int len, ikcpcb *kcp,
                               void *user);
};
}  // namespace minirtc

#endif