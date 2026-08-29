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
    VideoCodecType codec_type) {
  if (codec_type == VideoCodecType::AV1) {
    LOG_INFO("VideoToolbox AV1 encoding is unavailable; using the SVT-AV1 "
             "encoder");
    return std::make_unique<SvtAv1Encoder>(clock);
  }

  if (codec_type != VideoCodecType::H264) {
    LOG_ERROR("Unsupported video codec type [{}]",
              static_cast<int>(codec_type));
    return nullptr;
  }

#if defined(__APPLE__)
  if (hardware_acceleration) {
    return std::make_unique<VideoToolboxEncoder>(clock);
  }
  LOG_INFO("Hardware H.264 encoding disabled; using the OpenH264 encoder");
  return std::make_unique<OpenH264Encoder>(clock);
#elif defined(__linux__) && defined(__aarch64__)
  return std::make_unique<OpenH264Encoder>(OpenH264Encoder(clock));
#else
#if USE_CUDA
  if (hardware_acceleration) {
    if (CheckIsHardwareAccelerationSupported(VideoCodecType::H264)) {
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

std::unique_ptr<MediaCodec>
VideoEncoderFactory::CreateInitializedVideoEncoder(
    std::shared_ptr<SystemClock> clock, const MediaCodecConfig& config,
    bool hardware_acceleration, VideoCodecType codec_type) {
  auto encoder = CreateVideoEncoder(clock, hardware_acceleration, codec_type);
  if (encoder && encoder->Init(config) == 0) {
    return encoder;
  }

  const std::string failed_encoder_name =
      encoder ? encoder->GetEncoderName() : "requested encoder";

  // A negotiated AV1 stream must stay AV1. Only a failed hardware H.264
  // encoder can be transparently replaced with the software H.264 backend.
  if (!hardware_acceleration || codec_type == VideoCodecType::AV1) {
    LOG_ERROR("Encoder [{}] initialization failed", failed_encoder_name);
    return nullptr;
  }

  LOG_WARN(
      "Hardware H.264 encoder [{}] initialization failed; falling back to "
      "OpenH264",
      failed_encoder_name);
  encoder = CreateVideoEncoder(clock, false, VideoCodecType::H264);
  if (!encoder || encoder->Init(config) != 0) {
    LOG_ERROR("OpenH264 fallback encoder initialization failed");
    return nullptr;
  }

  return encoder;
}

bool VideoEncoderFactory::CheckIsHardwareAccelerationSupported(
    VideoCodecType codec_type) {
  if (codec_type != VideoCodecType::H264) {
    return false;
  }
#if defined(__APPLE__)
  return true;
#elif ((defined(_WIN32) || defined(_WIN64)) ||                                 \
       (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)))) && \
    USE_CUDA
  return CheckIsCudaEncodeSupported();
#else
  return false;
#endif
}
}  // namespace minirtc
