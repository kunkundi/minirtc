#ifndef _NVIDIA_VIDEO_ENCODER_H_
#define _NVIDIA_VIDEO_ENCODER_H_

#include <functional>

#include "NvEncoderCuda.h"
#include "log.h"
#include "media_codec.h"
#include "nvcodec_common.h"

namespace minirtc {

class NvidiaVideoEncoder : public MediaCodec {
 public:
  NvidiaVideoEncoder(std::shared_ptr<SystemClock> clock);
  virtual ~NvidiaVideoEncoder();

  int Init(const MediaCodecConfig& config) override;

  int Encode(const RawFrame& raw_frame,
             std::function<int(const EncodedFrame& encoded_frame)>
                 on_encoded_image) override;

  int ForceIdr() override;

  int SetTargetBitrate(int bitrate) override;

  bool SupportsNativeFrameInput(
      MiniRtcNativeVideoFrameType frame_type) const override {
    return frame_type == MiniRtcNativeVideoFrameCpuNv12;
  }

  int GetResolution(int* width, int* height) const override {
    if (seq_ == 0) {
      return -1;
    }

    *width = frame_width_;
    *height = frame_height_;
    return 0;
  }

  std::string GetEncoderName() const override { return "NvidiaH264"; }

 private:
  void ReleaseEncoderResources();
  int ResetEncodeResolution(unsigned int width, unsigned int height);

 private:
  std::shared_ptr<SystemClock> clock_ = nullptr;
  int index_of_gpu_ = 0;
  CUdevice cuda_device_ = 0;

  GUID codec_guid_ = NV_ENC_CODEC_H264_GUID;
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
  int max_payload_size_ = 1150;
  NvEncoder* encoder_ = nullptr;
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
