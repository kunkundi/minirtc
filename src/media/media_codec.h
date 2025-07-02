/*
 * @Author: DI JUNKUN
 * @Date: 2025-05-14
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _MEDIA_CODEC_H_
#define _MEDIA_CODEC_H_

#include <stdio.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "audio_frame.h"
#include "clock/system_clock.h"
#include "decoded_frame.h"
#include "encoded_frame.h"
#include "log.h"
#include "media_codec.h"
#include "minirtc.h"
#include "raw_frame.h"
#include "received_frame.h"

#define I_FRAME_INTERVAL 3000

class MediaCodec {
 public:
  MediaCodec() {}
  virtual ~MediaCodec() {}

 public:
  virtual int Init() { return 0; }

  virtual int Encode(
      const RawFrame& raw_frame,
      std::function<int(const EncodedFrame& encoded_frame)> on_encoded_image) {
    LOG_INFO("Not implemented");
    return 0;
  }

  virtual int Decode(
      std::unique_ptr<ReceivedFrame> received_frame,
      std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
    LOG_INFO("Not implemented");
    return 0;
  }

  virtual int Encode(const uint8_t* data, size_t size,
                     std::function<int(char* encoded_audio_buffer, size_t size)>
                         on_encoded_audio_buffer) {
    LOG_INFO("Not implemented");
    return 0;
  }

  virtual int Decode(
      const uint8_t* data, size_t size,
      std::function<void(uint8_t*, int)> on_receive_decoded_frame) {
    LOG_INFO("Not implemented");
    return 0;
  }

  virtual int ForceIdr() { return 0; }

  virtual int SetTargetBitrate(int bitrate) { return 0; }

  virtual int GetResolution(int* width, int* height) { return 0; }

  virtual std::string GetEncoderName() { return ""; }

  virtual std::string GetDecoderName() { return ""; }
};

#endif