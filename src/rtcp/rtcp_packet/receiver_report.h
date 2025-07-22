/*
 * @Author: DI JUNKUN
 * @Date: 2025-02-18
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RECEIVER_REPORT_H_
#define _RECEIVER_REPORT_H_

// RR
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P|   RC    |   PT=SR=200   |            length             |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                     SSRC of packet sender                     |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |                 SSRC_1 (SSRC of first source)                 | report
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ block
// | fraction lost |       cumulative number of packets lost       | 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |           extended highest sequence number received           |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                      interarrival jitter                      |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                         last SR (LSR)                         |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                   delay since last SR (DLSR)                  |
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |                 SSRC_2 (SSRC of second source)                | report
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ block
// :                               ...                             : 2
// +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
// |                  profile-specific extensions                  |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

#include <vector>

#include "rtcp_common_header.h"
#include "rtcp_report_block.h"

namespace minirtc {

class ReceiverReport {
 public:
  ReceiverReport();
  ~ReceiverReport();

 public:
  void SetSenderSsrc(uint32_t ssrc) { sender_ssrc_ = ssrc; }
  void SetReportBlock(RtcpReportBlock &rtcp_report_block);
  void SetReportBlocks(std::vector<RtcpReportBlock> &rtcp_report_blocks);

  uint32_t SenderSsrc() const { return sender_ssrc_; }
  std::vector<RtcpReportBlock> GetReportBlocks() const { return reports_; }

  const uint8_t *Build();
  size_t Parse(const RtcpCommonHeader &packet);

  const uint8_t *Buffer() const { return buffer_; }
  size_t Size() const { return size_; }

 private:
  uint32_t sender_ssrc_;
  RtcpCommonHeader rtcp_common_header_;
  std::vector<RtcpReportBlock> reports_;

  // Entire RTCP buffer
  uint8_t *buffer_ = nullptr;
  size_t size_ = 0;
};
}  // namespace minirtc

#endif