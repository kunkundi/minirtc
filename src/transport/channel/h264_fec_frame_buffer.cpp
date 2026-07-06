#include "h264_fec_frame_buffer.h"

#include <algorithm>

namespace minirtc {

uint64_t H264FecFrameBuffer::MakeFrameKey(const H264FecHeader& header) const {
  return (static_cast<uint64_t>(header.frame_id) << 32) |
         static_cast<uint64_t>(header.rtp_timestamp);
}

bool H264FecFrameBuffer::IsFrameComplete(const FrameState& frame) const {
  if (frame.block_count == 0 ||
      frame.blocks.size() != static_cast<size_t>(frame.block_count)) {
    return false;
  }

  for (uint16_t block_index = 0; block_index < frame.block_count;
       ++block_index) {
    auto block = frame.blocks.find(block_index);
    if (block == frame.blocks.end() || !block->second.complete) {
      return false;
    }
  }

  return true;
}

bool H264FecFrameBuffer::InsertPacket(const RtpFecPacket& packet,
                                      std::vector<uint8_t>* complete_frame,
                                      int64_t now_ms) {
  if (!complete_frame || packet.header.version != kH264FecVersion ||
      packet.header.block_count == 0 ||
      packet.header.block_index >= packet.header.block_count ||
      packet.header.symbol_size == 0 ||
      packet.header.source_symbol_count == 0 ||
      packet.header.repair_symbol_count == 0 ||
      packet.header.block_original_size == 0 ||
      packet.symbol.size() != packet.header.symbol_size) {
    return false;
  }

  const uint64_t frame_key = MakeFrameKey(packet.header);
  if (expired_frames_.find(frame_key) != expired_frames_.end()) {
    return false;
  }

  auto frame_iter = frames_.find(frame_key);
  if (frame_iter == frames_.end()) {
    FrameState frame;
    frame.frame_id = packet.header.frame_id;
    frame.rtp_timestamp = packet.header.rtp_timestamp;
    frame.original_frame_size = packet.header.original_frame_size;
    frame.block_count = packet.header.block_count;
    frame.first_packet_ms = now_ms;
    frame_iter = frames_.emplace(frame_key, std::move(frame)).first;
  }

  FrameState& frame = frame_iter->second;
  if (frame.original_frame_size != packet.header.original_frame_size ||
      frame.block_count != packet.header.block_count) {
    return false;
  }

  auto block_iter = frame.blocks.find(packet.header.block_index);
  if (block_iter == frame.blocks.end()) {
    FecBlockConfig config;
    config.source_symbol_count = packet.header.source_symbol_count;
    config.repair_symbol_count = packet.header.repair_symbol_count;
    config.symbol_size = packet.header.symbol_size;
    config.original_size = packet.header.block_original_size;

    BlockState block;
    block.config = config;
    block.decoder = std::make_unique<FecBlockDecoder>(config);
    block_iter =
        frame.blocks.emplace(packet.header.block_index, std::move(block)).first;
  }

  BlockState& block = block_iter->second;
  if (block.complete) {
    return IsFrameComplete(frame);
  }

  FecSymbol symbol;
  symbol.symbol_id = packet.header.symbol_id;
  symbol.is_repair = (packet.header.flags & kH264FecFlagRepair) != 0;
  symbol.data = packet.symbol;

  FecDecodeStatus status = block.decoder->AddSymbol(symbol);
  if (status == FecDecodeStatus::kComplete) {
    block.complete = true;
    block.decoded_data = block.decoder->DecodedData();
  } else if (status == FecDecodeStatus::kError) {
    return false;
  }

  if (!IsFrameComplete(frame)) {
    return false;
  }

  complete_frame->clear();
  complete_frame->reserve(frame.original_frame_size);
  for (uint16_t block_index = 0; block_index < frame.block_count;
       ++block_index) {
    const auto& decoded_block = frame.blocks[block_index].decoded_data;
    complete_frame->insert(complete_frame->end(), decoded_block.begin(),
                           decoded_block.end());
  }

  if (complete_frame->size() > frame.original_frame_size) {
    complete_frame->resize(frame.original_frame_size);
  }

  const bool exact_size = complete_frame->size() == frame.original_frame_size;
  frames_.erase(frame_iter);
  return exact_size;
}

void H264FecFrameBuffer::RemoveExpired(int64_t now_ms, int64_t timeout_ms) {
  for (auto iter = frames_.begin(); iter != frames_.end();) {
    if (iter->second.first_packet_ms != 0 &&
        now_ms - iter->second.first_packet_ms > timeout_ms) {
      expired_frames_[iter->first] = now_ms;
      iter = frames_.erase(iter);
    } else {
      ++iter;
    }
  }

  for (auto iter = expired_frames_.begin(); iter != expired_frames_.end();) {
    if (timeout_ms > 0 && now_ms - iter->second > timeout_ms * 5) {
      iter = expired_frames_.erase(iter);
    } else {
      ++iter;
    }
  }
}

}  // namespace minirtc
