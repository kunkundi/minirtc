#include "video_decoder_factory.h"

#include "aom/aom_av1_decoder.h"
#include "dav1d/dav1d_av1_decoder.h"
#include "openh264/openh264_decoder.h"

#if __APPLE__
#include "video_toolbox/video_toolbox_decoder.h"
#else
#include "nvcodec/nvidia_video_decoder.h"
#endif

#include "log.h"

namespace minirtc {

VideoDecoderFactory::VideoDecoderFactory() {}

VideoDecoderFactory::~VideoDecoderFactory() {}

std::unique_ptr<MediaCodec> VideoDecoderFactory::CreateVideoDecoder(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    bool av1_encoding) {
  if (av1_encoding) {
    return std::make_unique<Dav1dAv1Decoder>(Dav1dAv1Decoder(clock));
    // return std::make_unique<AomAv1Decoder>(AomAv1Decoder(clock));
  } else {
#if __APPLE__
    if (hardware_acceleration) {
      return std::make_unique<VideoToolboxDecoder>(VideoToolboxDecoder(clock));
    } else {
      return std::make_unique<OpenH264Decoder>(OpenH264Decoder(clock));
    }
#else
    if (hardware_acceleration) {
      if (CheckIsHardwareAccerlerationSupported()) {
        return std::make_unique<NvidiaVideoDecoder>(NvidiaVideoDecoder(clock));
      } else {
        return nullptr;
      }
    } else {
      return std::make_unique<OpenH264Decoder>(OpenH264Decoder(clock));
    }
#endif
  }
}

bool VideoDecoderFactory::CheckIsHardwareAccerlerationSupported() {
#if __APPLE__
  return false;
#else
  return CheckIsCudaDecodeSupported();
#endif
}
}  // namespace minirtc