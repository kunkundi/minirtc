#include "receiver_report.h"

namespace minirtc {

ReceiverReport::ReceiverReport() : buffer_(nullptr), size_(0) {}

ReceiverReport::~ReceiverReport() {
  if (buffer_) {
    delete[] buffer_;
    buffer_ = nullptr;
  }
  size_ = 0;
}

void ReceiverReport::SetReportBlock(RtcpReportBlock &rtcp_report_block) {
  reports_.push_back(std::move(rtcp_report_block));
}

void ReceiverReport::SetReportBlocks(
    std::vector<RtcpReportBlock> &rtcp_report_blocks) {
  reports_ = std::move(rtcp_report_blocks);
}

const uint8_t *ReceiverReport::Build() {
  if (reports_.size() > 0x1f) {
    LOG_ERROR("Too many RTCP receiver report blocks: {}", reports_.size());
    return nullptr;
  }

  size_t buffer_size = DEFAULT_RTCP_HEADER_SIZE + DEFAULT_RR_SIZE +
                       reports_.size() * RtcpReportBlock::kLength;
  if (buffer_size != size_) {
    delete[] buffer_;
    buffer_ = new uint8_t[buffer_size];
    size_ = buffer_size;
  }

  int pos = rtcp_common_header_.Create(
      DEFAULT_RTCP_VERSION, 0, static_cast<uint8_t>(reports_.size()),
      RTCP_TYPE::RR,
      (buffer_size - DEFAULT_RTCP_HEADER_SIZE) / 4, buffer_);

  buffer_[pos] = sender_ssrc_ >> 24 & 0xFF;
  buffer_[pos + 1] = sender_ssrc_ >> 16 & 0xFF;
  buffer_[pos + 2] = sender_ssrc_ >> 8 & 0xFF;
  buffer_[pos + 3] = sender_ssrc_ & 0xFF;
  pos += 4;

  for (const auto &report : reports_) {
    pos += report.Create(buffer_ + pos);
  }

  return buffer_;
}

size_t ReceiverReport::Parse(const RtcpCommonHeader &packet) {
  reports_.clear();
  const size_t report_count = packet.count();
  const size_t required_payload_size =
      DEFAULT_RR_SIZE + report_count * RtcpReportBlock::kLength;
  if (packet.payload_size_bytes() < required_payload_size) {
    LOG_WARN("RTCP receiver report payload is too short: size={}, reports={}",
             packet.payload_size_bytes(), report_count);
    return 0;
  }

  rtcp_common_header_ = packet;

  const uint8_t *payload = packet.payload();
  size_t pos = 0;
  sender_ssrc_ = (static_cast<uint32_t>(payload[pos]) << 24) |
                 (static_cast<uint32_t>(payload[pos + 1]) << 16) |
                 (static_cast<uint32_t>(payload[pos + 2]) << 8) |
                 static_cast<uint32_t>(payload[pos + 3]);
  pos += 4;
  for (size_t i = 0; i < report_count; ++i) {
    RtcpReportBlock report;
    pos += report.Parse(payload + pos);
    reports_.push_back(std::move(report));
  }

  return pos;
}
}  // namespace minirtc
