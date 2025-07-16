#include "video_encoder_factory.h"

#include "aom/aom_av1_encoder.h"
#include "avt/svt_av1_encoder.h"
#include "openh264/openh264_encoder.h"

#if __APPLE__
#include "video_toolbox/video_toolbox_encoder.h"
#else
#include "nvcodec/nvidia_video_encoder.h"
#endif

#include "log.h"

VideoEncoderFactory::VideoEncoderFactory() {}

VideoEncoderFactory::~VideoEncoderFactory() {}

std::unique_ptr<MediaCodec> VideoEncoderFactory::CreateVideoEncoder(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    bool av1_encoding) {
  if (av1_encoding) {
    // return std::make_unique<AomAv1Encoder>(AomAv1Encoder(clock));
    return std::make_unique<SvtAv1Encoder>(SvtAv1Encoder(clock));
  } else {
#if __APPLE__
    if (hardware_acceleration) {
      return std::make_unique<VideoToolboxEncoder>(VideoToolboxEncoder(clock));
    } else {
      return std::make_unique<OpenH264Encoder>(OpenH264Encoder(clock));
    }
#else
    if (hardware_acceleration) {
      if (CheckIsHardwareAccerlerationSupported()) {
        return std::make_unique<NvidiaVideoEncoder>(NvidiaVideoEncoder(clock));
      } else {
        return nullptr;
      }
    } else {
      return std::make_unique<OpenH264Encoder>(OpenH264Encoder(clock));
    }
#endif
  }
}

bool VideoEncoderFactory::CheckIsHardwareAccerlerationSupported() {
#if __APPLE__
  is_hardware_acceleration_supported = false;
  return false;
#else
  return CheckIsCudaEncodeSupported();
#endif
}