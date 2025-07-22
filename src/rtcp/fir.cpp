/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "fir.h"

#include "byte_io.h"
#include "log.h"

namespace minirtc {
namespace webrtc {
namespace rtcp {
// RFC 4585: Feedback format.
// Common packet format:
//
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |V=2|P|   FMT   |       PT      |          length               |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                  SSRC of packet sender                        |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |             SSRC of media source (unused) = 0                 |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  :            Feedback Control Information (FCI)                 :
//  :                                                               :
// Full intra request (FIR) (RFC 5104).
// The Feedback Control Information (FCI) for the Full Intra Request
// consists of one or more FCI entries.
// FCI:
//   0                   1                   2                   3
//   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  |                              SSRC                             |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  | Seq nr.       |    Reserved = 0                               |
//  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

Fir::Fir() = default;

Fir::Fir(const Fir& fir) = default;

Fir::~Fir() = default;

bool Fir::Parse(const RtcpCommonHeader& packet) {
  // The FCI field MUST contain one or more FIR entries.
  if (packet.payload_size_bytes() < kCommonFeedbackLength + kFciLength) {
    LOG_WARN("Packet is too small to be a valid FIR packet.");
    return false;
  }

  if ((packet.payload_size_bytes() - kCommonFeedbackLength) % kFciLength != 0) {
    LOG_WARN("Invalid size for a valid FIR packet.");
    return false;
  }

  ParseCommonFeedback(packet.payload());

  size_t number_of_fci_items =
      (packet.payload_size_bytes() - kCommonFeedbackLength) / kFciLength;
  const uint8_t* next_fci = packet.payload() + kCommonFeedbackLength;
  items_.resize(number_of_fci_items);
  for (Request& request : items_) {
    request.ssrc = ByteReader<uint32_t>::ReadBigEndian(next_fci);
    request.seq_nr = ByteReader<uint8_t>::ReadBigEndian(next_fci + 4);
    next_fci += kFciLength;
  }
  return true;
}

size_t Fir::BlockLength() const {
  return kHeaderLength + kCommonFeedbackLength + kFciLength * items_.size();
}

bool Fir::Create(uint8_t* packet, size_t* index, size_t max_length,
                 PacketReadyCallback callback) const {
  while (*index + BlockLength() > max_length) {
    if (!OnBufferFull(packet, index, callback)) return false;
  }
  size_t index_end = *index + BlockLength();
  CreateHeader(kFeedbackMessageType, kPacketType, HeaderLength(), packet,
               index);

  CreateCommonFeedback(packet + *index);
  *index += kCommonFeedbackLength;

  constexpr uint32_t kReserved = 0;
  for (const Request& request : items_) {
    ByteWriter<uint32_t>::WriteBigEndian(packet + *index, request.ssrc);
    ByteWriter<uint8_t>::WriteBigEndian(packet + *index + 4, request.seq_nr);
    ByteWriter<uint32_t, 3>::WriteBigEndian(packet + *index + 5, kReserved);
    *index += kFciLength;
  }

  return true;
}
}  // namespace rtcp
}  // namespace webrtc
}  // namespace minirtc