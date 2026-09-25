/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-24
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _AUDIO_DECODER_H_
#define _AUDIO_DECODER_H_

#include <stdio.h>

#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "audio_frame.h"
#include "media_codec.h"
#include "opus/opus.h"

namespace minirtc {

class AudioDecoder : public MediaCodec {
 public:
  AudioDecoder(int sample_rate, int channel_num, int frame_size);
  virtual ~AudioDecoder();
  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

 public:
  int Init() override;

  int Decode(
      const uint8_t* data, size_t size,
      std::function<void(uint8_t*, int)> on_receive_decoded_frame) override;

  int DecodePacket(const uint8_t* data, size_t size, uint16_t sequence,
                   uint32_t timestamp,
                   std::function<void(uint8_t*, int)> on_frame);
  struct RecoveryStats {
    uint64_t fec_attempts = 0;  // Opus may internally fall back to PLC.
    uint64_t plc_frames = 0;
    uint64_t late_or_duplicate_packets = 0;
    uint64_t discontinuities = 0;
  };
  const RecoveryStats& Stats() const { return recovery_stats_; }

  std::string GetDecoderName() const override { return "Opus"; }

 private:
  int DecodeFrame(const uint8_t* data, size_t size, int samples, bool fec,
                  const std::function<void(uint8_t*, int)>& on_frame);
  std::vector<opus_int16> pcm_;
  bool have_sequence_ = false;
  uint16_t last_sequence_ = 0;
  uint32_t last_timestamp_ = 0;
  int last_samples_ = 0;
  RecoveryStats recovery_stats_;
  OpusDecoder* opus_decoder_ = nullptr;
  int sample_rate_ = 48000;
  int channel_num_ = 1;
  int frame_size_ = 0;

};
}  // namespace minirtc

#endif