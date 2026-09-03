#include "nvidia_video_encoder.h"

#include <chrono>

#include "log.h"

// #define SAVE_RECEIVED_NV12_STREAM
// #define SAVE_ENCODED_H264_STREAM

namespace minirtc {

NvidiaVideoEncoder::NvidiaVideoEncoder(std::shared_ptr<SystemClock> clock)
    : clock_(clock) {}
NvidiaVideoEncoder::~NvidiaVideoEncoder() {
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

  if (nv12_data_) {
    free(nv12_data_);
    nv12_data_ = nullptr;
  }

  ReleaseEncoderResources();
}

void NvidiaVideoEncoder::ReleaseEncoderResources() {
  if (encoder_) {
    encoder_->DestroyEncoder();
    delete encoder_;
    encoder_ = nullptr;
  }

  if (cuda_context_) {
    cuCtxDestroy_ld(cuda_context_);
    cuda_context_ = nullptr;
  }
}

int NvidiaVideoEncoder::Init(const MediaCodecConfig& config) {
  frame_width_ = config.init_width;
  frame_height_ = config.init_height;
  key_frame_interval_ = config.key_frame_interval;
  max_bitrate_ = config.max_bitrate;
  average_bitrate_ = ClampEncoderTargetBitrate(
      config.average_bitrate, static_cast<int>(max_bitrate_));
  max_fps_ = config.max_frame_rate;
  max_payload_size_ = config.max_payload_size;
  preset_guid_ =
      config.video_degradation_preference ==
              VideoDegradationPreference::MaintainFrameRate
          ? NV_ENC_PRESET_P1_GUID
          : NV_ENC_PRESET_P7_GUID;
  tuning_info_ =
      config.video_degradation_preference ==
              VideoDegradationPreference::MaintainFrameRate
          ? NV_ENC_TUNING_INFO::NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY
          : NV_ENC_TUNING_INFO::NV_ENC_TUNING_INFO_LOW_LATENCY;

  try {
    CudaInitializer::Init();
    int num_of_gpu = 0;
    ck(cuDeviceGetCount_ld(&num_of_gpu));
    if (index_of_gpu_ < 0 || index_of_gpu_ >= num_of_gpu) {
      LOG_ERROR("GPU ordinal out of range. Should be within [0-{}]");
      return -1;
    }

    ck(cuDeviceGet_ld(&cuda_device_, index_of_gpu_));
    char device_name[80];
    ck(cuDeviceGetName_ld(device_name, sizeof(device_name), cuda_device_));
    LOG_INFO("H.264 encoder using [{}]", device_name);
    ck(cuCtxCreate_ld(&cuda_context_, 0, cuda_device_));

    encoder_ = new NvEncoderCuda(cuda_context_, frame_width_, frame_height_,
                                 buffer_format_, 0, false, false);

    NV_ENC_INITIALIZE_PARAMS init_params = {NV_ENC_INITIALIZE_PARAMS_VER};
    NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
    init_params.encodeConfig = &encodeConfig;
    encoder_->CreateDefaultEncoderParams(&init_params, codec_guid_,
                                         preset_guid_, tuning_info_);

    frame_width_max_ = encoder_->GetCapabilityValue(NV_ENC_CODEC_H264_GUID,
                                                    NV_ENC_CAPS_WIDTH_MAX);
    frame_height_max_ = encoder_->GetCapabilityValue(NV_ENC_CODEC_H264_GUID,
                                                     NV_ENC_CAPS_HEIGHT_MAX);
    // frame_width_min_ = encoder_->GetCapabilityValue(NV_ENC_CODEC_H264_GUID,
    //                                                 NV_ENC_CAPS_WIDTH_MIN);
    // frame_height_min_ = encoder_->GetCapabilityValue(NV_ENC_CODEC_H264_GUID,
    //                                                  NV_ENC_CAPS_HEIGHT_MIN);
    encode_level_max_ = encoder_->GetCapabilityValue(NV_ENC_CODEC_H264_GUID,
                                                     NV_ENC_CAPS_LEVEL_MAX);
    encode_level_min_ = encoder_->GetCapabilityValue(NV_ENC_CODEC_H264_GUID,
                                                     NV_ENC_CAPS_LEVEL_MIN);
    support_dynamic_resolution_ = encoder_->GetCapabilityValue(
        NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE);
    support_dynamic_bitrate_ = encoder_->GetCapabilityValue(
        NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE);

    init_params.encodeWidth = frame_width_;
    init_params.encodeHeight = frame_height_;
    init_params.frameRateNum = max_fps_;
    init_params.frameRateDen = 1;
    // must set max encode width and height otherwise will get crash when try to
    // reconfigure the resolution
    init_params.maxEncodeWidth = frame_width_max_;
    init_params.maxEncodeHeight = frame_height_max_;
    // init_params.darWidth = init_params.encodeWidth;
    // init_params.darHeight = init_params.encodeHeight;

    encodeConfig.gopLength = key_frame_interval_;
    encodeConfig.frameIntervalP = 1;
    encodeConfig.encodeCodecConfig.h264Config.idrPeriod = key_frame_interval_;
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    // encodeConfig.rcParams.constQP.qpIntra = 20;
    // encodeConfig.rcParams.constQP.qpInterP = 23;
    // encodeConfig.rcParams.constQP.qpInterB = 25;
    encodeConfig.rcParams.enableAQ = 1;          // enable AQ
    encodeConfig.rcParams.aqStrength = 8;        // 4~8
    encodeConfig.rcParams.enableTemporalAQ = 1;  // good for static area
    encodeConfig.rcParams.enableMinQP = 1;
    encodeConfig.rcParams.enableMaxQP = 1;
    encodeConfig.rcParams.minQP.qpIntra = 18;
    encodeConfig.rcParams.minQP.qpInterP = 20;
    encodeConfig.rcParams.minQP.qpInterB = 22;
    encodeConfig.rcParams.maxQP.qpIntra = 35;
    encodeConfig.rcParams.maxQP.qpInterP = 37;
    encodeConfig.rcParams.maxQP.qpInterB = 40;

    encodeConfig.rcParams.averageBitRate = average_bitrate_;
    // use the default VBV buffer size
    encodeConfig.rcParams.vbvBufferSize = 0;
    encodeConfig.rcParams.maxBitRate = average_bitrate_;
    // use the default VBV initial delay
    encodeConfig.rcParams.vbvInitialDelay = 0;
    // enable adaptive quantization (Spatial)
    // encodeConfig.rcParams.enableAQ = false;
    encodeConfig.rcParams.enableLookahead = 0;
    encodeConfig.encodeCodecConfig.h264Config.idrPeriod =
        encodeConfig.gopLength;
    encodeConfig.encodeCodecConfig.h264Config.level = NV_ENC_LEVEL_H264_52;
    encodeConfig.encodeCodecConfig.h264Config.disableSPSPPS = 0;
    encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

    encoder_->CreateEncoder(&init_params);
  } catch (const NVENCException& exception) {
    LOG_ERROR("NVENC H.264 encoder init failed, status=[{}]: {}",
              static_cast<int>(exception.getErrorCode()), exception.what());
    ReleaseEncoderResources();
    return -1;
  } catch (const std::exception& exception) {
    LOG_ERROR("NVENC H.264 encoder init failed: {}", exception.what());
    ReleaseEncoderResources();
    return -1;
  } catch (...) {
    LOG_ERROR("NVENC H.264 encoder init failed with an unknown exception");
    ReleaseEncoderResources();
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

int NvidiaVideoEncoder::Encode(
    const RawFrame& raw_frame,
    std::function<int(const EncodedFrame& encoded_frame)> on_encoded_image) {
  if (!encoder_) {
    LOG_ERROR("Invalid encoder");
    return -1;
  }

#ifdef SAVE_RECEIVED_NV12_STREAM
  if (const auto* native_frame = raw_frame.NativeFrame()) {
    const auto& nv12 = native_frame->payload.cpu_nv12;
    for (uint32_t row = 0; row < native_frame->height; ++row) {
      fwrite(nv12.y_plane + static_cast<size_t>(row) * nv12.y_stride, 1,
             native_frame->width, file_nv12_);
    }
    for (uint32_t row = 0; row < native_frame->height / 2U; ++row) {
      fwrite(nv12.uv_plane + static_cast<size_t>(row) * nv12.uv_stride, 1,
             native_frame->width, file_nv12_);
    }
  } else {
    fwrite(raw_frame.Buffer(), 1, raw_frame.Size(), file_nv12_);
  }
#endif

  if (raw_frame.Width() != frame_width_ ||
      raw_frame.Height() != frame_height_) {
    if (support_dynamic_resolution_) {
      if (0 != ResetEncodeResolution(raw_frame.Width(), raw_frame.Height())) {
        return -1;
      }
    }
  }

  if (0 == seq_++ % key_frame_interval_) {
    if (ForceIdr() != 0) {
      return -1;
    }
  }
  const VideoFrameType frame_type = next_frame_is_key_
                                        ? VideoFrameType::kVideoFrameKey
                                        : VideoFrameType::kVideoFrameDelta;
  next_frame_is_key_ = false;

#ifdef SHOW_SUBMODULE_TIME_COST
  auto start = std::chrono::steady_clock::now();
#endif

  const NvEncInputFrame* encoder_inputframe = encoder_->GetNextInputFrame();
  const XNativeVideoFrame* native_frame = raw_frame.NativeFrame();
  if (native_frame && native_frame->type == XNativeVideoFrameCpuNv12) {
    const auto& nv12 = native_frame->payload.cpu_nv12;
    NvEncoderCuda::CopyHostNv12PlanesToDeviceFrame(
        cuda_context_, nv12.y_plane, nv12.y_stride, nv12.uv_plane,
        nv12.uv_stride,
        reinterpret_cast<CUdeviceptr>(encoder_inputframe->inputPtr),
        encoder_inputframe->pitch, encoder_->GetEncodeWidth(),
        encoder_->GetEncodeHeight(), encoder_inputframe->chromaOffsets);
  } else {
    NvEncoderCuda::CopyToDeviceFrame(
        cuda_context_,
        (void*)raw_frame.Buffer(),  // NOLINT
        0, reinterpret_cast<CUdeviceptr>(encoder_inputframe->inputPtr),
        encoder_inputframe->pitch, encoder_->GetEncodeWidth(),
        encoder_->GetEncodeHeight(), CU_MEMORYTYPE_HOST,
        encoder_inputframe->bufferFormat, encoder_inputframe->chromaOffsets,
        encoder_inputframe->numChromaPlanes);
  }

  encoder_->EncodeFrame(encoded_packets_, nullptr, &encoded_packet_qps_);

  if (encoded_packets_.size() < 1) {
    return -1;
  }

  for (size_t packet_index = 0; packet_index < encoded_packets_.size();
       ++packet_index) {
    const auto& packet = encoded_packets_[packet_index];
    if (on_encoded_image) {
      EncodedFrame encoded_frame(packet.data(), packet.size(),
                                 encoder_->GetEncodeWidth(),
                                 encoder_->GetEncodeHeight());

      encoded_frame.SetFrameType(frame_type);
      encoded_frame.SetEncodedWidth(encoder_->GetEncodeWidth());
      encoded_frame.SetEncodedHeight(encoder_->GetEncodeHeight());
      encoded_frame.SetCapturedTimestamp(raw_frame.CapturedTimestamp());
      encoded_frame.SetEncodedTimestamp(clock_->CurrentTime());
      if (packet_index < encoded_packet_qps_.size()) {
        encoded_frame.SetQp(static_cast<int>(encoded_packet_qps_[packet_index]),
                            0, 51, true);
      }
      on_encoded_image(encoded_frame);
#ifdef SAVE_ENCODED_H264_STREAM
      fwrite((unsigned char*)packet.data(), 1, packet.size(), file_h264_);
#endif
    }
  }

#ifdef SHOW_SUBMODULE_TIME_COST
  auto encode_time_cost = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
  LOG_INFO("Encode time cost {}ms", encode_time_cost);
#endif

  return 0;
}

int NvidiaVideoEncoder::ForceIdr() {
  if (!encoder_) {
    return -1;
  }

  NV_ENC_RECONFIGURE_PARAMS reconfig_params;
  memset(&reconfig_params, 0, sizeof(reconfig_params));
  reconfig_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  NV_ENC_INITIALIZE_PARAMS init_params = {NV_ENC_INITIALIZE_PARAMS_VER};
  NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
  init_params.encodeConfig = &encode_config;
  encoder_->GetInitializeParams(&init_params);

  reconfig_params.reInitEncodeParams = init_params;
  reconfig_params.forceIDR = 1;

  if (!encoder_->Reconfigure(&reconfig_params)) {
    LOG_ERROR("Failed to force I frame");
    return -1;
  }

  next_frame_is_key_ = true;

  return 0;
}

int NvidiaVideoEncoder::SetTargetBitrate(int bitrate) {
  if (!encoder_ || bitrate <= 0) {
    return -1;
  }

  const int target_bitrate =
      ClampEncoderTargetBitrate(bitrate, static_cast<int>(max_bitrate_));
  if (target_bitrate != bitrate) {
    LOG_WARN("NVENC target bitrate clamped: requested={} max={}", bitrate,
             max_bitrate_);
  }
  average_bitrate_ = target_bitrate;

  NV_ENC_RECONFIGURE_PARAMS reconfig_params;
  memset(&reconfig_params, 0, sizeof(reconfig_params));
  reconfig_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  NV_ENC_INITIALIZE_PARAMS init_params;
  NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
  init_params.encodeConfig = &encode_config;
  encoder_->GetInitializeParams(&init_params);
  init_params.frameRateDen = 1;
  init_params.frameRateNum = init_params.frameRateDen * max_fps_;
  init_params.encodeConfig->rcParams.averageBitRate = target_bitrate;
  init_params.encodeConfig->rcParams.maxBitRate = target_bitrate;
  reconfig_params.reInitEncodeParams = init_params;
  reconfig_params.forceIDR = 0;
  return encoder_->Reconfigure(&reconfig_params) ? 0 : -1;
}

int NvidiaVideoEncoder::ResetEncodeResolution(unsigned int width,
                                              unsigned int height) {
  if (!encoder_) {
    return -1;
  }

  if (width > frame_width_max_ || height > frame_height_max_) {
    LOG_ERROR(
        "Target resolution is too large for this hardware encoder, which "
        "[{}x{}] and support max resolution is [{}x{}]",
        width, height, frame_width_max_, frame_height_max_);
    return -1;
  }

  frame_width_ = width;
  frame_height_ = height;

  NV_ENC_RECONFIGURE_PARAMS reconfig_params;
  memset(&reconfig_params, 0, sizeof(reconfig_params));
  reconfig_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  NV_ENC_INITIALIZE_PARAMS init_params = {NV_ENC_INITIALIZE_PARAMS_VER};
  NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
  init_params.encodeConfig = &encode_config;
  encoder_->GetInitializeParams(&init_params);

  reconfig_params.reInitEncodeParams = init_params;
  reconfig_params.reInitEncodeParams.encodeWidth = frame_width_;
  reconfig_params.reInitEncodeParams.encodeHeight = frame_height_;
  // reconfig_params.reInitEncodeParams.darWidth =
  //     reconfig_params.reInitEncodeParams.encodeWidth;
  // reconfig_params.reInitEncodeParams.darHeight =
  //     reconfig_params.reInitEncodeParams.encodeHeight;
  reconfig_params.forceIDR = 1;

  if (!encoder_->Reconfigure(&reconfig_params)) {
    LOG_ERROR("Failed to reset resolution");
    return -1;
  }

  // The resolution reconfigure itself requests an IDR. Record that guarantee
  // so the first packet at the new dimensions is labelled as a key frame.
  next_frame_is_key_ = true;
  seq_ = 1;

  return 0;
}
}  // namespace minirtc
