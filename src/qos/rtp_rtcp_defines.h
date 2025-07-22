/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_RTP_RTCP_INCLUDE_RTP_RTCP_DEFINES_H_
#define MODULES_RTP_RTCP_INCLUDE_RTP_RTCP_DEFINES_H_

#include <stddef.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "api/array_view.h"
#include "api/rtp_rtcp/rtp_rtcp_typedef.h"
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "congestion_control_feedback.h"
#include "report_block_data.h"

namespace minirtc {
class RtpPacket;
namespace webrtc {
class RtpPacketToSend;

struct RtpState {
  uint16_t sequence_number = 0;
  uint32_t start_timestamp = 0;
  uint32_t timestamp = 0;
  Timestamp capture_time = Timestamp::MinusInfinity();
  Timestamp last_timestamp_time = Timestamp::MinusInfinity();
  bool ssrc_has_acked = false;
};

class RtcpIntraFrameObserver {
 public:
  virtual ~RtcpIntraFrameObserver() {}

  virtual void OnReceivedIntraFrameRequest(uint32_t ssrc) = 0;
};

// Observer for incoming LossNotification RTCP messages.
// See the documentation of LossNotification for details.
class RtcpLossNotificationObserver {
 public:
  virtual ~RtcpLossNotificationObserver() = default;

  virtual void OnReceivedLossNotification(uint32_t ssrc,
                                          uint16_t seq_num_of_last_decodable,
                                          uint16_t seq_num_of_last_received,
                                          bool decodability_flag) = 0;
};

// Interface to watch incoming rtcp packets related to the link in general.
// All message handlers have default empty implementation. This way users only
// need to implement the ones they are interested in.
// All message handles pass `receive_time` parameter, which is receive time
// of the rtcp packet that triggered the update.
class NetworkLinkRtcpObserver {
 public:
  virtual ~NetworkLinkRtcpObserver() = default;

  // RFC 8888 congestion control feedback.
  virtual void OnCongestionControlFeedback(
      Timestamp /* receive_time */,
      const rtcp::CongestionControlFeedback& /* feedback */) {}
  virtual void OnReceiverEstimatedMaxBitrate(Timestamp /* receive_time */,
                                             DataRate /* bitrate */) {}

  // Called on an RTCP packet with sender or receiver reports with non zero
  // report blocks. Report blocks are combined from all reports into one array.
  virtual void OnReport(
      Timestamp /* receive_time */,
      rtc::ArrayView<const ReportBlockData> /* report_blocks */) {}
  virtual void OnRttUpdate(Timestamp /* receive_time */, TimeDelta /* rtt */) {}
};

struct RtpPacketSendInfo {
  static RtpPacketSendInfo From(const RtpPacketToSend& rtp_packet_to_send,
                                const PacedPacketInfo& pacing_info);

  uint16_t transport_sequence_number = 0;
  std::optional<uint32_t> media_ssrc;
  uint16_t rtp_sequence_number = 0;  // Only valid if `media_ssrc` is set.
  uint32_t rtp_timestamp = 0;
  size_t length = 0;
  std::optional<RtpPacketMediaType> packet_type;
  PacedPacketInfo pacing_info;
};

class NetworkStateEstimateObserver {
 public:
  virtual void OnRemoteNetworkEstimate(NetworkStateEstimate estimate) = 0;
  virtual ~NetworkStateEstimateObserver() = default;
};

class TransportFeedbackObserver {
 public:
  virtual ~TransportFeedbackObserver() = default;

  virtual void OnAddPacket(const RtpPacketSendInfo& packet_info) = 0;
};

// Interface for PacketRouter to send rtcp feedback on behalf of
// congestion controller.
// TODO(bugs.webrtc.org/8239): Remove and use RtcpTransceiver directly
// when RtcpTransceiver always present in rtp transport.
class RtcpFeedbackSenderInterface {
 public:
  virtual ~RtcpFeedbackSenderInterface() = default;
  virtual void SendCombinedRtcpPacket(
      std::vector<std::unique_ptr<RtcpPacket>> rtcp_packets) = 0;
  virtual void SetRemb(int64_t bitrate_bps, std::vector<uint32_t> ssrcs) = 0;
  virtual void UnsetRemb() = 0;
};

class StreamFeedbackObserver {
 public:
  struct StreamPacketInfo {
    bool received;

    // `rtp_sequence_number` and `is_retransmission` are only valid if `ssrc`
    // is populated.
    std::optional<uint32_t> ssrc;
    uint16_t rtp_sequence_number;
    bool is_retransmission;
  };
  virtual ~StreamFeedbackObserver() = default;

  virtual void OnPacketFeedbackVector(
      std::vector<StreamPacketInfo> packet_feedback_vector) = 0;
};

class StreamFeedbackProvider {
 public:
  virtual void RegisterStreamFeedbackObserver(
      std::vector<uint32_t> ssrcs, StreamFeedbackObserver* observer) = 0;
  virtual void DeRegisterStreamFeedbackObserver(
      StreamFeedbackObserver* observer) = 0;
  virtual ~StreamFeedbackProvider() = default;
};

class RtcpRttStats {
 public:
  virtual void OnRttUpdate(int64_t rtt) = 0;

  virtual int64_t LastProcessedRtt() const = 0;

  virtual ~RtcpRttStats() {}
};

struct RtpPacketCounter {
  RtpPacketCounter()
      : header_bytes(0), payload_bytes(0), padding_bytes(0), packets(0) {}

  explicit RtpPacketCounter(const minirtc::RtpPacket& packet);
  explicit RtpPacketCounter(const RtpPacketToSend& packet_to_send);

