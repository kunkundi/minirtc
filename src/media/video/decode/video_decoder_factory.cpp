#include "video_decoder_factory.h"

#include "aom/aom_av1_decoder.h"
#include "dav1d/dav1d_av1_decoder.h"
#include "openh264/openh264_decoder.h"

#if defined(__APPLE__)
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#include "wmf/wmf_h264_software_decoder.h"
#endif

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

std::unique_ptr<MediaCodec>
VideoDecoderFactory::CreateVideoDecoder(std::shared_ptr<SystemClock> clock,
                                        bool hardware_acceleration,
                                        VideoCodecType codec_type,
                                        bool native_video_output) {
#if !defined(__APPLE__)
  (void)native_video_output;
#endif
  if (codec_type == VideoCodecType::AV1) {
    if (hardware_acceleration) {
      LOG_INFO("Hardware AV1 decoding is not supported; using the dav1d "
               "decoder");
    }
    return std::make_unique<Dav1dAv1Decoder>(clock);
    // return std::make_unique<AomAv1Decoder>(clock);
  }

  if (codec_type != VideoCodecType::H264) {
    LOG_ERROR("Unsupported video codec type [{}]",
              static_cast<int>(codec_type));
    return nullptr;
  }

#if defined(__APPLE__)
  if (hardware_acceleration &&
      CheckIsHardwareAccelerationSupported(VideoCodecType::H264)) {
    return std::make_unique<VideoToolboxDecoder>(clock, native_video_output);
  }
  LOG_INFO("Hardware H.264 decoding {}; using the OpenH264 decoder",
           hardware_acceleration ? "is unavailable" : "is disabled");
  return std::make_unique<OpenH264Decoder>(clock);
#elif defined(__linux__) && defined(__aarch64__)
  return std::make_unique<OpenH264Decoder>(clock);
#else
#if USE_CUDA
  if (hardware_acceleration) {
    if (CheckIsHardwareAccelerationSupported(VideoCodecType::H264)) {
      return std::make_unique<NvidiaVideoDecoder>(clock);
    } else {
      // Hardware requested but not supported: fallback to software.
      return std::make_unique<OpenH264Decoder>(clock);
    }
  } else {
#endif
    return std::make_unique<OpenH264Decoder>(clock);
#if USE_CUDA
  }
#endif
#endif
}

bool VideoDecoderFactory::CheckIsHardwareAccelerationSupported(
    VideoCodecType codec_type) {
  if (codec_type != VideoCodecType::H264) {
    return false;
  }
#if defined(__APPLE__)
  return VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
#elif ((defined(_WIN32) || defined(_WIN64)) ||                                 \
       (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)))) && \
    USE_CUDA
  return CheckIsCudaDecodeSupported();
#else
  return false;
#endif
}

} // namespace minirtc
