#include "rtp_packet_h264.h"

namespace minirtc {
RtpPacketH264::RtpPacketH264() {}

RtpPacketH264::~RtpPacketH264() {}

bool RtpPacketH264::GetFrameHeaderInfo() {
  if (fu_info_got_) {
    return true;
  }

  const uint8_t* frame_buffer = Payload();
  size_t offset = 0;

  if (rtp::PAYLOAD_TYPE::RTX == PayloadType()) {
    osn_ = frame_buffer[0] << 8 | frame_buffer[0 + 1];
    offset = 2;
  }

  fu_indicator_.forbidden_bit = (frame_buffer[0 + offset] >> 7) & 0x01;
  fu_indicator_.nal_reference_idc = (frame_buffer[0 + offset] >> 5) & 0x03;
  fu_indicator_.nal_unit_type = frame_buffer[0 + offset] & 0x1F;

  if (rtp::NAL_UNIT_TYPE::NALU == fu_indicator_.nal_unit_type) {
    add_offset_to_payload(1 + offset);
  } else if (rtp::NAL_UNIT_TYPE::FU_A == fu_indicator_.nal_unit_type) {
    fu_header_.start = (frame_buffer[1 + offset] >> 7) & 0x01;
    fu_header_.end = (frame_buffer[1 + offset] >> 6) & 0x01;
    fu_header_.remain_bit = (frame_buffer[1 + offset] >> 5) & 0x01;
    fu_header_.nal_unit_type = frame_buffer[1 + offset] & 0x1F;
    add_offset_to_payload(2 + offset);
  }

  fu_info_got_ = true;

  return true;
}
}  // namespace minirtc