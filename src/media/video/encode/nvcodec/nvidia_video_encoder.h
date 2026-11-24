#ifndef _NVIDIA_VIDEO_ENCODER_H_
#define _NVIDIA_VIDEO_ENCODER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "NvEncoderCuda.h"
#include "log.h"
#include "media_codec.h"
#include "nvcodec_common.h"

namespace minirtc {

#ifdef _WIN32
class NvEncoderD3D11;
#endif

class NvidiaVideoEncoder : public MediaCodec {
 public:
  NvidiaVideoEncoder(std::shared_ptr<SystemClock> clock,
                     VideoCodecType codec = VideoCodecType::H264);
  virtual ~NvidiaVideoEncoder();

  int Init(const MediaCodecConfig& config) override;

  int Encode(const RawFrame& raw_frame,
             std::function<int(const EncodedFrame& encoded_frame)>
                 on_encoded_image) override;

  int ForceIdr() override;
#ifdef _WIN32
  bool SupportsInputFrameRateUpdates() const override;
  int SetInputFrameRate(int fps) override;
  int SetTargetFrameRate(int fps) override;
#endif

  int SetTargetBitrate(int bitrate) override;

  bool SupportsNativeFrameInput(
      MiniRtcNativeVideoFrameType frame_type) const override {
#ifdef _WIN32
    if (frame_type == MiniRtcNativeVideoFrameD3D11Nv12) return directx_ != nullptr;
#endif
    return encoder_ && frame_type == MiniRtcNativeVideoFrameCpuNv12;
  }

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  // Separate input preparation (texture acquisition or upload) from the
  // encoder call (including the wait for completed bitstream output). Both
  // stages run synchronously inside Encode(), so the values describe the last
  // completed call.
  bool GetLastEncodeStageTimingUs(int64_t* upload_us,
                                  int64_t* submit_us) const override {
    if (!upload_us || !submit_us) return false;
    *upload_us = last_upload_us_.load(std::memory_order_relaxed);
    *submit_us = last_submit_us_.load(std::memory_order_relaxed);
    return *upload_us >= 0;
  }
#endif

  int GetResolution(int* width, int* height) const override {
#ifdef _WIN32
    if (auto fallback = std::atomic_load(&fallback_)) return fallback->GetResolution(width, height);
#endif
    if (!encoder_ || seq_ == 0) {
      return -1;
    }

    *width = frame_width_;
    *height = frame_height_;
    return 0;
  }

  std::string GetEncoderName() const override {
#ifdef _WIN32
    if (auto fallback = std::atomic_load(&fallback_)) return fallback->GetEncoderName();
#endif
    return codec_guid_ == NV_ENC_CODEC_HEVC_GUID ? "NVENC (H.265)"
                                                 : "NVENC (H.264)";
  }

 private:
  int EncodeHardware(const RawFrame& frame,
                     const std::function<int(const EncodedFrame&)>& callback);
  int InitBackend(const MediaCodecConfig& config, bool use_directx);
  MediaCodecConfig CurrentConfig(unsigned width, unsigned height) const;
  void ReleaseEncoderResources();
  int ResetEncodeResolution(unsigned int width, unsigned int height);

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  int index_of_gpu_ = 0;
  CUdevice cuda_device_ = 0;

#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  // Microseconds spent in the two synchronous stages of the last Encode().
  std::atomic<int64_t> last_upload_us_{-1};
  std::atomic<int64_t> last_submit_us_{0};
#endif

  const GUID codec_guid_;
  GUID preset_guid_ = NV_ENC_PRESET_P7_GUID;
  NV_ENC_TUNING_INFO tuning_info_ =
      NV_ENC_TUNING_INFO::NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  NV_ENC_BUFFER_FORMAT buffer_format_ =
      NV_ENC_BUFFER_FORMAT::NV_ENC_BUFFER_FORMAT_NV12;

  uint32_t frame_width_max_ = 0;
  uint32_t frame_height_max_ = 0;
  uint32_t frame_width_min_ = 0;
  uint32_t frame_height_min_ = 0;
  uint32_t encode_level_max_ = 0;
  uint32_t encode_level_min_ = 0;
  bool support_dynamic_resolution_ = false;
  bool support_dynamic_bitrate_ = false;

  uint32_t frame_width_ = 1280;
  uint32_t frame_height_ = 720;
  uint32_t key_frame_interval_ = 3000;
  uint32_t average_bitrate_ = MINIRTC_AVERAGE_BITRATE;
  uint32_t max_bitrate_ = kDefaultMaxEncoderBitrateBps;
  uint32_t max_fps_ = 60;
  MediaCodecConfig initial_config_;
  NvEncoder* encoder_ = nullptr;
#ifdef _WIN32
  std::shared_ptr<MediaCodec> fallback_;  // Published to the stats thread.
  int64_t retry_backend_at_ms_ = 0;
  NvEncoderD3D11* directx_ = nullptr;  // Owned by encoder_.
#endif
  CUcontext cuda_context_ = nullptr;
  std::vector<std::vector<uint8_t>> encoded_packets_;
  std::vector<uint32_t> encoded_packet_qps_;
  unsigned char* encoded_image_ = nullptr;
  FILE* file_nv12_ = nullptr;
  FILE* file_h264_ = nullptr;
  std::string h264_file_name_;
  std::string nv12_file_name_;
  unsigned char* nv12_data_ = nullptr;
  unsigned int seq_ = 0;
  bool next_frame_is_key_ = false;
};
}  // namespace minirtc

#endif
