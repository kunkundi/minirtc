#include "nvidia_video_encoder.h"

#include <chrono>

#include "log.h"
#include "video_encoder_factory.h"
#ifdef _WIN32
#include "nv_encoder_d3d11.h"
#endif

// #define SAVE_RECEIVED_NV12_STREAM
// #define SAVE_ENCODED_H264_STREAM

namespace minirtc {

NvidiaVideoEncoder::NvidiaVideoEncoder(std::shared_ptr<SystemClock> clock,
                                     VideoCodecType codec)
    : clock_(std::move(clock)),
      codec_guid_(codec == VideoCodecType::H265 ? NV_ENC_CODEC_HEVC_GUID
                                                : NV_ENC_CODEC_H264_GUID) {}
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
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  last_upload_us_.store(-1, std::memory_order_relaxed);
#endif
  if (encoder_) {
    encoder_->DestroyEncoder();
    delete encoder_;
    encoder_ = nullptr;
#ifdef _WIN32
    directx_ = nullptr;
#endif
  }

  if (cuda_context_) {
    cuCtxDestroy_ld(cuda_context_);
    cuda_context_ = nullptr;
  }
}

int NvidiaVideoEncoder::Init(const MediaCodecConfig& config) {
  initial_config_ = config;
#ifdef _WIN32
  if (InitBackend(config, true) == 0) return 0;
#endif
  return InitBackend(config, false);
}

