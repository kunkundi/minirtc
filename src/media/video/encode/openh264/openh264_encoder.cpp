#include "openh264_encoder.h"

#include <chrono>

#include "libyuv.h"
#include "log.h"

// #define SAVE_RECEIVED_NV12_STREAM
// #define SAVE_ENCODED_H264_STREAM

namespace minirtc {

static void Nv12ToI420(unsigned char* Src_data, int src_width, int src_height,
                       unsigned char* Dst_data) {
  // NV12
  int NV12_Y_Size = src_width * src_height;

  // YUV420
  int I420_Y_Size = src_width * src_height;
  int I420_U_Size = (src_width >> 1) * (src_height >> 1);
  int I420_V_Size = I420_U_Size;

  // src: buffer address of Y channel and UV channel
  unsigned char* Y_data_Src = Src_data;
  unsigned char* UV_data_Src = Src_data + NV12_Y_Size;
  int src_stride_y = src_width;
  int src_stride_uv = src_width;

  // dst: buffer address of Y channel、U channel and V channel
  unsigned char* Y_data_Dst = Dst_data;
  unsigned char* U_data_Dst = Dst_data + I420_Y_Size;
  unsigned char* V_data_Dst = Dst_data + I420_Y_Size + I420_V_Size;
  int Dst_Stride_Y = src_width;
  int Dst_Stride_U = src_width >> 1;
  int Dst_Stride_V = Dst_Stride_U;

  libyuv::NV12ToI420(
      (const uint8_t*)Y_data_Src, src_stride_y, (const uint8_t*)UV_data_Src,
      src_stride_uv, (uint8_t*)Y_data_Dst, Dst_Stride_Y, (uint8_t*)U_data_Dst,
      Dst_Stride_U, (uint8_t*)V_data_Dst, Dst_Stride_V, src_width, src_height);
}

OpenH264Encoder::OpenH264Encoder(std::shared_ptr<SystemClock> clock)
    : clock_(clock) {}

OpenH264Encoder::~OpenH264Encoder() {
#ifdef SAVE_RECEIVED_NV12_STREAM
  if (file_nv12_) {
    fflush(file_nv12_);
    fclose(file_nv12_);
    file_nv12_ = nullptr;
  }
#endif

#ifdef SAVE_ENCODED_H264_STREAM
  if (file_h264_) {
    fflush(file_h264_);
    fclose(file_h264_);
    file_h264_ = nullptr;
  }
#endif

  if (yuv420p_frame_) {
    delete[] yuv420p_frame_;
    yuv420p_frame_ = nullptr;
  }

  Release();
}

int OpenH264Encoder::InitEncoderParams(int width, int height) {
  int ret = 0;

  if (!openh264_encoder_) {
    LOG_ERROR("Invalid openh264 encoder");
    return -1;
  }

  ret = openh264_encoder_->GetDefaultParams(&encoder_params_);
  // if (codec_.mode == VideoCodecMode::kRealtimeVideo) {  //
  encoder_params_.iUsageType = CAMERA_VIDEO_REAL_TIME;
  // } else if (codec_.mode == VideoCodecMode::kScreensharing) {
  // encoder_params_.iUsageType = SCREEN_CONTENT_REAL_TIME;
  // }

  encoder_params_.iPicWidth = width;
  encoder_params_.iPicHeight = height;
  encoder_params_.iTargetBitrate = target_bitrate_;
  encoder_params_.iMaxBitrate = UNSPECIFIED_BIT_RATE;
  encoder_params_.iRCMode = RC_BITRATE_MODE;
  encoder_params_.fMaxFrameRate = max_fps_;
  encoder_params_.bEnableFrameSkip = true;
  encoder_params_.uiIntraPeriod = key_frame_interval_;
  encoder_params_.eSpsPpsIdStrategy = SPS_LISTING;
  encoder_params_.uiMaxNalSize = 0;
  encoder_params_.iMaxQp = 37;
  encoder_params_.iMinQp = 24;
  // Threading model: use auto.
  //  0: auto (dynamic imp. internal encoder)
  //  1: single thread (default value)
  // >1: number of threads
  encoder_params_.iMultipleThreadIdc =
      std::min((int)std::thread::hardware_concurrency() / 2, 8);
  // The base spatial layer 0 is the only one we use.
  encoder_params_.sSpatialLayers[0].iVideoWidth = encoder_params_.iPicWidth;
  encoder_params_.sSpatialLayers[0].iVideoHeight = encoder_params_.iPicHeight;
  encoder_params_.sSpatialLayers[0].fFrameRate = encoder_params_.fMaxFrameRate;
  encoder_params_.sSpatialLayers[0].iSpatialBitrate =
      encoder_params_.iTargetBitrate;
  encoder_params_.sSpatialLayers[0].iMaxSpatialBitrate =
      encoder_params_.iMaxBitrate;
  encoder_params_.sSpatialLayers[0].uiLevelIdc = LEVEL_5_2;

  // SingleNalUnit
  encoder_params_.sSpatialLayers[0].sSliceArgument.uiSliceNum = 0;
  encoder_params_.sSpatialLayers[0].sSliceArgument.uiSliceMode =
      SM_FIXEDSLCNUM_SLICE;  // enabled dynamic slicing for multi-thread
  // encoder_params_.sSpatialLayers[0].sSliceArgument.uiSliceSizeConstraint =
  //     static_cast<unsigned int>(max_payload_size_);

  return ret;
}

int OpenH264Encoder::ResetEncodeResolution(unsigned int width,
                                           unsigned int height) {
  frame_width_ = width;
  frame_height_ = height;

  if (openh264_encoder_) {
    openh264_encoder_->Uninitialize();
  } else {
    LOG_ERROR("Invalid openh264 encoder");
    return -1;
  }

  if (0 != InitEncoderParams(width, height)) {
    LOG_ERROR("Reset encoder params [{}x{}] failed", width, height);
    return -1;
  }

  if (0 != openh264_encoder_->InitializeExt(&encoder_params_)) {
    LOG_ERROR("Reset encoder resolution [{}x{}] failed", width, height);
    return -1;
  }

  return 0;
}

int OpenH264Encoder::Init() {
  // Create encoder.
  if (WelsCreateSVCEncoder(&openh264_encoder_) != 0) {
    LOG_ERROR("Failed to create OpenH264 encoder");
    return -1;
  }

  long ret = -1;
  int trace_level = WELS_LOG_QUIET;
  ret = openh264_encoder_->SetOption(ENCODER_OPTION_TRACE_LEVEL, &trace_level);
  if (ret != cmResultSuccess) {
    LOG_ERROR("Failed to set encoder trace level");
  }

  // Create encoder parameters based on the layer configuration.
  InitEncoderParams(frame_width_, frame_height_);

  ret = openh264_encoder_->InitializeExt(&encoder_params_);
  if (ret != cmResultSuccess) {
    LOG_ERROR("Failed to initialize OpenH264 encoder");
    return -1;
  }

  video_format_ = EVideoFormatType::videoFormatI420;
  ret = openh264_encoder_->SetOption(ENCODER_OPTION_DATAFORMAT, &video_format_);
  if (ret != cmResultSuccess) {
    LOG_ERROR("Failed to set encoder data format");
    return -1;
  }

#ifdef SAVE_RECEIVED_NV12_STREAM
  nv12_file_name_ = "received_nv12_stream_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yuv";
  file_nv12_ = fopen(nv12_file_name_.c_str(), "w+b");
  if (!file_nv12_) {
    LOG_WARN("Fail to open {}", nv12_file_name_.c_str());
  }
#endif

#ifdef SAVE_ENCODED_H264_STREAM
  h264_file_name_ = "encoded_h264_stream_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".h264";
  file_h264_ = fopen(h264_file_name_.c_str(), "w+b");
  if (!file_h264_) {
    LOG_WARN("Fail to open {}", h264_file_name_.c_str());
  }
#endif

  return 0;
}

int OpenH264Encoder::Encode(
    const RawFrame& raw_frame,
    std::function<int(const EncodedFrame& encoded_frame)> on_encoded_image) {
  if (!openh264_encoder_) {
    LOG_ERROR("Invalid openh264 encoder");
    return -1;
  }

#ifdef SAVE_RECEIVED_NV12_STREAM
  fwrite(raw_frame.Buffer(), 1, raw_frame.Size(), file_nv12_);
#endif

  if (!yuv420p_frame_) {
    yuv420p_frame_capacity_ = raw_frame.Size();
    yuv420p_frame_ = new unsigned char[yuv420p_frame_capacity_];
  }

  if (yuv420p_frame_capacity_ < raw_frame.Size()) {
    yuv420p_frame_capacity_ = raw_frame.Size();
    delete[] yuv420p_frame_;
    yuv420p_frame_ = new unsigned char[yuv420p_frame_capacity_];
  }

  if (raw_frame.Width() != frame_width_ ||
      raw_frame.Height() != frame_height_) {
    ResetEncodeResolution(raw_frame.Width(), raw_frame.Height());
  }

  Nv12ToI420((unsigned char*)raw_frame.Buffer(), raw_frame.Width(),
             raw_frame.Height(), yuv420p_frame_);

  VideoFrameType frame_type;
  if (0 == seq_++ % key_frame_interval_) {
    ForceIdr();
    frame_type = VideoFrameType::kVideoFrameKey;
  } else {
    frame_type = VideoFrameType::kVideoFrameDelta;
  }

  raw_frame_ = {0};
  raw_frame_.iPicWidth = encoder_params_.iPicWidth;
  raw_frame_.iPicHeight = encoder_params_.iPicHeight;
  raw_frame_.iColorFormat = EVideoFormatType::videoFormatI420;
  raw_frame_.uiTimeStamp = raw_frame.CapturedTimestamp();

  raw_frame_.iStride[0] = encoder_params_.iPicWidth;
  raw_frame_.iStride[1] = raw_frame_.iStride[2] =
      encoder_params_.iPicWidth >> 1;
  raw_frame_.pData[0] = (unsigned char*)yuv420p_frame_;
  raw_frame_.pData[1] = raw_frame_.pData[0] +
                        encoder_params_.iPicWidth * encoder_params_.iPicHeight;
  raw_frame_.pData[2] =
      raw_frame_.pData[1] +
      (encoder_params_.iPicWidth * encoder_params_.iPicHeight >> 2);

  SFrameBSInfo info;
  memset(&info, 0, sizeof(SFrameBSInfo));

  int enc_ret = openh264_encoder_->EncodeFrame(&raw_frame_, &info);
  if (enc_ret != 0) {
    LOG_ERROR("OpenH264 frame encoding failed, EncodeFrame returned {}",
              enc_ret);

    return -1;
  }

  if (info.eFrameType == videoFrameTypeSkip) {
    return 0;
  }

  for (int layer = 0; layer < info.iLayerNum; ++layer) {
    const SLayerBSInfo& layerInfo = info.sLayerInfo[layer];
    size_t layer_len = 0;
    for (int nal = 0; nal < layerInfo.iNalCount; ++nal) {
      layer_len += layerInfo.pNalLengthInByte[nal];
    }

    if (on_encoded_image) {
      EncodedFrame encoded_frame(info.sLayerInfo[layer].pBsBuf, layer_len,
                                 raw_frame_.iPicWidth, raw_frame_.iPicHeight);
      encoded_frame.SetFrameType(frame_type);
      encoded_frame.SetEncodedWidth(raw_frame_.iPicWidth);
      encoded_frame.SetEncodedHeight(raw_frame_.iPicHeight);
      encoded_frame.SetCapturedTimestamp(raw_frame.CapturedTimestamp());
      encoded_frame.SetEncodedTimestamp(clock_->CurrentTime());
      on_encoded_image(encoded_frame);
#ifdef SAVE_ENCODED_H264_STREAM
      fwrite(encoded_frame.Buffer(), 1, encoded_frame.Size(), file_h264_);
#endif
    }
  }

  return 0;
}

int OpenH264Encoder::ForceIdr() {
  if (openh264_encoder_) {
    return openh264_encoder_->ForceIntraFrame(true);
  }

  return 0;
}

int OpenH264Encoder::SetTargetBitrate(int bitrate) {
  if (!openh264_encoder_) {
    return -1;
  }

  target_bitrate_ = bitrate;
  encoder_params_.iTargetBitrate = target_bitrate_;

  SBitrateInfo target_bitrate;
  memset(&target_bitrate, 0, sizeof(SBitrateInfo));
  target_bitrate.iLayer = SPATIAL_LAYER_ALL, target_bitrate.iBitrate = bitrate;
  return openh264_encoder_->SetOption(ENCODER_OPTION_BITRATE, &target_bitrate);
}

int OpenH264Encoder::Release() {
  if (openh264_encoder_) {
    openh264_encoder_->Uninitialize();
    WelsDestroySVCEncoder(openh264_encoder_);
    openh264_encoder_ = nullptr;
  }

  return 0;
}
}  // namespace minirtc