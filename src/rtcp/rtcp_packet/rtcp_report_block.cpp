#include "rtcp_report_block.h"

#include "byte_io.h"
#include "log.h"

// From RFC 3550, RTP: A Transport Protocol for Real-Time Applications.
//
// RTCP report block (RFC 3550).
//
//     0                   1                   2                   3
//     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//    +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//  0 |                 SSRC_1 (SSRC of first source)                 |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  4 | fraction lost |       cumulative number of packets lost       |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//  8 |           extended highest sequence number received           |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// 12 |                      interarrival jitter                      |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// 16 |                         last SR (LSR)                         |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// 20 |                   delay since last SR (DLSR)                  |
// 24 +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+

namespace minirtc {

RtcpReportBlock::RtcpReportBlock()
    : sender_ssrc_(0),
      source_ssrc_(0),
      fraction_lost_(0),
      cumulative_lost_(0),
      extended_high_seq_num_(0),
      jitter_(0),
      last_sr_(0),
      delay_since_last_sr_(0),
      report_block_timestamp_utc_(0),
      report_block_timestamp_(0),
      last_rtt_(0),
      sum_rtt_(0),
      num_rtts_(0) {}

size_t RtcpReportBlock::Create(uint8_t* buffer) const {
  buffer[0] = (source_ssrc_ >> 24) & 0xFF;
  buffer[1] = (source_ssrc_ >> 16) & 0xFF;
  buffer[2] = (source_ssrc_ >> 8) & 0xFF;
  buffer[3] = source_ssrc_ & 0xFF;

  buffer[4] = fraction_lost_;

  buffer[5] = (cumulative_lost_ >> 16) & 0xFF;
  buffer[6] = (cumulative_lost_ >> 8) & 0xFF;
  buffer[7] = cumulative_lost_ & 0xFF;

  buffer[8] = (extended_high_seq_num_ >> 24) & 0xFF;
  buffer[9] = (extended_high_seq_num_ >> 16) & 0xFF;
  buffer[10] = (extended_high_seq_num_ >> 8) & 0xFF;
  buffer[11] = extended_high_seq_num_ & 0xFF;

  buffer[12] = (jitter_ >> 24) & 0xFF;
  buffer[13] = (jitter_ >> 16) & 0xFF;
  buffer[14] = (jitter_ >> 8) & 0xFF;
  buffer[15] = jitter_ & 0xFF;

  buffer[16] = (last_sr_ >> 24) & 0xFF;
  buffer[17] = (last_sr_ >> 16) & 0xFF;
  buffer[18] = (last_sr_ >> 8) & 0xFF;
  buffer[19] = last_sr_ & 0xFF;

  buffer[20] = (delay_since_last_sr_ >> 24) & 0xFF;
  buffer[21] = (delay_since_last_sr_ >> 16) & 0xFF;
  buffer[22] = (delay_since_last_sr_ >> 8) & 0xFF;
  buffer[23] = delay_since_last_sr_ & 0xFF;

  return RtcpReportBlock::kLength;
}

size_t RtcpReportBlock::Parse(const uint8_t* buffer) {
  source_ssrc_ =
      (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
  fraction_lost_ = buffer[4];
  cumulative_lost_ = (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
  if (cumulative_lost_ & 0x800000) {  // Check if the sign bit is set
    cumulative_lost_ |= 0xFF000000;   // Sign extend to 32 bits
  }
  extended_high_seq_num_ =
      (buffer[8] << 24) | (buffer[9] << 16) | (buffer[10] << 8) | buffer[11];
  jitter_ =
      (buffer[12] << 24) | (buffer[13] << 16) | (buffer[14] << 8) | buffer[15];
  last_sr_ =
      (buffer[16] << 24) | (buffer[17] << 16) | (buffer[18] << 8) | buffer[19];
  delay_since_last_sr_ =
      (buffer[20] << 24) | (buffer[21] << 16) | (buffer[22] << 8) | buffer[23];
  return RtcpReportBlock::kLength;
}

void RtcpReportBlock::SetReportBlock(uint32_t sender_ssrc,
                                     const RtcpReportBlock& report_block,
                                     int64_t report_block_timestamp_utc,
                                     int64_t report_block_timestamp) {
  sender_ssrc_ = sender_ssrc;
  source_ssrc_ = report_block.SourceSsrc();
  fraction_lost_ = report_block.FractionLost();
  cumulative_lost_ = report_block.CumulativeLost();
  extended_high_seq_num_ = report_block.ExtendedHighSeqNum();
  jitter_ = report_block.Jitter();
  report_block_timestamp_utc_ = report_block_timestamp_utc;
  report_block_timestamp_ = report_block_timestamp;
}

void RtcpReportBlock::AddRoundTripTimeSample(int64_t rtt) {
  last_rtt_ = rtt;
  sum_rtt_ += rtt;
  ++num_rtts_;
}
}  // namespace minirtc