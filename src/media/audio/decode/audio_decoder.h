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

 public:
  int Init(const MediaCodecConfig& config) override;

  int Decode(
      const uint8_t* data, size_t size,
      std::function<void(uint8_t*, int)> on_receive_decoded_frame) override;

  std::string GetDecoderName() const override { return "Opus"; }

 private:
  /* data */
  OpusDecoder* opus_decoder_ = nullptr;
  int sample_rate_ = 48000;
  int channel_num_ = 1;
  int frame_size_ = 0;

  FILE* pcm_file;
  FILE* pcm_file1;
};
}  // namespace minirtc

#endif