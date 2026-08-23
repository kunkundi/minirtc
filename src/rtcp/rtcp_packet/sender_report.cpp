#include "sender_report.h"

namespace minirtc {

SenderReport::SenderReport() : buffer_(nullptr), size_(0) {}

SenderReport::~SenderReport() {
  if (buffer_) {
    delete[] buffer_;
    buffer_ = nullptr;
  }

  size_ = 0;
}

void SenderReport::SetReportBlock(RtcpReportBlock &rtcp_report_block) {
  reports_.push_back(std::move(rtcp_report_block));
}

void SenderReport::SetReportBlocks(
    std::vector<RtcpReportBlock> &rtcp_report_blocks) {
  reports_ = std::move(rtcp_report_blocks);
}

const uint8_t *SenderReport::Build() {
  if (reports_.size() > 0x1f) {
    LOG_ERROR("Too many RTCP sender report blocks: {}", reports_.size());
    return nullptr;
  }

  size_t buffer_size = DEFAULT_RTCP_HEADER_SIZE + DEFAULT_SR_SIZE +
                       reports_.size() * RtcpReportBlock::kLength;
  if (buffer_size != size_) {
    delete[] buffer_;
    buffer_ = new uint8_t[buffer_size];
    size_ = buffer_size;
  }

  int pos = rtcp_common_header_.Create(
      DEFAULT_RTCP_VERSION, 0, static_cast<uint8_t>(reports_.size()),
      RTCP_TYPE::SR,
      (buffer_size - DEFAULT_RTCP_HEADER_SIZE) / 4, buffer_);

  buffer_[pos++] = sender_info_.sender_ssrc >> 24 & 0xFF;
  buffer_[pos++] = sender_info_.sender_ssrc >> 16 & 0xFF;
  buffer_[pos++] = sender_info_.sender_ssrc >> 8 & 0xFF;
  buffer_[pos++] = sender_info_.sender_ssrc & 0xFF;

  buffer_[pos++] = sender_info_.ntp_ts_msw >> 24 & 0xFF;
  buffer_[pos++] = sender_info_.ntp_ts_msw >> 16 & 0xFF;
  buffer_[pos++] = sender_info_.ntp_ts_msw >> 8 & 0xFF;
  buffer_[pos++] = sender_info_.ntp_ts_msw & 0xFF;
  buffer_[pos++] = sender_info_.ntp_ts_lsw >> 24 & 0xFF;
  buffer_[pos++] = sender_info_.ntp_ts_lsw >> 16 & 0xFF;
  buffer_[pos++] = sender_info_.ntp_ts_lsw >> 8 & 0xFF;
  buffer_[pos++] = sender_info_.ntp_ts_lsw & 0xFF;

  buffer_[pos++] = sender_info_.rtp_ts >> 24 & 0xFF;
  buffer_[pos++] = sender_info_.rtp_ts >> 16 & 0xFF;
  buffer_[pos++] = sender_info_.rtp_ts >> 8 & 0xFF;
  buffer_[pos++] = sender_info_.rtp_ts & 0xFF;

  buffer_[pos++] = sender_info_.sender_packet_count >> 24 & 0xFF;
  buffer_[pos++] = sender_info_.sender_packet_count >> 16 & 0xFF;
  buffer_[pos++] = sender_info_.sender_packet_count >> 8 & 0xFF;
  buffer_[pos++] = sender_info_.sender_packet_count & 0xFF;

  buffer_[pos++] = sender_info_.sender_octet_count >> 24 & 0xFF;
  buffer_[pos++] = sender_info_.sender_octet_count >> 16 & 0xFF;
  buffer_[pos++] = sender_info_.sender_octet_count >> 8 & 0xFF;
  buffer_[pos++] = sender_info_.sender_octet_count & 0xFF;

  for (auto &report : reports_) {
    pos += report.Create(buffer_ + pos);
  }

  return buffer_;
}

bool SenderReport::Parse(const RtcpCommonHeader &packet) {
  reports_.clear();
  constexpr size_t kSenderInfoLength = DEFAULT_SR_SIZE;
  const size_t report_count = packet.count();
  const size_t required_payload_size =
      kSenderInfoLength + report_count * RtcpReportBlock::kLength;
  if (packet.payload_size_bytes() < required_payload_size) {
    LOG_WARN("RTCP sender report payload is too short: size={}, reports={}",
             packet.payload_size_bytes(), report_count);
    return false;
  }

  rtcp_common_header_ = packet;
  const uint8_t *payload = packet.payload();
  size_t pos = 0;
  const auto read_u32 = [&payload, &pos]() {
    const uint32_t value =
        (static_cast<uint32_t>(payload[pos]) << 24) |
        (static_cast<uint32_t>(payload[pos + 1]) << 16) |
        (static_cast<uint32_t>(payload[pos + 2]) << 8) |
        static_cast<uint32_t>(payload[pos + 3]);
    pos += 4;
    return value;
  };

  sender_info_.sender_ssrc = read_u32();
  sender_ssrc_ = sender_info_.sender_ssrc;
  sender_info_.ntp_ts_msw = read_u32();
  sender_info_.ntp_ts_lsw = read_u32();
  sender_info_.rtp_ts = read_u32();
  sender_info_.sender_packet_count = read_u32();
  sender_info_.sender_octet_count = read_u32();

  for (size_t i = 0; i < report_count; ++i) {
    RtcpReportBlock report;
    pos += report.Parse(payload + pos);
    reports_.emplace_back(std::move(report));
  }

  return pos == required_payload_size;
}
}
