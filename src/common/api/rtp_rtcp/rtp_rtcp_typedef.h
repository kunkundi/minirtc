/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef _RTP_RTCP_TYPEDEF_H_
#define _RTP_RTCP_TYPEDEF_H_

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
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"

#define RTCP_CNAME_SIZE 256  // RFC 3550 page 44, including null termination
#define IP_PACKET_SIZE 1500  // we assume ethernet

namespace minirtc {
namespace webrtc {

const int kVideoPayloadTypeFrequency = 90000;

// TODO(bugs.webrtc.org/6458): Remove this when all the depending projects are
// updated to correctly set rtp rate for RtcpSender.
const int kBogusRtpRateForAudioRtcp = 8000;

// Minimum RTP header size in bytes.
const uint8_t kRtpHeaderSize = 12;

// This enum must not have any gaps, i.e., all integers between
// kRtpExtensionNone and kRtpExtensionNumberOfExtensions must be valid enum
// entries.
enum RTPExtensionType : int {
  kRtpExtensionNone,
  kRtpExtensionTransmissionTimeOffset,
  kRtpExtensionAudioLevel,
  kRtpExtensionCsrcAudioLevel,
  kRtpExtensionInbandComfortNoise,
  kRtpExtensionAbsoluteSendTime,
  kRtpExtensionAbsoluteCaptureTime,
  kRtpExtensionVideoRotation,
  kRtpExtensionTransportSequenceNumber,
  kRtpExtensionTransportSequenceNumber02,
  kRtpExtensionPlayoutDelay,
  kRtpExtensionVideoContentType,
  kRtpExtensionVideoLayersAllocation,
  kRtpExtensionVideoTiming,
  kRtpExtensionRtpStreamId,
  kRtpExtensionRepairedRtpStreamId,
  kRtpExtensionMid,
  kRtpExtensionGenericFrameDescriptor,
  kRtpExtensionGenericFrameDescriptor00 [[deprecated]] =
      kRtpExtensionGenericFrameDescriptor,
  kRtpExtensionDependencyDescriptor,
  kRtpExtensionGenericFrameDescriptor02 [[deprecated]] =
      kRtpExtensionDependencyDescriptor,
  kRtpExtensionColorSpace,
  kRtpExtensionVideoFrameTrackingId,
  kRtpExtensionCorruptionDetection,
  kRtpExtensionNumberOfExtensions  // Must be the last entity in the enum.
};

enum RTCPAppSubTypes { kAppSubtypeBwe = 0x00 };

// TODO(sprang): Make this an enum class once rtcp_receiver has been cleaned up.
enum RTCPPacketType : uint32_t {
  kRtcpReport = 0x0001,
  kRtcpSr = 0x0002,
  kRtcpRr = 0x0004,
  kRtcpSdes = 0x0008,
  kRtcpBye = 0x0010,
  kRtcpPli = 0x0020,
  kRtcpNack = 0x0040,
  kRtcpFir = 0x0080,
  kRtcpTmmbr = 0x0100,
  kRtcpTmmbn = 0x0200,
  kRtcpSrReq = 0x0400,
  kRtcpLossNotification = 0x2000,
  kRtcpRemb = 0x10000,
  kRtcpTransmissionTimeOffset = 0x20000,
  kRtcpXrReceiverReferenceTime = 0x40000,
  kRtcpXrDlrrReportBlock = 0x80000,
  kRtcpTransportFeedback = 0x100000,
  kRtcpXrTargetBitrate = 0x200000,
};

enum class KeyFrameReqMethod : uint8_t {
  kNone,     // Don't request keyframes.
  kPliRtcp,  // Request keyframes through Picture Loss Indication.
  kFirRtcp   // Request keyframes through Full Intra-frame Request.
};

enum RtxMode {
  kRtxOff = 0x0,
  kRtxRetransmitted = 0x1,     // Only send retransmissions over RTX.
  kRtxRedundantPayloads = 0x2  // Preventively send redundant payloads
                               // instead of padding.
};

const size_t kRtxHeaderSize = 2;

// NOTE! `kNumMediaTypes` must be kept in sync with RtpPacketMediaType!
static constexpr size_t kNumMediaTypes = 5;
enum class RtpPacketMediaType : size_t {
  kAudio,                         // Audio media packets.
  kVideo,                         // Video media packets.
  kRetransmission,                // Retransmisions, sent as response to NACK.
  kForwardErrorCorrection,        // FEC packets.
  kPadding = kNumMediaTypes - 1,  // RTX or plain padding sent to maintain BWE.
  // Again, don't forget to update `kNumMediaTypes` if you add another value!
};
}  // namespace webrtc
}  // namespace minirtc

#endif  // _RTP_RTCP_TYPEDEF_H_