  void Add(const RtpPacketCounter& other) {
    header_bytes += other.header_bytes;
    payload_bytes += other.payload_bytes;
    padding_bytes += other.padding_bytes;
    packets += other.packets;
    total_packet_delay += other.total_packet_delay;
  }

  bool operator==(const RtpPacketCounter& other) const {
    return header_bytes == other.header_bytes &&
           payload_bytes == other.payload_bytes &&
           padding_bytes == other.padding_bytes && packets == other.packets &&
           total_packet_delay == other.total_packet_delay;
  }

  // Not inlined, since use of minirtc::RtpPacket would result in circular
  // includes.
  void AddPacket(const minirtc::RtpPacket& packet);
  void AddPacket(const RtpPacketToSend& packet_to_send);

  size_t TotalBytes() const {
    return header_bytes + payload_bytes + padding_bytes;
  }

  size_t header_bytes;   // Number of bytes used by RTP headers.
  size_t payload_bytes;  // Payload bytes, excluding RTP headers and padding.
  size_t padding_bytes;  // Number of padding bytes.
  size_t packets;        // Number of packets.
  // The total delay of all `packets`. For RtpPacketToSend packets, this is
  // `time_in_send_queue()`. For receive packets, this is zero.
  webrtc::TimeDelta total_packet_delay = webrtc::TimeDelta::Zero();
};

// Data usage statistics for a (rtp) stream.
struct StreamDataCounters {
  StreamDataCounters();

  void Add(const StreamDataCounters& other) {
    transmitted.Add(other.transmitted);
    retransmitted.Add(other.retransmitted);
    fec.Add(other.fec);
    if (other.first_packet_time < first_packet_time) {
      // Use oldest time (excluding unsed value represented as plus infinity.
      first_packet_time = other.first_packet_time;
    }
  }

  void MaybeSetFirstPacketTime(Timestamp now) {
    if (first_packet_time == Timestamp::PlusInfinity()) {
      first_packet_time = now;
    }
  }

  // Return time since first packet is send/received, or zero if such event
  // haven't happen.
  TimeDelta TimeSinceFirstPacket(Timestamp now) const {
    return first_packet_time == Timestamp::PlusInfinity()
               ? TimeDelta::Zero()
               : now - first_packet_time;
  }

  // Returns the number of bytes corresponding to the actual media payload (i.e.
  // RTP headers, padding, retransmissions and fec packets are excluded).
  // Note this function does not have meaning for an RTX stream.
  size_t MediaPayloadBytes() const {
    return transmitted.payload_bytes - retransmitted.payload_bytes -
           fec.payload_bytes;
  }

  // Time when first packet is sent/received.
  Timestamp first_packet_time = Timestamp::PlusInfinity();

  RtpPacketCounter transmitted;    // Number of transmitted packets/bytes.
  RtpPacketCounter retransmitted;  // Number of retransmitted packets/bytes.
  RtpPacketCounter fec;            // Number of redundancy packets/bytes.
};

class RtpSendRates {
 public:
  constexpr RtpSendRates() = default;
  RtpSendRates(const RtpSendRates& rhs) = default;
  RtpSendRates& operator=(const RtpSendRates&) = default;

  DataRate& operator[](RtpPacketMediaType type) {
    return send_rates_[static_cast<size_t>(type)];
  }
  const DataRate& operator[](RtpPacketMediaType type) const {
    return send_rates_[static_cast<size_t>(type)];
  }
  DataRate Sum() const {
    return std::accumulate(send_rates_.begin(), send_rates_.end(),
                           DataRate::Zero());
  }

 private:
  std::array<DataRate, kNumMediaTypes> send_rates_;
};

// Callback, called whenever byte/packet counts have been updated.
class StreamDataCountersCallback {
 public:
  virtual ~StreamDataCountersCallback() {}

  virtual void DataCountersUpdated(const StreamDataCounters& counters,
                                   uint32_t ssrc) = 0;
};

// Information exposed through the GetStats api.
struct RtpReceiveStats {
  // `packets_lost` and `jitter` are defined by RFC 3550, and exposed in the
  // RTCReceivedRtpStreamStats dictionary, see
  // https://w3c.github.io/webrtc-stats/#receivedrtpstats-dict*
  int32_t packets_lost = 0;
  // Interarrival jitter in samples.
  uint32_t jitter = 0;
  // Interarrival jitter in time.
  webrtc::TimeDelta interarrival_jitter = webrtc::TimeDelta::Zero();

  // Time of the last packet received in unix epoch,
  // i.e. Timestamp::Zero() represents 1st Jan 1970 00:00
  std::optional<Timestamp> last_packet_received;

  // Counters exposed in RTCInboundRtpStreamStats, see
  // https://w3c.github.io/webrtc-stats/#inboundrtpstats-dict*
  RtpPacketCounter packet_counter;
};

// Callback, used to notify an observer whenever new rates have been estimated.
class BitrateStatisticsObserver {
 public:
  virtual ~BitrateStatisticsObserver() {}

  virtual void Notify(uint32_t total_bitrate_bps,
                      uint32_t retransmit_bitrate_bps, uint32_t ssrc) = 0;
};

// Callback, used to notify an observer whenever a packet is sent to the
// transport.
class SendPacketObserver {
 public:
  virtual ~SendPacketObserver() = default;
  virtual void OnSendPacket(std::optional<uint16_t> packet_id,
                            Timestamp capture_time, uint32_t ssrc) = 0;
};

}  // namespace webrtc
}  // namespace minirtc

#endif  // MODULES_RTP_RTCP_INCLUDE_RTP_RTCP_DEFINES_H_
