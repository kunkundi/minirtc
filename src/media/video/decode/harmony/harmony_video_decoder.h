/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-15
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _HARMONY_VIDEO_DECODER_H_
#define _HARMONY_VIDEO_DECODER_H_

#include "media_codec.h"
#include "h264_bitstream_parser.h"
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <atomic>
#include <map>

namespace minirtc {
// API 20 synchronous codec queues are drained on a dedicated output thread.
// The public frame contract remains packed NV12, shared with GLES and previews.
class HarmonyVideoDecoder final : public MediaCodec {
 public:
  HarmonyVideoDecoder(std::shared_ptr<SystemClock> clock, VideoCodecType type);
  ~HarmonyVideoDecoder() override;
  int Init() override;
  int Decode(std::unique_ptr<ReceivedFrame> frame, std::function<void(const DecodedFrame*)> callback) override;
  int GetResolution(int* width, int* height) const override;
  std::string GetDecoderName() const override;
  static bool Supported(VideoCodecType type);
 private:
  struct Pending { int64_t captured, received, submitted; std::function<void(const DecodedFrame*)> callback; };
  bool Configure(int width, int height);
  void StopHardware();
  int Fallback(const char* reason);
  int DecodeSoftware(std::unique_ptr<ReceivedFrame> frame, std::function<void(const DecodedFrame*)> callback);
  void Drain();
  bool Output(uint32_t index);
  std::shared_ptr<SystemClock> clock_;
  VideoCodecType type_;
  mutable std::mutex mutex_, pending_mutex_;
  H264BitstreamParser parser_;
  std::unique_ptr<MediaCodec> software_;
  OH_AVCodec* codec_ = nullptr;
  std::atomic<bool> stop_{true}, failed_{false};
  bool initialized_ = false;
  int configured_width_ = 0, configured_height_ = 0;
  std::atomic<int> width_{0}, height_{0};
  int64_t next_pts_ = 0;
  uint64_t hardware_frames_ = 0, software_frames_ = 0;
  std::map<int64_t, Pending> pending_;
  std::string name_;
  std::thread output_;
};
}

#endif
