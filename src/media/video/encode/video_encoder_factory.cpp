#include "video_encoder_factory.h"

#include <exception>

#include "avt/svt_av1_encoder.h"
#if !defined(MINIRTC_IOS)
#include "openh264/openh264_encoder.h"
#endif

#if defined(_WIN32) || defined(_WIN64)
#include "wmf/wmf_video_encoder.h"
#elif defined(__APPLE__)
#include "video_toolbox/video_toolbox_encoder.h"
#endif

#if USE_CUDA && (defined(_WIN32) || (defined(__linux__) && defined(__x86_64__)))
#define MINIRTC_HAS_CUDA_ENCODER 1
#include "nvcodec/nvidia_video_encoder.h"
#else
#define MINIRTC_HAS_CUDA_ENCODER 0
#endif

#if defined(__ANDROID__)
#include "android_codec_support.h"
#include "mediacodec/android_video_encoder.h"
#endif

#include "log.h"

namespace minirtc {

VideoEncoderFactory::VideoEncoderFactory() {}

VideoEncoderFactory::~VideoEncoderFactory() {}

std::unique_ptr<MediaCodec> VideoEncoderFactory::TryInitEncoder(
    std::unique_ptr<MediaCodec> encoder, const MediaCodecConfig& config) {
  if (!encoder) return nullptr;
  std::string name = "requested encoder";
  try {
    name = encoder->GetEncoderName();
    if (encoder->Init(config) == 0) return encoder;
    LOG_WARN("Encoder [{}] initialization failed", name);
  } catch (const std::exception& error) {
    LOG_WARN("Encoder [{}] initialization threw: {}", name, error.what());
  } catch (...) {
    LOG_WARN("Encoder [{}] initialization threw an unknown exception", name);
  }
  return nullptr;
}

std::unique_ptr<MediaCodec> VideoEncoderFactory::CreateVideoEncoder(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    VideoCodecType codec_type) {
#if defined(__ANDROID__)
  if (hardware_acceleration && (codec_type == VideoCodecType::H264 ||
                                codec_type == VideoCodecType::AV1)) {
    return std::make_unique<AndroidVideoEncoder>(clock, codec_type);
  }
#endif
  if (codec_type == VideoCodecType::H265) {
    if (!hardware_acceleration) return nullptr;
#if MINIRTC_HAS_CUDA_ENCODER
    if (CheckIsCudaHevcEncodeSupported())
      return std::make_unique<NvidiaVideoEncoder>(clock, codec_type);
#endif
#if defined(_WIN32) || defined(_WIN64)
    if (WmfVideoEncoder::IsSupported(codec_type))
      return std::make_unique<WmfVideoEncoder>(clock, codec_type);
#elif defined(__APPLE__) && TARGET_OS_OSX
    if (CheckIsHardwareAccelerationSupported(codec_type))
      return std::make_unique<VideoToolboxEncoder>(clock, codec_type);
#endif
    return nullptr;
  }
  if (codec_type == VideoCodecType::AV1) {
    LOG_INFO("Using the SVT-AV1 software encoder");
    return std::make_unique<SvtAv1Encoder>(clock);
  }

  if (codec_type != VideoCodecType::H264) {
    LOG_ERROR("Unsupported video codec type [{}]",
              static_cast<int>(codec_type));
    return nullptr;
  }

#if defined(_WIN32) || defined(_WIN64)
  if (hardware_acceleration) {
#if USE_CUDA
    if (CheckIsCudaEncodeSupported()) {
      return std::make_unique<NvidiaVideoEncoder>(clock);
    }
#endif
    return std::make_unique<WmfVideoEncoder>(clock, codec_type);
  }
  return std::make_unique<OpenH264Encoder>(clock);
#elif defined(MINIRTC_IOS)
  if (!hardware_acceleration) {
    LOG_ERROR("Software H.264 encoding is unavailable on iOS");
    return nullptr;
  }
  return std::make_unique<VideoToolboxEncoder>(clock);
#elif defined(__APPLE__)
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
      return std::make_unique<NvidiaVideoEncoder>(clock);
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

std::unique_ptr<MediaCodec> VideoEncoderFactory::CreateInitializedVideoEncoder(
    std::shared_ptr<SystemClock> clock, const MediaCodecConfig& config,
    bool hardware_acceleration, VideoCodecType codec_type) {
#if defined(_WIN32) || defined(_WIN64)
  if (codec_type == VideoCodecType::H264 ||
      codec_type == VideoCodecType::H265) {
    if (hardware_acceleration) {
#if MINIRTC_HAS_CUDA_ENCODER
      if (codec_type == VideoCodecType::H265 ? CheckIsCudaHevcEncodeSupported()
                                             : CheckIsCudaEncodeSupported()) {
        if (auto encoder = TryInitEncoder(
                std::make_unique<NvidiaVideoEncoder>(clock, codec_type),
                config)) {
          return encoder;
        }
      }
#endif
      return CreateWindowsFallbackEncoder(clock, config, codec_type);
    }
    if (codec_type == VideoCodecType::H265) return nullptr;
    return TryInitEncoder(std::make_unique<OpenH264Encoder>(clock), config);
  }
#endif
  auto encoder = TryInitEncoder(
      CreateVideoEncoder(clock, hardware_acceleration, codec_type), config);
  if (encoder) {
    return encoder;
  }

#if defined(MINIRTC_IOS)
  // iOS has no software H.264 backend to fall back to.
  return nullptr;
#else
  // A negotiated AV1 stream must stay AV1. Only a failed hardware H.264
  // encoder can be transparently replaced with the software H.264 backend.
  if (!hardware_acceleration || codec_type != VideoCodecType::H264) {
    return nullptr;
  }

  LOG_WARN(
      "Hardware H.264 encoder initialization failed; falling back to OpenH264");
  return TryInitEncoder(
      CreateVideoEncoder(clock, false, VideoCodecType::H264), config);
#endif
}

#ifdef _WIN32
std::unique_ptr<MediaCodec> VideoEncoderFactory::CreateWindowsFallbackEncoder(
    std::shared_ptr<SystemClock> clock, const MediaCodecConfig& config,
    VideoCodecType codec_type) {
  if (auto encoder = TryInitEncoder(
          std::make_unique<WmfVideoEncoder>(clock, codec_type), config)) {
    return encoder;
  }
  if (codec_type != VideoCodecType::H264) return nullptr;
  return TryInitEncoder(std::make_unique<OpenH264Encoder>(clock), config);
}
#endif

bool VideoEncoderFactory::CheckIsHardwareAccelerationSupported(
    VideoCodecType codec_type) {
  if (codec_type == VideoCodecType::H265) {
#if MINIRTC_HAS_CUDA_ENCODER
    if (CheckIsCudaHevcEncodeSupported()) return true;
#endif
#if defined(_WIN32) || defined(_WIN64)
    return WmfVideoEncoder::IsSupported(codec_type);
#elif defined(__APPLE__) && TARGET_OS_OSX
    return VideoToolboxEncoder::IsHardwareHevcSupported();
#else
    return false;
#endif
  }
#if defined(__ANDROID__)
  return !FindAndroidCodecs(codec_type, true).empty();
#else
  if (codec_type != VideoCodecType::H264) {
    return false;
  }
#if defined(_WIN32) || defined(_WIN64)
#if USE_CUDA
  if (CheckIsCudaEncodeSupported()) return true;
#endif
  return WmfVideoEncoder::IsSupported(codec_type);
#elif defined(__APPLE__)
  return true;
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)) && \
    USE_CUDA
  return CheckIsCudaEncodeSupported();
#else
  return false;
#endif
#endif
}
}  // namespace minirtc
