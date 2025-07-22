/*
 * @Author: DI JUNKUN
 * @Date: 2024-04-22
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _OBU_H_
#define _OBU_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace minirtc {
namespace obu {
struct PayloadSizeLimits {
  int max_payload_len = 1200;
  int first_packet_reduction_len = 0;
  int last_packet_reduction_len = 0;
  // Reduction len for packet that is first & last at the same time.
  int single_packet_reduction_len = 0;
};

enum class VideoFrameType {
  kEmptyFrame = 0,
  kVideoFrameKey = 3,
  kVideoFrameDelta = 4,
};

struct Obu {
  uint8_t header;
  uint8_t extension_header;  // undefined if (header & kXbit) == 0
  std::vector<uint8_t> payload;
  size_t size;  // size of the header and payload combined.
};

struct Packet {
  explicit Packet(int first_obu_index) : first_obu(first_obu_index) {}
  // Indexes into obus_ vector of the first and last obus that should put into
  // the packet.
  int first_obu;
  int num_obu_elements = 0;
  int first_obu_offset = 0;
  int last_obu_size;
  // Total size consumed by the packet.
  int packet_size = 0;
};
}  // namespace obu
}  // namespace minirtc
#endif