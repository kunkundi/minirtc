/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-03
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DATA_CHANNEL_SEND_H_
#define _DATA_CHANNEL_SEND_H_

#include "ice_agent.h"
#include "ikcp.h"
#include "media_channel.h"
#include "paced_sender.h"
#include "rtp_data_sender.h"
#include "rtp_packetizer.h"
#include "thread_base.h"

namespace minirtc {

// Internal timer class to periodically update KCP
class KcpUpdateTimer : public ThreadBase {
 public:
  KcpUpdateTimer(ikcpcb* kcp, const std::string& channel_name)
      : kcp_(kcp), channel_name_(channel_name) {
    SetPeriod(std::chrono::milliseconds(10));  // 10ms update interval
    SetThreadName("KcpUpdate-" + channel_name);
  }

  bool Process() override {
    if (!kcp_) {
      return false;
    }

    // Use monotonic clock for KCP
    uint32_t now = GetCurrentTimeMs();

    // Use ikcp_check to avoid unnecessary updates
    uint32_t next_update = ikcp_check(kcp_, now);
    if (now >= next_update) {
      ikcp_update(kcp_, now);
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
};

class DataChannelSend : public MediaChannel {
 public:
  DataChannelSend();
  DataChannelSend(const std::string& channel_name,
                  std::shared_ptr<IceAgent> ice_agent,
                  std::shared_ptr<IOStatistics> ice_io_statistics,
                  bool use_reliable = false);
  virtual ~DataChannelSend();

 public:
  void Initialize(rtp::PAYLOAD_TYPE payload_type,
                  std::shared_ptr<PacedSender> packet_sender) override;
  void Destroy();

  uint32_t GetSsrc() {
    if (rtp_data_sender_) {
      return rtp_data_sender_->GetSsrc();
    }
    return 0;
  }

  int SendData(const char* data, size_t size) override;

  int SendReliableData(const char* data, size_t size) override;

  void SetTargetBitrate(int64_t target_bitrate_bps);

  void OnReceiverReport(const ReceiverReport& receiver_report) {}

  int OnReceiveRtpPacket(const char* data, size_t size);

 private:
  std::string channel_name_;
  bool use_reliable_ = false;
  std::shared_ptr<PacedSender> paced_sender_ = nullptr;
  std::shared_ptr<IceAgent> ice_agent_ = nullptr;
  std::shared_ptr<IOStatistics> ice_io_statistics_ = nullptr;
  std::unique_ptr<RtpPacketizer> rtp_packetizer_ = nullptr;
  std::unique_ptr<RtpDataSender> rtp_data_sender_ = nullptr;
  ikcpcb* kcp_ = nullptr;
  std::unique_ptr<KcpUpdateTimer> kcp_update_timer_ = nullptr;

 private:
  bool InitKcp();
  int OnKcpOutput(const char* data, int len);
  static int KcpOutputCallback(const char* buf, int len, ikcpcb* kcp,
                               void* user);
};
}  // namespace minirtc

#endif