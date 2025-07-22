#ifndef _VIDEO_ENCODER_FACTORY_H_
#define _VIDEO_ENCODER_FACTORY_H_

#include <memory>

#include "media_codec.h"

namespace minirtc {

class VideoEncoderFactory {
 public:
  VideoEncoderFactory();
  ~VideoEncoderFactory();

  static std::unique_ptr<MediaCodec> CreateVideoEncoder(
      std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
      bool av1_encoding);

  static bool CheckIsHardwareAccerlerationSupported();
};
}  // namespace minirtc

#endif