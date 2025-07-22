/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-24
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _AUDIO_ENCODER_H_
#define _AUDIO_ENCODER_H_

#include <functional>
#include <vector>

#include "media_codec.h"
#include "opus/opus.h"

namespace minirtc {

class AudioEncoder : public MediaCodec {
 public:
  AudioEncoder(int sample_rate, int channel_num, int frame_size);
  virtual ~AudioEncoder();

  int Init();

  int Encode(const uint8_t* data, size_t size,
             std::function<int(char* encoded_audio_buffer, size_t size)>
                 on_encoded_audio_buffer);

  std::string GetEncoderName() { return "Opus"; }

 private:
  OpusEncoder* opus_encoder_ = nullptr;
  int sample_rate_ = 48000;
  int channel_num_ = 1;
  int frame_size_ = 480;
};
}  // namespace minirtc

#endif  // _AUDIO_ENCODER_H_
