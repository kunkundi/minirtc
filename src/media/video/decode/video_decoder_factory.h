#ifndef _VIDEO_DECODER_FACTORY_H_
#define _VIDEO_DECODER_FACTORY_H_

#include <memory>

#include "media_codec.h"

namespace minirtc {
class VideoDecoderFactory {
 public:
  VideoDecoderFactory();
  ~VideoDecoderFactory();

  static std::unique_ptr<MediaCodec> CreateVideoDecoder(
      std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
      bool av1_encoding);

  static bool CheckIsHardwareAccerlerationSupported();
};
}  // namespace minirtc

#endif