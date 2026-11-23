#include "video_decoder_factory.h"

#include <exception>

#include "dav1d/dav1d_av1_decoder.h"
#if defined(_WIN32) || defined(_WIN64)
#include "wmf/d3d11_h264_decoder.h"
#endif
#if !defined(MINIRTC_IOS)
#include "openh264/openh264_decoder.h"
#endif

#if defined(__APPLE__)
#include <CoreMedia/CoreMedia.h>
#include <TargetConditionals.h>
#include <VideoToolbox/VideoToolbox.h>

#include "video_toolbox/video_toolbox_decoder.h"
#endif

#if USE_CUDA &&                            \
    (defined(_WIN32) || defined(_WIN64) || \
     (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))))
#define MINIRTC_HAS_CUDA_DECODER 1
#include "nvcodec/nvidia_video_decoder.h"
#else
#define MINIRTC_HAS_CUDA_DECODER 0
#endif

#if defined(__ANDROID__)
#include "android_codec_support.h"
#include "mediacodec/android_video_decoder.h"
#endif

#include "log.h"
#if defined(__OHOS__)
#include "harmony/harmony_video_decoder.h"
#endif

namespace minirtc {

VideoDecoderFactory::VideoDecoderFactory() {}

VideoDecoderFactory::~VideoDecoderFactory() {}

std::unique_ptr<MediaCodec> VideoDecoderFactory::TryInitDecoder(
    std::unique_ptr<MediaCodec> decoder) {
  if (!decoder) return nullptr;
  std::string name = "requested decoder";
  try {
    name = decoder->GetDecoderName();
    if (decoder->Init() == 0) return decoder;
    LOG_WARN("Decoder [{}] initialization failed", name);
  } catch (const std::exception& error) {
    LOG_WARN("Decoder [{}] initialization threw: {}", name, error.what());
  } catch (...) {
    LOG_WARN("Decoder [{}] initialization threw an unknown exception", name);
  }
  return nullptr;
}

std::unique_ptr<MediaCodec> VideoDecoderFactory::CreateVideoDecoder(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    VideoCodecType codec_type, bool native_video_output) {
#if defined(__OHOS__)
  if (hardware_acceleration &&
      (codec_type == VideoCodecType::H264 || codec_type == VideoCodecType::AV1))
    return std::make_unique<HarmonyVideoDecoder>(clock, codec_type);
#endif
#if defined(__ANDROID__)
  if (hardware_acceleration && (codec_type == VideoCodecType::H264 ||
                                codec_type == VideoCodecType::AV1)) {
    return std::make_unique<AndroidVideoDecoder>(clock, codec_type,
                                                 native_video_output);
  }
#endif
  if (codec_type == VideoCodecType::AV1) {
    if (hardware_acceleration) {
      LOG_INFO(
          "Hardware AV1 decoding is not supported; using the dav1d "
          "decoder");
    }
    // Software decoders disable native output on unsupported platforms.
    return std::make_unique<Dav1dAv1Decoder>(clock, native_video_output);
  }

  if (codec_type != VideoCodecType::H264) {
    LOG_ERROR("Unsupported video codec type [{}]",
              static_cast<int>(codec_type));
    return nullptr;
  }

#if defined(MINIRTC_IOS)
  // Simulator VideoToolbox can decode in software without hardware support.
#if !TARGET_OS_SIMULATOR
  if (!hardware_acceleration ||
      !CheckIsHardwareAccelerationSupported(VideoCodecType::H264)) {
    LOG_ERROR("VideoToolbox H.264 decoding is unavailable or disabled on iOS");
    return nullptr;
  }
#endif
  return std::make_unique<VideoToolboxDecoder>(clock, native_video_output);
#else
#if defined(__APPLE__)
  if (hardware_acceleration &&
      CheckIsHardwareAccelerationSupported(VideoCodecType::H264)) {
    return std::make_unique<VideoToolboxDecoder>(clock, native_video_output);
  }
  LOG_INFO("Hardware H.264 decoding {}; using the OpenH264 decoder",
           hardware_acceleration ? "is unavailable" : "is disabled");
#elif defined(_WIN32) || defined(_WIN64)
  if (hardware_acceleration) {
#if MINIRTC_HAS_CUDA_DECODER
    if (CheckIsCudaDecodeSupported())
      return std::make_unique<NvidiaVideoDecoder>(clock, native_video_output);
#endif
    return std::make_unique<D3D11H264Decoder>(clock, native_video_output);
  }
#elif MINIRTC_HAS_CUDA_DECODER
  if (hardware_acceleration &&
      CheckIsHardwareAccelerationSupported(VideoCodecType::H264)) {
    return std::make_unique<NvidiaVideoDecoder>(clock, native_video_output);
  }
#endif
  return std::make_unique<OpenH264Decoder>(clock, native_video_output);
#endif
}

std::unique_ptr<MediaCodec> VideoDecoderFactory::CreateInitializedVideoDecoder(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration,
    VideoCodecType codec_type, bool native_video_output) {
#if defined(_WIN32) || defined(_WIN64)
  if (codec_type == VideoCodecType::H264) {
    if (hardware_acceleration) {
#if MINIRTC_HAS_CUDA_DECODER
      if (CheckIsCudaDecodeSupported()) {
        if (auto decoder =
                TryInitDecoder(std::make_unique<NvidiaVideoDecoder>(
                    clock, native_video_output))) {
          return decoder;
        }
      }
#endif
      if (auto decoder = TryInitDecoder(
              std::make_unique<D3D11H264Decoder>(clock, native_video_output))) {
        return decoder;
      }
    }
    return TryInitDecoder(
        std::make_unique<OpenH264Decoder>(clock, native_video_output));
  }
#endif
  auto decoder = TryInitDecoder(CreateVideoDecoder(
      clock, hardware_acceleration, codec_type, native_video_output));
  if (decoder) return decoder;
#if !defined(MINIRTC_IOS)
  if (hardware_acceleration && codec_type == VideoCodecType::H264) {
    return TryInitDecoder(
        CreateVideoDecoder(clock, false, codec_type, native_video_output));
  }
#endif
  return nullptr;
}

bool VideoDecoderFactory::CheckIsHardwareAccelerationSupported(
    VideoCodecType codec_type) {
#if defined(__OHOS__)
  return HarmonyVideoDecoder::Supported(codec_type);
#endif
#if defined(__ANDROID__)
  return !FindAndroidCodecs(codec_type, false).empty();
#else
  if (codec_type != VideoCodecType::H264) {
    return false;
  }
#if defined(_WIN32) || defined(_WIN64)
#if MINIRTC_HAS_CUDA_DECODER
  if (CheckIsCudaDecodeSupported()) return true;
#endif
  return D3D11H264Decoder::IsSupported();
#elif defined(__APPLE__)
  return VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
#elif MINIRTC_HAS_CUDA_DECODER
  return CheckIsCudaDecodeSupported();
#else
  return false;
#endif
#endif
}

}  // namespace minirtc
