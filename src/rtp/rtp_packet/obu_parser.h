/*
 * @Author: DI JUNKUN
 * @Date: 2024-04-22
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _OBU_PARSER_H_
#define _OBU_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "obu.h"
#include "rtp_packet.h"

namespace minirtc {
namespace obu {

typedef enum {
  OBU_SEQUENCE_HEADER = 1,
  OBU_TEMPORAL_DELIMITER = 2,
  OBU_FRAME_HEADER = 3,
  OBU_TILE_GROUP = 4,
  OBU_METADATA = 5,
  OBU_FRAME = 6,
  OBU_REDUNDANT_FRAME_HEADER = 7,
  OBU_TILE_LIST = 8,
  OBU_PADDING = 15,
} OBU_TYPE;

std::vector<Obu> ParseObus(uint8_t* payload, int payload_size);

std::vector<Packet> Packetize(std::vector<Obu> obus);

bool NextPacket(RtpPacket* packet);

const char* ObuTypeToString(OBU_TYPE type);

bool ObuHasExtension(uint8_t obu_header);

bool ObuHasSize(uint8_t obu_header);

int ObuType(uint8_t obu_header);
}  // namespace obu
}  // namespace minirtc

#endif