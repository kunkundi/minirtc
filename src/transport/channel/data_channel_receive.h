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
#include "rtp_data_sender.h"
#include "rtp_packetizer.h"
#include "thread_base.h"

namespace minirtc {

// Internal timer class to periodically update KCP (receive side)
class KcpUpdateTimerReceive : public ThreadBase {
 public:
  KcpUpdateTimerReceive(ikcpcb* kcp, const std::string& channel_name,
                        std::function<void()> on_update_callback)
      : kcp_(kcp),
        channel_name_(channel_name),
        on_update_callback_(on_update_callback) {
    SetPeriod(std::chrono::milliseconds(10));  // 10ms update interval
    SetThreadName("KcpUpdateRecv-" + channel_name);
  }

  bool Process() override {
    if (!kcp_) {
      return false;  // Stop timer if KCP is destroyed
    }

    // Use monotonic clock for KCP
    uint32_t now = GetCurrentTimeMs();

    // Use ikcp_check to avoid unnecessary updates
    uint32_t next_update = ikcp_check(kcp_, now);
    bool updated = false;
    if (now >= next_update) {
      ikcp_update(kcp_, now);
      updated = true;
    }

    // Always try to receive data, even if update wasn't needed
    if (on_update_callback_) {
      on_update_callback_();
    }

    return true;
  }

 private:
  static uint32_t GetCurrentTimeMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
            .count());
  }

  ikcpcb* kcp_;
  std::string channel_name_;
  std::function<void()> on_update_callback_;
};

class DataChannelReceive : public MediaChannel {
 public:
  DataChannelReceive();
  DataChannelReceive(const std::string& channel_name, uint32_t ssrc,
                     std::shared_ptr<IceAgent> ice_agent,
                     std::shared_ptr<IOStatistics> ice_io_statistics,
                     std::function<void(const char*, size_t)> on_receive_data,
                     bool use_reliable = false);
  virtual ~DataChannelReceive();

 public:
  void Initialize(rtp::PAYLOAD_TYPE payload_type);
  void Destroy();

  int OnReceiveRtpPacket(const char* data, size_t size);

  void OnSenderReport(const SenderReport& sender_report) {
    if (rtp_data_receiver_) {
      rtp_data_receiver_->OnSenderReport(sender_report);
    }
  }

 private:
  std::string channel_name_;
  uint32_t ssrc_ = 0;
  bool use_reliable_ = false;
  std::shared_ptr<IceAgent> ice_agent_ = nullptr;
  std::shared_ptr<IOStatistics> ice_io_statistics_ = nullptr;
  std::unique_ptr<RtpDataReceiver> rtp_data_receiver_ = nullptr;
  std::unique_ptr<RtpPacketizer> rtp_packetizer_ = nullptr;
  std::unique_ptr<RtpDataSender> rtp_data_sender_ = nullptr;
  std::function<void(const char*, size_t)> on_receive_data_ = nullptr;
  ikcpcb* kcp_ = nullptr;
  std::unique_ptr<KcpUpdateTimerReceive> kcp_update_timer_ = nullptr;

 private:
  bool InitKcp();
  int OnKcpOutput(const char* data, int len);
  static int KcpOutputCallback(const char* buf, int len, ikcpcb* kcp,
                               void* user);

  void TryReceiveKcpData();
};
}  // namespace minirtc

#endif