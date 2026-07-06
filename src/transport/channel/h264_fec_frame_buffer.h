#ifndef _H264_FEC_FRAME_BUFFER_H_
#define _H264_FEC_FRAME_BUFFER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "fec_decoder.h"
#include "rtp_fec.h"

namespace minirtc {

class H264FecFrameBuffer {
 public:
  H264FecFrameBuffer() = default;
  ~H264FecFrameBuffer() = default;

  bool InsertPacket(const RtpFecPacket& packet,
                    std::vector<uint8_t>* complete_frame,
                    int64_t now_ms = 0);
  void RemoveExpired(int64_t now_ms, int64_t timeout_ms);

 private:
  struct BlockState {
    FecBlockConfig config;
    std::unique_ptr<FecBlockDecoder> decoder;
    bool complete = false;
    std::vector<uint8_t> decoded_data;
  };

  struct FrameState {
    uint32_t frame_id = 0;
    uint32_t rtp_timestamp = 0;
    uint32_t original_frame_size = 0;
    uint16_t block_count = 0;
    int64_t first_packet_ms = 0;
    std::map<uint16_t, BlockState> blocks;
  };

  uint64_t MakeFrameKey(const H264FecHeader& header) const;
  bool IsFrameComplete(const FrameState& frame) const;

 private:
  std::map<uint64_t, FrameState> frames_;
  std::map<uint64_t, int64_t> expired_frames_;
};

}  // namespace minirtc

#endif
