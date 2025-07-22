#include "obu_parser.h"

#include "byte_buffer.h"
#include "log.h"

namespace minirtc {
namespace obu {

constexpr int kAggregationHeaderSize = 1;
// when there are 3 or less OBU (fragments) in a packet, size of the last one
// can be omited.
constexpr int kMaxNumObusToOmitSize = 3;
constexpr uint8_t kObuSizePresentBit = 0b0'0000'010;
constexpr int kObuTypeSequenceHeader = 1;
constexpr int kObuTypeTemporalDelimiter = 2;
constexpr int kObuTypeTileList = 8;
constexpr int kObuTypePadding = 15;

const char* ObuTypeToString(OBU_TYPE type) {
  switch (type) {
    case OBU_SEQUENCE_HEADER:
      return "OBU_SEQUENCE_HEADER";
    case OBU_TEMPORAL_DELIMITER:
      return "OBU_TEMPORAL_DELIMITER";
    case OBU_FRAME_HEADER:
      return "OBU_FRAME_HEADER";
    case OBU_REDUNDANT_FRAME_HEADER:
      return "OBU_REDUNDANT_FRAME_HEADER";
    case OBU_FRAME:
      return "OBU_FRAME";
    case OBU_TILE_GROUP:
      return "OBU_TILE_GROUP";
    case OBU_METADATA:
      return "OBU_METADATA";
    case OBU_TILE_LIST:
      return "OBU_TILE_LIST";
    case OBU_PADDING:
      return "OBU_PADDING";
    default:
      break;
  }
  return "<Invalid OBU Type>";
}

bool ObuHasExtension(uint8_t obu_header) { return obu_header & 0b0'0000'100; }

bool ObuHasSize(uint8_t obu_header) { return obu_header & kObuSizePresentBit; }

int ObuType(uint8_t obu_header) { return (obu_header & 0b0'1111'000) >> 3; }

std::vector<Obu> ParseObus(uint8_t* payload, int payload_size) {
  std::vector<Obu> result;
  ByteBufferReader payload_reader(reinterpret_cast<const char*>(payload),
                                  payload_size);
  while (payload_reader.Length() > 0) {
    Obu obu;
    bool has_ext_header = false;
    payload_reader.ReadUInt8(&obu.header);
    obu.size = 1;
    if (ObuHasExtension(obu.header)) {
      if (payload_reader.Length() == 0) {
        LOG_ERROR(
            "Malformed AV1 input: expected extension_header, no more bytes in "
            "the buffer. Offset: {}",
            (payload_size - payload_reader.Length()));
        return {};
      }
      payload_reader.ReadUInt8(&obu.extension_header);
      ++obu.size;
      has_ext_header = true;
    }
    if (!ObuHasSize(obu.header)) {
      obu.payload = std::vector<uint8_t>(
          reinterpret_cast<const uint8_t*>(payload_reader.Data()),
          reinterpret_cast<const uint8_t*>(payload_reader.Data()) +
              payload_reader.Length());
      payload_reader.Consume(payload_reader.Length());
    } else {
      uint64_t size = 0;
      size_t size_len = 0;
      if (!payload_reader.ReadUVarint(&size, &size_len) ||
          size > payload_reader.Length()) {
        LOG_ERROR(
            "Malformed AV1 input: declared payload_size {} is larger than "
            "remaining buffer size {}",
            size, payload_reader.Length());
        return {};
      }
      if (0 == size) {
        obu.payload.push_back(0);
      } else {
        obu.payload = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(payload_reader.Data()),
            reinterpret_cast<const uint8_t*>(payload_reader.Data()) + size);

        std::vector<uint8_t> size_data = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(payload_reader.Data() - size_len),
            reinterpret_cast<const uint8_t*>(payload_reader.Data() - size_len) +
                size_len);
        obu.payload.insert(obu.payload.begin(), size_data.begin(),
                           size_data.end());
      }
      payload_reader.Consume(size);
    }
    obu.size += obu.payload.size();
    if (has_ext_header) {
      obu.payload.insert(obu.payload.begin(), obu.extension_header);
    }
    // Skip obus that shouldn't be transfered over rtp.
    // int obu_type = ObuType(obu.header);
    obu.payload.insert(obu.payload.begin(), obu.header);
    // if (obu_type != kObuTypeTemporalDelimiter &&  //
    //     obu_type != kObuTypeTileList &&           //
    //     obu_type != kObuTypePadding) {
    result.push_back(obu);
    // }
  }

  // for (int i = 0; i < result.size(); i++) {
  //   LOG_ERROR("[{}] Obu size = [{}], Obu type [{}|{}]", i, result[i].size,
  //             ObuType(result[i].payload[0]),
  //             ObuTypeToString((OBU_TYPE)ObuType(result[i].header)));
  // }

  return result;
}

}  // namespace obu
}  // namespace minirtc