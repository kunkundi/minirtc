/*
 * @Author: DI JUNKUN
 * @Date: 2025-05-14
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _MEDIA_CHANNEL_H_
#define _MEDIA_CHANNEL_H_

#include <memory>
#include <vector>

#include "encoded_frame.h"
#include "log.h"
#include "paced_sender.h"
#include "receiver_report.h"
#include "sender_report.h"

namespace minirtc {

class MediaChannel {
 public:
  MediaChannel() {}
  virtual ~MediaChannel() {}

 public:
  virtual void Initialize(rtp::PAYLOAD_TYPE payload_type,
                          std::shared_ptr<PacedSender> packet_sender) {
    LOG_INFO("Initialize() default implementation");
  }

  virtual void Initialize(rtp::PAYLOAD_TYPE payload_type) {
    LOG_INFO("Initialize() default implementation");
  }

  virtual void Destroy() { LOG_INFO("Destroy() default implementation"); }

  virtual uint32_t GetSsrc() {
    LOG_INFO("GetSsrc() default implementation");
    return 0;
  }

  virtual uint32_t GetRtxSsrc() {
    LOG_INFO("GetRtxSsrc() default implementation");
    return 0;
  }

  virtual int SendVideo(const EncodedFrame& encoded_frame) {
    LOG_INFO("SendVideo() default implementation");
    return 0;
  }

  virtual int SendAudio(char* data, size_t size,
                        uint32_t samples_per_channel) {
    LOG_INFO("SendAudio() default implementation");
    return 0;
  }

  virtual int SendData(const char* data, size_t size) {
    LOG_INFO("SendData() default implementation");
    return 0;
  }

  virtual int SendReliableData(const char* data, size_t size) {
    LOG_INFO("SendReliableData() default implementation");
    return 0;
  }

  virtual void OnReceiverReport(const ReceiverReport& receiver_report) {
    LOG_INFO("OnReceiverReport() default implementation");
  }

  virtual void OnRttUpdate(int64_t rtt_ms) {}

  virtual int OnReceiveRtpPacket(const char* data, size_t size) {
    LOG_INFO("OnReceiveRtpPacket() default implementation");
    return 0;
  }

  virtual void OnSenderReport(const SenderReport& sender_report) {
    LOG_INFO("OnSenderReport() default implementation");
  }

  virtual void OnSentRtpPacket(
      std::unique_ptr<webrtc::RtpPacketToSend> packet) {
    LOG_INFO("OnSentRtpPacket() default implementation");
  }

  virtual void OnRtpPacketSendFailed(
      const webrtc::RtpPacketToSend& packet) {}

  virtual void OnReceiveNack(
      const std::vector<uint16_t>& nack_sequence_numbers) {
    LOG_INFO("OnReceiveNack() default implementation");
  }

  virtual void RequestKeyFrame() {
    LOG_INFO("RequestKeyFrame() default implementation");
  }

  virtual std::vector<std::unique_ptr<RtpPacket>> GeneratePadding(
      uint32_t payload_size, int64_t padding_time_us) {
    LOG_INFO("GeneratePadding() default implementation");
    return {};
  }

  virtual bool CanGeneratePadding() const { return false; }
};
}  // namespace minirtc

#endif
