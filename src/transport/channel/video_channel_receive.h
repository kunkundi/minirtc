/*
 * @Author: DI JUNKUN
 * @Date: 2025-01-03
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_CHANNEL_RECEIVE_H_
#define _VIDEO_CHANNEL_RECEIVE_H_

#include "clock/system_clock.h"
#include "ice_agent.h"
#include "media_channel.h"
#include "rtp_video_receiver.h"

namespace minirtc {

class VideoChannelReceive : public MediaChannel {
 public:
  VideoChannelReceive();
  VideoChannelReceive(const std::string &channel_name, uint32_t ssrc,
                      uint32_t rtx_ssrc,
                      std::shared_ptr<SystemClock> clock,
                      std::shared_ptr<IceAgent> ice_agent,
                      std::shared_ptr<IOStatistics> ice_io_statistics,
                      std::function<void(std::unique_ptr<ReceivedFrame>)>
                          on_receive_complete_frame);

  virtual ~VideoChannelReceive();

 public:
  void Initialize(rtp::PAYLOAD_TYPE payload_type) override;
  void Destroy() override;

  void SetAbsoluteSendTimeExtensionId(
      std::optional<uint8_t> extension_id) override;

  int OnReceiveRtpPacket(const char *data, size_t size) override;

  void RequestKeyFrame() override;

  void OnRttUpdate(int64_t rtt_ms) override;

  void OnSenderReport(const SenderReport &sender_report) override {
    if (rtp_video_receiver_) {
      rtp_video_receiver_->OnSenderReport(sender_report);
    }
  }

 private:
  std::string channel_name_;
  uint32_t ssrc_ = 0;
  uint32_t rtx_ssrc_ = 0;
  std::shared_ptr<IceAgent> ice_agent_ = nullptr;
  std::shared_ptr<IOStatistics> ice_io_statistics_ = nullptr;
  std::unique_ptr<RtpVideoReceiver> rtp_video_receiver_ = nullptr;
  std::optional<uint8_t> abs_send_time_ext_id_;
  std::function<void(std::unique_ptr<ReceivedFrame>)>
      on_receive_complete_frame_ = nullptr;

 private:
  std::shared_ptr<SystemClock> clock_;
};
}  // namespace minirtc

#endif