int NvidiaVideoEncoder::InitBackend(const MediaCodecConfig& config,
                                    [[maybe_unused]] bool use_directx) {
  frame_width_ = config.init_width;
  frame_height_ = config.init_height;
  key_frame_interval_ = config.key_frame_interval;
  max_bitrate_ = config.max_bitrate;
  average_bitrate_ = ClampEncoderTargetBitrate(
      config.average_bitrate, static_cast<int>(max_bitrate_));
  max_fps_ = config.max_frame_rate;
  // Always the fastest preset with the ultra-low-latency tuning. The preset is
  // not a latency/quality dial we can afford to move: P7 (the tier that a
  // resolution-preferring degradation setting used to select) is tuned for
  // offline transcoding and measured 19 ms inside EncodeFrame for a single 4K
  // frame on an RTX 3070, against a 16.7 ms budget at 60 fps, which cost a
  // quarter of the captured frames at encode-queue admission. Quality is traded
  // through bitrate and QP instead, which is the knob that matches the real
  // constraint, and screen content gains little from the slower tiers anyway.
  preset_guid_ = NV_ENC_PRESET_P1_GUID;
  tuning_info_ = NV_ENC_TUNING_INFO::NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

  try {
    if (LoadNvEncodeApi() != 0) return -1;
#ifdef _WIN32
    if (use_directx) {
      directx_ = NvEncoderD3D11::Create(frame_width_, frame_height_).release();
      if (!directx_) return -1;
      encoder_ = directx_;
    } else
#endif
    {
      if (!CudaInitializer::Init()) return -1;
      int num_of_gpu = 0;
      if (cuDeviceGetCount_ld(&num_of_gpu) != CUDA_SUCCESS) return -1;
      if (index_of_gpu_ < 0 || index_of_gpu_ >= num_of_gpu) {
        LOG_ERROR("CUDA device {} unavailable ({} devices)", index_of_gpu_, num_of_gpu);
        return -1;
      }

      if (cuDeviceGet_ld(&cuda_device_, index_of_gpu_) != CUDA_SUCCESS) return -1;
      char device_name[80]{};
      cuDeviceGetName_ld(device_name, sizeof(device_name), cuda_device_);
      LOG_INFO("{} using [{}], preset P1, ultra-low latency", GetEncoderName(), device_name);
      if (cuCtxCreate_ld(&cuda_context_, 0, cuda_device_) != CUDA_SUCCESS) return -1;

      encoder_ = new NvEncoderCuda(cuda_context_, frame_width_, frame_height_,
                                   buffer_format_, 0, false, false);
    }

    NV_ENC_INITIALIZE_PARAMS init_params = {NV_ENC_INITIALIZE_PARAMS_VER};
    NV_ENC_CONFIG encodeConfig = {NV_ENC_CONFIG_VER};
    init_params.encodeConfig = &encodeConfig;
    encoder_->CreateDefaultEncoderParams(&init_params, codec_guid_,
                                         preset_guid_, tuning_info_);

    const VideoResolutionLimits limits{
        encoder_->GetCapabilityValue(codec_guid_, NV_ENC_CAPS_WIDTH_MAX),
        encoder_->GetCapabilityValue(codec_guid_, NV_ENC_CAPS_HEIGHT_MAX)};
    const auto [width, height] =
        FitVideoResolution(config.init_width, config.init_height, 0, 0, limits);
    if (width == 0 || height == 0)
      NVENC_THROW_ERROR("Invalid NVENC resolution limits or input size",
                        NV_ENC_ERR_UNSUPPORTED_PARAM);
    resolution_limits_ = limits;
    frame_width_ = width;
    frame_height_ = height;
    // frame_width_min_ = encoder_->GetCapabilityValue(codec_guid_,
    //                                                 NV_ENC_CAPS_WIDTH_MIN);
    // frame_height_min_ = encoder_->GetCapabilityValue(codec_guid_,
    //                                                  NV_ENC_CAPS_HEIGHT_MIN);
    encode_level_max_ = encoder_->GetCapabilityValue(codec_guid_,
                                                     NV_ENC_CAPS_LEVEL_MAX);
    encode_level_min_ = encoder_->GetCapabilityValue(codec_guid_,
                                                     NV_ENC_CAPS_LEVEL_MIN);
    support_dynamic_resolution_ = encoder_->GetCapabilityValue(
        codec_guid_, NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE);
    support_dynamic_bitrate_ = encoder_->GetCapabilityValue(
        codec_guid_, NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE);

    init_params.darWidth = init_params.encodeWidth = frame_width_;
    init_params.darHeight = init_params.encodeHeight = frame_height_;
    init_params.frameRateNum = max_fps_;
    init_params.frameRateDen = 1;
    // must set max encode width and height otherwise will get crash when try to
    // reconfigure the resolution
    init_params.maxEncodeWidth = limits.max_width;
    init_params.maxEncodeHeight = limits.max_height;
#ifdef _WIN32
    if (directx_) {
      // NVENC reserves driver surfaces for this envelope, irrespective of the
      // size of the registered input texture. Grow it only for a larger source.
      init_params.maxEncodeWidth = frame_width_;
      init_params.maxEncodeHeight = frame_height_;
    }
#endif
    encodeConfig.gopLength = key_frame_interval_;
    encodeConfig.frameIntervalP = 1;
    encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    // encodeConfig.rcParams.constQP.qpIntra = 20;
    // encodeConfig.rcParams.constQP.qpInterP = 23;
    // encodeConfig.rcParams.constQP.qpInterB = 25;
    const bool desktop = initial_config_.video_content_type ==
                         MiniRtcVideoContentType::ScreenContent;
    const bool single_pass = initial_config_.UseDesktopRealtimeRateControl();
    if (desktop) {
      encodeConfig.rcParams.multiPass = single_pass ? NV_ENC_MULTI_PASS_DISABLED
                                                   : NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
      LOG_INFO("{}: {} for desktop {}x{} at {} fps", GetEncoderName(),
               single_pass ? "single-pass CBR" : "two-pass CBR (quarter resolution)",
               frame_width_, frame_height_, max_fps_);
    }
    encodeConfig.rcParams.enableAQ = single_pass ? 0 : 1;
    encodeConfig.rcParams.aqStrength = single_pass ? 0 : 8;
    encodeConfig.rcParams.enableTemporalAQ = !single_pass &&
        encoder_->GetCapabilityValue(codec_guid_, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) != 0;
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
    encodeConfig.rcParams.enableLookahead = 0;
    if (codec_guid_ == NV_ENC_CODEC_HEVC_GUID) {
      auto& hevc = encodeConfig.encodeCodecConfig.hevcConfig;
      encodeConfig.profileGUID = NV_ENC_HEVC_PROFILE_MAIN_GUID;
      hevc.chromaFormatIDC = 1;
      hevc.pixelBitDepthMinus8 = 0;
      hevc.idrPeriod = encodeConfig.gopLength;
      hevc.disableSPSPPS = 0;
      hevc.repeatSPSPPS = 1;
      hevc.outputAUD = 1;
    } else {
      auto& h264 = encodeConfig.encodeCodecConfig.h264Config;
      h264.idrPeriod = encodeConfig.gopLength;
      h264.level = NV_ENC_LEVEL_H264_52;
      h264.disableSPSPPS = 0;
      h264.repeatSPSPPS = 1;
    }

    encoder_->CreateEncoder(&init_params);
  } catch (const NVENCException& exception) {
    LOG_ERROR("NVENC encoder init failed, status=[{}]: {}",
              static_cast<int>(exception.getErrorCode()), exception.what());
    ReleaseEncoderResources();
    return -1;
  } catch (const std::exception& exception) {
    LOG_ERROR("NVENC encoder init failed: {}", exception.what());
    ReleaseEncoderResources();
    return -1;
  } catch (...) {
    LOG_ERROR("NVENC encoder init failed with an unknown exception");
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
  // The encode queue owns backend transitions. Only complete, initialized
  // replacements are published to the statistics reader.
  if (const auto* native = raw_frame.NativeFrame();
      native && !SupportsNativeFrameInput(native->type)) {
    RawFrame cpu(*native);
    cpu.SetCapturedTimestamp(raw_frame.CapturedTimestamp());
    if (!cpu.MaterializeNativeFrame()) return -1;
    return Encode(cpu, std::move(on_encoded_image));
  }
  if (encoder_) {
#ifdef _WIN32
    struct ReleaseTexture {
      NvEncoderD3D11*& encoder;
      ~ReleaseTexture() { if (encoder) encoder->Finish(); }
    } release_texture{directx_};
    const bool was_directx = directx_ != nullptr;
#endif
    try {
      return EncodeHardware(raw_frame, on_encoded_image);
    } catch (const NVENCException& error) {
      LOG_WARN("NVENC failed while encoding {}x{}: {}", raw_frame.Width(),
               raw_frame.Height(), error.what());
#ifdef _WIN32
      const auto config = CurrentConfig(raw_frame.Width(), raw_frame.Height());
      ReleaseEncoderResources();
      seq_ = 0;
      next_frame_is_key_ = true;
      if (was_directx && InitBackend(config, false) == 0)
        return Encode(raw_frame, std::move(on_encoded_image));
#else
      return -1;
#endif
    }
  }
#ifdef _WIN32
  if (!fallback_) {
    if (clock_->CurrentTimeMs() < retry_backend_at_ms_) return -1;
    const auto config = CurrentConfig(raw_frame.Width(), raw_frame.Height());
    const auto codec = codec_guid_ == NV_ENC_CODEC_HEVC_GUID ? VideoCodecType::H265
                                                             : VideoCodecType::H264;
    std::shared_ptr<MediaCodec> replacement =
        VideoEncoderFactory::CreateWindowsFallbackEncoder(clock_, config, codec);
    if (!replacement) {
      // A device reset can temporarily take down both NVENC and Media Foundation.
      const bool retry_hardware = retry_backend_at_ms_ != 0;
      retry_backend_at_ms_ = clock_->CurrentTimeMs() + 1000;
      if (retry_hardware && Init(config) == 0)
        return Encode(raw_frame, std::move(on_encoded_image));
      return -1;
    }
    LOG_WARN("NVENC unavailable; continuing with {}",
             replacement->GetEncoderName());
    std::atomic_store(&fallback_, std::move(replacement));
    const auto size = fallback_->PrepareEncodeResolution(raw_frame.Width(), raw_frame.Height());
    if (size.first != int(raw_frame.Width()) || size.second != int(raw_frame.Height()))
      return -1;  // The next input is scaled for the replacement backend.
    // Re-enter to materialize the failed hardware input for the new encoder.
    return Encode(raw_frame, std::move(on_encoded_image));
  }
  return fallback_->Encode(raw_frame, std::move(on_encoded_image));
#else
  return -1;
#endif
}

int NvidiaVideoEncoder::EncodeHardware(
    const RawFrame& raw_frame,
    const std::function<int(const EncodedFrame&)>& on_encoded_image) {

#ifdef SAVE_RECEIVED_NV12_STREAM
  if (const auto* native = raw_frame.NativeFrame()) {
    std::vector<uint8_t> packed(raw_frame.Size());
    if (native->copy_to_nv12(native->owner, packed.data(), packed.size()) == 0)
      fwrite(packed.data(), 1, packed.size(), file_nv12_);
  } else {
    fwrite(raw_frame.Buffer(), 1, raw_frame.Size(), file_nv12_);
  }
#endif

  if (raw_frame.Width() != frame_width_ ||
      raw_frame.Height() != frame_height_) {
    if (ResetEncodeResolution(raw_frame.Width(), raw_frame.Height()) != 0) return -1;
  }

  const bool key_frame = next_frame_is_key_ || seq_ % key_frame_interval_ == 0;
  const VideoFrameType frame_type =
      key_frame ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta;

#ifdef SHOW_SUBMODULE_TIME_COST
  auto start = std::chrono::steady_clock::now();
#endif

  const MiniRtcNativeVideoFrame* native_frame = raw_frame.NativeFrame();
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  using StageClock = std::chrono::steady_clock;
  const bool sample_timings = get_logger()->should_log(spdlog::level::debug);
  const auto upload_start = sample_timings ? StageClock::now() : StageClock::time_point{};
#endif
#ifdef _WIN32
  if (directx_) {
    // An unreadable capture surface is a dropped input, not a failed encoder.
    if (!directx_->Prepare(raw_frame)) return -1;
  } else
#endif
  {
    const NvEncInputFrame* encoder_inputframe = encoder_->GetNextInputFrame();
    if (native_frame && native_frame->type == MiniRtcNativeVideoFrameCpuNv12) {
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
  }
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  const auto submit_start = sample_timings ? StageClock::now() : StageClock::time_point{};
#endif
  NV_ENC_PIC_PARAMS picture{};
  if (key_frame)
    picture.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
  encoder_->EncodeFrame(encoded_packets_, &picture, &encoded_packet_qps_);
#ifdef _WIN32
  if (directx_) directx_->Finish();
#endif
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  if (sample_timings) {
    const auto submit_end = StageClock::now();
    last_upload_us_.store(std::chrono::duration_cast<std::chrono::microseconds>(
                              submit_start - upload_start).count(), std::memory_order_relaxed);
    last_submit_us_.store(std::chrono::duration_cast<std::chrono::microseconds>(
                              submit_end - submit_start).count(), std::memory_order_relaxed);
  } else {
    last_upload_us_.store(-1, std::memory_order_relaxed);
  }
#endif

  if (encoded_packets_.size() < 1) {
    return -1;
  }

  next_frame_is_key_ = false;
  ++seq_;
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
  LOG_DEBUG("Encode time cost {}ms", encode_time_cost);
#endif

  return 0;
}

int NvidiaVideoEncoder::ForceIdr() {
#ifdef _WIN32
  if (fallback_) return fallback_->ForceIdr();
#endif
  next_frame_is_key_ = true;

  return 0;
}

int NvidiaVideoEncoder::SetTargetBitrate(int bitrate) {
#ifdef _WIN32
  if (fallback_) return fallback_->SetTargetBitrate(bitrate);
#endif
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
  try {
    return encoder_->Reconfigure(&reconfig_params) ? 0 : -1;
  } catch (const NVENCException& error) {
    LOG_WARN("NVENC bitrate reconfiguration failed: {}", error.what());
    return -1;
  }
}

#ifdef _WIN32
bool NvidiaVideoEncoder::SupportsInputFrameRateUpdates() const {
  return fallback_ && fallback_->SupportsInputFrameRateUpdates();
}
int NvidiaVideoEncoder::SetInputFrameRate(int fps) {
  return fallback_ ? fallback_->SetInputFrameRate(fps) : -1;
}
int NvidiaVideoEncoder::SetTargetFrameRate(int fps) {
  return fallback_ ? fallback_->SetTargetFrameRate(fps) : -1;
}
#endif

MediaCodecConfig NvidiaVideoEncoder::CurrentConfig(unsigned width, unsigned height) const {
  auto config = initial_config_;
  config.init_width = width;
  config.init_height = height;
  config.init_bitrate = config.average_bitrate = average_bitrate_;
  return config;
}

int NvidiaVideoEncoder::ResetEncodeResolution(unsigned int width,
                                              unsigned int height) {
  if (!encoder_) {
    return -1;
  }

  if (width > static_cast<unsigned>(resolution_limits_.max_width) ||
      height > static_cast<unsigned>(resolution_limits_.max_height)) {
    LOG_ERROR(
        "Target resolution is too large for this hardware encoder, which "
        "[{}x{}] and support max resolution is [{}x{}]",
        width, height, resolution_limits_.max_width, resolution_limits_.max_height);
    NVENC_THROW_ERROR("Requested resolution exceeds NVENC capabilities",
                      NV_ENC_ERR_UNSUPPORTED_PARAM);
  }

#ifdef _WIN32
  if (directx_ && (!support_dynamic_resolution_ || !directx_->Fits(width, height))) {
    auto config = CurrentConfig(width, height);
    // All DirectX submissions are synchronous. Retire the old session first
    // so a resolution switch also works at the driver's concurrent-session cap.
    ReleaseEncoderResources();
    if (InitBackend(config, true) != 0)
      NVENC_THROW_ERROR("Could not reopen Direct3D encoder at the requested size",
                        NV_ENC_ERR_ENCODER_NOT_INITIALIZED);
    seq_ = 0;
    next_frame_is_key_ = true;
    return 0;
  }
#endif

  if (!support_dynamic_resolution_)
    NVENC_THROW_ERROR("NVENC does not support dynamic resolution", NV_ENC_ERR_UNSUPPORTED_PARAM);
  NV_ENC_RECONFIGURE_PARAMS reconfig_params;
  memset(&reconfig_params, 0, sizeof(reconfig_params));
  reconfig_params.version = NV_ENC_RECONFIGURE_PARAMS_VER;
  NV_ENC_INITIALIZE_PARAMS init_params = {NV_ENC_INITIALIZE_PARAMS_VER};
  NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
  init_params.encodeConfig = &encode_config;
  encoder_->GetInitializeParams(&init_params);

  reconfig_params.reInitEncodeParams = init_params;
  reconfig_params.reInitEncodeParams.encodeWidth = width;
  reconfig_params.reInitEncodeParams.encodeHeight = height;
  reconfig_params.reInitEncodeParams.darWidth = width;
  reconfig_params.reInitEncodeParams.darHeight = height;
  reconfig_params.forceIDR = 1;

  if (!encoder_->Reconfigure(&reconfig_params)) {
    LOG_ERROR("Failed to reset resolution");
    return -1;
  }

  frame_width_ = width;
  frame_height_ = height;
  // The resolution reconfigure itself requests an IDR. Record that guarantee
  // so the first packet at the new dimensions is labelled as a key frame.
  next_frame_is_key_ = true;
  seq_ = 1;

  return 0;
}
}  // namespace minirtc
