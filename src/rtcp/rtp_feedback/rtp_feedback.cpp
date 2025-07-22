#include "rtp_feedback.h"

#include "byte_io.h"

// RFC 4585, Section 6.1: Feedback format.
//
// Common packet format:
//
//    0                   1                   2                   3
//    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |V=2|P|   FMT   |       PT      |          length               |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// 0 |                  SSRC of packet sender                        |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// 4 |                  SSRC of media source                         |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   :            Feedback Control Information (FCI)                 :
//   :                                                               :

namespace minirtc {

void RtpFeedback::ParseCommonFeedback(const uint8_t* payload) {
  SetSenderSsrc(ByteReader<uint32_t>::ReadBigEndian(&payload[0]));
  SetMediaSsrc(ByteReader<uint32_t>::ReadBigEndian(&payload[4]));
}

void RtpFeedback::CreateCommonFeedback(uint8_t* payload) const {
  ByteWriter<uint32_t>::WriteBigEndian(&payload[0], sender_ssrc());
  ByteWriter<uint32_t>::WriteBigEndian(&payload[4], media_ssrc());
}
}  // namespace minirtc