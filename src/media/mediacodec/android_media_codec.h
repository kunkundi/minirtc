/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-02
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ANDROID_MEDIA_CODEC_H_
#define _ANDROID_MEDIA_CODEC_H_

#include <condition_variable>

#include "h264_bitstream_parser.h"
#include "media_codec.h"

namespace minirtc {
struct AndroidCodecState;
// Internal NDK backend shared by the video encoder and decoder adapters.
// Input methods and bitrate changes are serialized; output is drained
// independently of input. Software fallback belongs to the adapters.
class AndroidMediaCodec final {
 public:
  enum class Mode { Encoder, Decoder };
  enum class Result {
    Ok,
    InvalidInput,
    NeedKeyFrame,
    ConfigureFailed,
    CodecError,
  };

  AndroidMediaCodec(std::shared_ptr<SystemClock> clock, VideoCodecType type,
                    Mode mode, bool native_output = false);
  ~AndroidMediaCodec();
  int InitEncoder(const MediaCodecConfig& config);
  int InitDecoder();
  Result Encode(const RawFrame& frame,
                std::function<int(const EncodedFrame&)> callback);
  Result Decode(const ReceivedFrame& frame,
                std::function<void(const DecodedFrame*)> callback);
  int ForceIdr();
  int SetTargetBitrate(int bitrate);
  int GetResolution(int* width, int* height) const;
  std::string GetName() const;
  // Invalidate retained Surface frames before an adapter switches to software.
  void Stop();
  static bool IsValidSize(int width, int height);

 private:
  bool Configure(int width, int height);
  void DrainLoop();
  void StopCodec();
  std::shared_ptr<SystemClock> clock_;
  VideoCodecType type_;
  const bool encoder_, native_output_;
  MediaCodecConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread drain_thread_;
  std::shared_ptr<AndroidCodecState> state_;
  H264BitstreamParser parser_;
  int width_ = 0, height_ = 0;
  int64_t next_pts_ = 0;
  uint64_t input_sequence_ = 0;
};
}  // namespace minirtc

#endif