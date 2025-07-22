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
  size_t buffer_size = DEFAULT_RTCP_HEADER_SIZE + DEFAULT_RR_SIZE +
                       reports_.size() * RtcpReportBlock::kLength;
  if (!buffer_ || buffer_size != size_) {
    delete[] buffer_;
    buffer_ = nullptr;
  }

  buffer_ = new uint8_t[buffer_size];
  size_ = buffer_size;

  int pos = rtcp_common_header_.Create(
      DEFAULT_RTCP_VERSION, 0, DEFAULT_RR_BLOCK_NUM, RTCP_TYPE::RR,
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
  rtcp_common_header_ = packet;

  const uint8_t *payload = packet.payload();
  const uint8_t *payload_end = packet.payload() + packet.payload_size_bytes();
  size_t pos = 0;
  sender_ssrc_ = (payload[pos] << 24) + (payload[pos + 1] << 16) +
                 (payload[pos + 2] << 8) + payload[pos + 3];
  pos += 4;
  while (payload + pos < payload_end) {
    RtcpReportBlock report;
    pos += report.Parse(payload + pos);
    reports_.push_back(std::move(report));
  }

  return pos;
}
}  // namespace minirtc