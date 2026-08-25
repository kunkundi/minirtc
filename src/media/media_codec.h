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
#include "bitrate_limits.h"

#define MINIRTC_INIT_WIDTH 1920
#define MINIRTC_INIT_HEIGHT 1080
#define MINIRTC_INIT_BITRATE 1'000'000
#define MINIRTC_MIN_BITRATE 1'000'000
#define MINIRTC_AVERAGE_BITRATE 2'500'000
#define MINIRTC_MIN_FRAME_RATE 15
#define MINIRTC_MAX_FRAME_RATE 60
#define MINIRTC_KEY_FRAME_INTERVAL 3000
#define MINIRTC_MAX_PAYLOAD_SIZE 1150

namespace minirtc {

class MediaCodecConfig {
 public:
  MediaCodecConfig()
      : init_width(MINIRTC_INIT_WIDTH),
        init_height(MINIRTC_INIT_HEIGHT),
        init_bitrate(MINIRTC_INIT_BITRATE),
        min_bitrate(MINIRTC_MIN_BITRATE),
        average_bitrate(MINIRTC_AVERAGE_BITRATE),
        max_bitrate(kDefaultMaxEncoderBitrateBps),
        min_frame_rate(MINIRTC_MIN_FRAME_RATE),
        max_frame_rate(MINIRTC_MAX_FRAME_RATE),
        key_frame_interval(MINIRTC_KEY_FRAME_INTERVAL),
        max_payload_size(MINIRTC_MAX_PAYLOAD_SIZE),
        video_content_type(VideoContentType::ScreenContent),
        video_degradation_preference(
            VideoDegradationPreference::MaintainFrameRate) {}
  ~MediaCodecConfig() {}

  int init_width;
  int init_height;
  int init_bitrate;
  int min_bitrate;
  int average_bitrate;
  int max_bitrate;
  int min_frame_rate;
  int max_frame_rate;
  int key_frame_interval;
  int max_payload_size;
  VideoContentType video_content_type;
  VideoDegradationPreference video_degradation_preference;
};

class MediaCodec {
 public:
  MediaCodec() {}
  virtual ~MediaCodec() {}

 public:
  virtual int Init() {
    LOG_INFO("Not implemented");
    return 0;
  }

  virtual int Init(const MediaCodecConfig& config) {
    LOG_INFO("Not implemented");
    return 0;
  }

  virtual int Encode(
      const RawFrame& raw_frame,
      std::function<int(const EncodedFrame& encoded_frame)> on_encoded_image) {
    // Video encoders that rebuild themselves when the input dimensions change
    // must guarantee that the first frame emitted at the new resolution is a
    // key frame carrying the codec configuration needed by the decoder.
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

  virtual bool SupportsDynamicEncodingSpeedPriority() const { return false; }

  virtual int SetPrioritizeEncodingSpeedOverQuality(bool prioritize_speed) {
    return -1;
  }

  virtual int GetResolution(int* width, int* height) const { return 0; }

  virtual std::string GetEncoderName() const { return ""; }

  virtual std::string GetDecoderName() const { return ""; }
};
}  // namespace minirtc

#endif
