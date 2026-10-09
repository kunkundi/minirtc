#include "video_decoder_factory.h"

#include "dav1d/dav1d_av1_decoder.h"
#if !defined(MINIRTC_IOS)
#include "openh264/openh264_decoder.h"
#endif

#if defined(__APPLE__)
#include <CoreMedia/CoreMedia.h>
#include <TargetConditionals.h>
#include <VideoToolbox/VideoToolbox.h>
#include "video_toolbox/video_toolbox_decoder.h"
#endif

#if USE_CUDA &&                                                          \
    (defined(_WIN32) || defined(_WIN64) ||                                \
     (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))))
#define MINIRTC_HAS_CUDA_DECODER 1
#include "nvcodec/nvidia_video_decoder.h"
#else
#define MINIRTC_HAS_CUDA_DECODER 0
#endif

#if defined(__ANDROID__)
#include "mediacodec/android_video_decoder.h"
#include "android_codec_support.h"
#endif

#include "log.h"

namespace minirtc {

VideoDecoderFactory::VideoDecoderFactory() {}

VideoDecoderFactory::~VideoDecoderFactory() {}

std::unique_ptr<MediaCodec>
VideoDecoderFactory::CreateVideoDecoder(std::shared_ptr<SystemClock> clock,
                                        bool hardware_acceleration,
                                        VideoCodecType codec_type,
                                        bool native_video_output) {
#if defined(__ANDROID__)
  if (hardware_acceleration &&
      (codec_type == VideoCodecType::H264 || codec_type == VideoCodecType::AV1)) {
    return std::make_unique<AndroidVideoDecoder>(clock, codec_type,
                                                 native_video_output);
  }
#endif
  if (codec_type == VideoCodecType::AV1) {
    if (hardware_acceleration) {
      LOG_INFO("Hardware AV1 decoding is not supported; using the dav1d "
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
#elif MINIRTC_HAS_CUDA_DECODER
  if (hardware_acceleration &&
      CheckIsHardwareAccelerationSupported(VideoCodecType::H264)) {
    return std::make_unique<NvidiaVideoDecoder>(clock, native_video_output);
  }
#endif
  return std::make_unique<OpenH264Decoder>(clock, native_video_output);
#endif
}

bool VideoDecoderFactory::CheckIsHardwareAccelerationSupported(
    VideoCodecType codec_type) {
#if defined(__ANDROID__)
  return !FindAndroidCodecs(codec_type, false).empty();
#else
  if (codec_type != VideoCodecType::H264) {
    return false;
  }
#if defined(__APPLE__)
  return VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
#elif MINIRTC_HAS_CUDA_DECODER
  return CheckIsCudaDecodeSupported();
#else
  return false;
#endif
#endif
}

} // namespace minirtc
