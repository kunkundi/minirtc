#include "video_decoder_factory.h"

#include "aom/aom_av1_decoder.h"
#include "dav1d/dav1d_av1_decoder.h"
#include "openh264/openh264_decoder.h"

#if (defined(_WIN32) || defined(_WIN64)) && USE_CUDA
#include "nvcodec/nvidia_video_decoder.h"
#elif defined(__APPLE__)
#include "video_toolbox/video_toolbox_decoder.h"
#elif defined(__linux__)
#if (defined(__x86_64__) || defined(__amd64__)) && USE_CUDA
#include "nvcodec/nvidia_video_decoder.h"
#elif defined(__aarch64__) || defined(__arm64__)
#else
// use software encoder
#endif
#else
// use software encoder
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
#if defined(__APPLE__)
    if (hardware_acceleration) {
      return std::make_unique<VideoToolboxDecoder>(VideoToolboxDecoder(clock));
    } else {
      return std::make_unique<OpenH264Decoder>(OpenH264Decoder(clock));
    }
#elif defined(__linux__) && defined(__aarch64__)
    return std::make_unique<OpenH264Decoder>(OpenH264Decoder(clock));
#else
#if USE_CUDA
    if (hardware_acceleration) {
      if (CheckIsHardwareAccerlerationSupported()) {
        return std::make_unique<NvidiaVideoDecoder>(NvidiaVideoDecoder(clock));
      } else {
        return nullptr;
      }
    } else {
#endif
      return std::make_unique<OpenH264Decoder>(OpenH264Decoder(clock));
#if USE_CUDA
    }
#endif
#endif
  }
}

bool VideoDecoderFactory::CheckIsHardwareAccerlerationSupported() {
#if defined(__APPLE__)
  return false;
#elif ((defined(_WIN32) || defined(_WIN64)) ||                                 \
       (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)))) && \
    USE_CUDA
  return CheckIsCudaDecodeSupported();
#else
  return false;
#endif
}
}  // namespace minirtc