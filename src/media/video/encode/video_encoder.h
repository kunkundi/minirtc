#ifndef _VIDEO_ENCODER_H_
#define _VIDEO_ENCODER_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "clock/system_clock.h"
#include "encoded_frame.h"
#include "media_codec.h"
#include "minirtc.h"
#include "raw_frame.h"

#define I_FRAME_INTERVAL 3000
class VideoEncoder : public MediaCodec {
 public:
  virtual int Init() = 0;

  virtual int Encode(const RawFrame& raw_frame,
                     std::function<int(const EncodedFrame& encoded_frame)>
                         on_encoded_image) = 0;

  virtual int ForceIdr() = 0;

  virtual int SetTargetBitrate(int bitrate) = 0;

  virtual int GetResolution(int* width, int* height) = 0;

  virtual std::string GetEncoderName() = 0;

  VideoEncoder() = default;
  virtual ~VideoEncoder() {}
};

#endif