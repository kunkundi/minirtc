#include "video_encoder_factory.h"

#include "aom/aom_av1_encoder.h"
#include "avt/svt_av1_encoder.h"
#include "openh264/openh264_encoder.h"

#if (defined(_WIN32) || defined(_WIN64)) && USE_CUDA
#include "nvcodec/nvidia_video_encoder.h"
#elif defined(__APPLE__)
#include "video_toolbox/video_toolbox_encoder.h"
#elif defined(__linux__)
#if (defined(__x86_64__) || defined(__amd64__)) && USE_CUDA
#include "nvcodec/nvidia_video_encoder.h"
#elif defined(__aarch64__) || defined(__arm64__)
// use software encoder
#else
// use software encoder
#endif
#else
// use software encoder
#endif

#include "log.h"

namespace minirtc {

VideoEncoderFactory::VideoEncoderFactory() {}

VideoEncoderFactory::~VideoEncoderFactory() {}

std::unique_ptr<MediaCodec> VideoEncoderFactory::CreateVideoEncoder(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    bool av1_encoding) {
  if (av1_encoding) {
    // return std::make_unique<AomAv1Encoder>(AomAv1Encoder(clock));
    return std::make_unique<SvtAv1Encoder>(SvtAv1Encoder(clock));
  } else {
#if defined(__APPLE__)
    if (hardware_acceleration) {
      return std::make_unique<VideoToolboxEncoder>(VideoToolboxEncoder(clock));
    } else {
      return std::make_unique<OpenH264Encoder>(OpenH264Encoder(clock));
    }
#elif defined(__linux__) && defined(__aarch64__)
    return std::make_unique<OpenH264Encoder>(OpenH264Encoder(clock));
#else
#if USE_CUDA
    if (hardware_acceleration) {
      if (CheckIsHardwareAccerlerationSupported()) {
        return std::make_unique<NvidiaVideoEncoder>(NvidiaVideoEncoder(clock));
      } else {
        return nullptr;
      }
    } else {
#endif
      return std::make_unique<OpenH264Encoder>(OpenH264Encoder(clock));
#if USE_CUDA
    }
#endif
#endif
  }
}

bool VideoEncoderFactory::CheckIsHardwareAccerlerationSupported() {
#if defined(__APPLE__)
  return false;
#elif ((defined(_WIN32) || defined(_WIN64)) ||                                 \
       (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)))) && \
    USE_CUDA
  return CheckIsCudaEncodeSupported();
#else
  return false;
#endif
}
}  // namespace minirtc