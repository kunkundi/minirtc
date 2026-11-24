#include "video_toolbox_encoder.h"
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <TargetConditionals.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "h264_bitstream_parser.h"
#include "log.h"

// #define SAVE_RECEIVED_NV12_STREAM
// #define SAVE_ENCODED_H264_STREAM

namespace minirtc {

using namespace std;

class VideoToolboxEncoder::Impl {
 public:
  Impl(std::shared_ptr<SystemClock> clock, VideoCodecType codec);
  ~Impl();

  int Init(const MediaCodecConfig& config);
  int Encode(const RawFrame& raw_frame, function<int(const EncodedFrame&)> on_encoded_image);
  int ForceIdr();
  int SetTargetBitrate(int bitrate);
  int SetTargetFrameRate(int frame_rate);
  bool SupportsDynamicEncodingSpeedPriority() const {
    return speed_priority_supported_.load();
  }
  int SetPrioritizeEncodingSpeedOverQuality(bool prioritize_speed);
  int GetResolution(int* width, int* height);
  std::string GetEncoderName() const {
    return hevc_ ? "VideoToolbox (H.265)" : "VideoToolbox (H.264)";
  }

 private:
  struct FrameCallbackContext {
    function<int(const EncodedFrame&)> callback;
    uint64_t generation = 0;
  };

  int CreateCompressionSession(int width, int height, bool try_realtime_options,
                               VTCompressionSessionRef* session_out);
  int TryCompressionSession(int width, int height, CFDictionaryRef encoder_spec,
                            bool low_latency, bool try_spatial_aq,
                            VTCompressionSessionRef* session_out);
  int ResetEncodeResolution(int width, int height);
  int ApplyBitrateProperties(VTCompressionSessionRef session, int bitrate, bool low_latency);
  int ApplyEncodingSpeedPriority(VTCompressionSessionRef session,
                                 bool prioritize_speed);
  void ResetCodecState();

  static void CompressionOutputCallback(void* outputCallbackRefCon, void* sourceFrameRefCon,
                                        OSStatus status, VTEncodeInfoFlags infoFlags,
                                        CMSampleBufferRef sampleBuffer);
  void* RegisterFrameCallback(
      function<int(const EncodedFrame&)> callback);
  bool TakeFrameCallback(void* source_frame_ref_con,
                         FrameCallbackContext* context);
  void CancelFrameCallback(void* context);
  void CancelPendingFrameCallbacks();
  void FinishActiveCallback();
  void WaitForActiveCallbacks();
  void RecordSessionFailure(OSStatus status, uint64_t generation);

 private:
  std::shared_ptr<SystemClock> clock_;
  const bool hevc_;
  int width_ = 2880;
  int height_ = 1800;
  int max_fps_ = 60;
  int average_bitrate_ = MINIRTC_AVERAGE_BITRATE;
  int max_bitrate_ = kDefaultMaxEncoderBitrateBps;
  int keyframe_interval_ = 30;
  int seq_ = 0;
  bool prioritize_encoding_speed_ = false;
  bool low_latency_active_ = false;
#if TARGET_OS_OSX && __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  bool desktop_realtime_rate_control_ = false;
#endif
  std::atomic<bool> speed_priority_supported_{false};
  std::atomic<bool> force_idr_ = false;
  std::atomic<bool> native_input_logged_ = false;

  VTCompressionSessionRef session_ = nullptr;
  mutex lock_;
  atomic<uint64_t> session_generation_{0};
  // Readers in transport/adaptation must never wait for a VideoToolbox call.
  // Publish both dimensions together so a resize cannot expose a mixed pair.
  atomic<uint64_t> resolution_snapshot_{0};
  atomic<uint64_t> failed_generation_{0};
  int64_t next_recovery_attempt_ms_ = 0;

  mutex callback_lock_;
  condition_variable callback_cv_;
  unordered_map<uintptr_t, FrameCallbackContext> pending_frame_callbacks_;
  uintptr_t next_frame_callback_token_ = 1;
  size_t active_callbacks_ = 0;
  bool shutting_down_ = false;

  mutex h264_parser_lock_;
  H264BitstreamParser h264_bitstream_parser_;
  uint64_t h264_parser_generation_ = 0;

  FILE* file_h264_ = nullptr;
  FILE* file_nv12_ = nullptr;

  std::string h264_file_name_;
  std::string nv12_file_name_;

  void HandleEncodedSampleBuffer(CMSampleBufferRef sampleBuffer,
                                 FrameCallbackContext context);
};

#pragma mark - Impl 实现

VideoToolboxEncoder::Impl::Impl(std::shared_ptr<SystemClock> clock, VideoCodecType codec)
    : clock_(std::move(clock)), hevc_(codec == VideoCodecType::H265) {}

VideoToolboxEncoder::Impl::~Impl() {
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
  {
    lock_guard<mutex> callback_guard(callback_lock_);
    shutting_down_ = true;
    session_generation_.fetch_add(1);
  }

  {
    lock_guard<mutex> guard(lock_);
    if (session_) {
      VTCompressionSessionInvalidate(session_);
      CFRelease(session_);
      session_ = nullptr;
    }
  }

  CancelPendingFrameCallbacks();
  WaitForActiveCallbacks();
}

int VideoToolboxEncoder::Impl::Init(const MediaCodecConfig& config) {
  lock_guard<mutex> guard(lock_);
#if TARGET_OS_OSX && __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  desktop_realtime_rate_control_ = config.UseDesktopRealtimeRateControl();
#endif
  max_fps_ = config.max_frame_rate;
  max_bitrate_ = config.max_bitrate;
  prioritize_encoding_speed_ =
      config.video_degradation_preference ==
      MiniRtcVideoDegradationPreference::MaintainFrameRate;
  average_bitrate_ =
      ClampEncoderTargetBitrate(config.average_bitrate, max_bitrate_);
  keyframe_interval_ = config.key_frame_interval;

  if (ResetEncodeResolution(config.init_width, config.init_height) != 0) {
    return -1;
  }

#ifdef SAVE_RECEIVED_NV12_STREAM
  nv12_file_name_ =
      "received_nv12_stream_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yuv";
  file_nv12_ = fopen(nv12_file_name_.c_str(), "w+b");
  if (!file_nv12_) {
    LOG_WARN("Fail to open {}", nv12_file_name_.c_str());
  }
#endif

#ifdef SAVE_ENCODED_H264_STREAM
  if (!hevc_) {
    h264_file_name_ =
        "encoded_h264_stream_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".h264";
    file_h264_ = fopen(h264_file_name_.c_str(), "w+b");
    if (!file_h264_) {
      LOG_WARN("Fail to open {}", h264_file_name_.c_str());
    }
  }
#endif

  return 0;
}

int VideoToolboxEncoder::Impl::CreateCompressionSession(
    int width, int height, bool try_realtime_options, VTCompressionSessionRef* session_out) {
  if (!session_out || width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0) {
    LOG_ERROR("Invalid VideoToolbox encoder resolution [{}x{}]", width, height);
    return -1;
  }

  *session_out = nullptr;

  CFMutableDictionaryRef encoder_spec = CFDictionaryCreateMutable(
      nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

  if (!encoder_spec) {
    LOG_ERROR("Failed to create VideoToolbox encoder specification");
    return -1;
  }

#if TARGET_OS_IOS
  if (@available(iOS 17.4, *)) {
    CFDictionarySetValue(
        encoder_spec,
        kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
        kCFBooleanTrue);
  }
#else
  CFDictionarySetValue(
      encoder_spec, hevc_ ? kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder
                          : kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
      kCFBooleanTrue);
#endif

#if TARGET_OS_OSX && __MAC_OS_X_VERSION_MAX_ALLOWED >= 110300
  if (@available(macOS 11.3, *)) {
    if (try_realtime_options) {
      CFDictionarySetValue(encoder_spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl,
                           kCFBooleanTrue);
      if (TryCompressionSession(width, height, encoder_spec, true, true, session_out) == 0) {
        CFRelease(encoder_spec);
        return 0;
      }
      CFDictionaryRemoveValue(encoder_spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl);
      LOG_WARN("Retrying {} with standard rate control at {}x{}", GetEncoderName(), width, height);
    }
  }
#endif
  const int result = TryCompressionSession(width, height, encoder_spec, false,
                                           try_realtime_options, session_out);
  CFRelease(encoder_spec);
  return result;
}

int VideoToolboxEncoder::Impl::TryCompressionSession(
    int width, int height, CFDictionaryRef encoder_spec, bool low_latency,
    [[maybe_unused]] bool try_spatial_aq, VTCompressionSessionRef* session_out) {
  VTCompressionSessionRef new_session = nullptr;
  const CMVideoCodecType codec = hevc_ ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;
  OSStatus status = VTCompressionSessionCreate(nullptr, width, height, codec, encoder_spec,
                                               nullptr, nullptr, CompressionOutputCallback,
                                               this, &new_session);
  if (status != noErr || new_session == nullptr) {
    LOG_ERROR("Failed to create VideoToolbox encoder session [{}x{}]: {}", width, height, status);
    return -1;
  }

  const bool speed_priority_supported =
      ApplyEncodingSpeedPriority(new_session, prioritize_encoding_speed_) == 0;
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_MoreFramesBeforeStart,
                       kCFBooleanFalse);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_AllowFrameReordering,
                       kCFBooleanFalse);
  CFStringRef profile = low_latency ? kVTProfileLevel_H264_High_AutoLevel
                                    : kVTProfileLevel_H264_High_5_2;
  if (hevc_) {
    profile = kVTProfileLevel_HEVC_Main_AutoLevel;
    VTSessionSetProperty(new_session, kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
  }
  status = VTSessionSetProperty(new_session, kVTCompressionPropertyKey_ProfileLevel, profile);
  if ((low_latency || hevc_) && status != noErr) {
    LOG_WARN("{} rejected the requested profile: {}", GetEncoderName(), status);
    VTCompressionSessionInvalidate(new_session);
    CFRelease(new_session);
    return -1;
  }

  CFNumberRef frameIntervalRef = CFNumberCreate(nullptr, kCFNumberIntType, &keyframe_interval_);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
                       frameIntervalRef);
  CFRelease(frameIntervalRef);

  CFNumberRef fpsRef = CFNumberCreate(nullptr, kCFNumberIntType, &max_fps_);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
  CFRelease(fpsRef);

  if (ApplyBitrateProperties(new_session, average_bitrate_, low_latency) != 0) {
    VTCompressionSessionInvalidate(new_session);
    CFRelease(new_session);
    return -1;
  }

  int maxFrameDelayCount = 1;
  CFNumberRef maxFrameDelayCountRef =
      CFNumberCreate(nullptr, kCFNumberIntType, &maxFrameDelayCount);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_MaxFrameDelayCount,
                       maxFrameDelayCountRef);
  CFRelease(maxFrameDelayCountRef);

  bool applied_aq = false;
#if TARGET_OS_OSX && __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
  if (@available(macOS 15.0, *)) {
    if (try_spatial_aq && desktop_realtime_rate_control_) {
      const int level = kVTQPModulationLevel_Disable;
      CFNumberRef value = CFNumberCreate(nullptr, kCFNumberIntType, &level);
      if (value) {
        applied_aq = VTSessionSetProperty(
            new_session, kVTCompressionPropertyKey_SpatialAdaptiveQPLevel, value) == noErr;
        CFRelease(value);
      }
    }
  }
#endif
  status = VTCompressionSessionPrepareToEncodeFrames(new_session);
  if (status != noErr) {
    LOG_ERROR("Failed to prepare VideoToolbox encoder session [{}x{}]: {}", width, height, status);
    VTCompressionSessionInvalidate(new_session);
    CFRelease(new_session);
    if (applied_aq) {
      return TryCompressionSession(width, height, encoder_spec, low_latency, false, session_out);
    }
    return -1;
  }

  low_latency_active_ = low_latency;
  speed_priority_supported_.store(speed_priority_supported);
  *session_out = new_session;
  LOG_INFO("{} session {}x{}: low_latency={}", GetEncoderName(), width, height, low_latency);
  return 0;
}

int VideoToolboxEncoder::Impl::ResetEncodeResolution(int width, int height) {
  const int previous_width = width_;
  const int previous_height = height_;
  const bool retry_failed_session =
      session_ && failed_generation_.load() == session_generation_.load() &&
      width == previous_width && height == previous_height;
  // Revoke pending output before invalidation so it cannot be delivered as
  // frames from the replacement session.
  session_generation_.fetch_add(1);
  speed_priority_supported_.store(false);
  CancelPendingFrameCallbacks();
  if (session_) {
    VTCompressionSessionInvalidate(session_);
    CFRelease(session_);
    session_ = nullptr;
  }

  VTCompressionSessionRef new_session = nullptr;
  if (CreateCompressionSession(width, height, !retry_failed_session, &new_session) != 0) {
    next_recovery_attempt_ms_ = clock_->CurrentTimeMs() + 1000;
    return -1;
  }

  session_ = new_session;
  width_ = width;
  height_ = height;
  ResetCodecState();
  resolution_snapshot_.store((static_cast<uint64_t>(width) << 32) |
                             static_cast<uint32_t>(height));
  next_recovery_attempt_ms_ = 0;

  LOG_INFO("VideoToolbox encoder resolution changed: {}x{} -> {}x{}", previous_width,
           previous_height, width_, height_);
  return 0;
}

int VideoToolboxEncoder::Impl::ApplyBitrateProperties(VTCompressionSessionRef session,
                                                      int bitrate, bool low_latency) {
  if (!session || bitrate <= 0) {
    LOG_ERROR("Invalid VideoToolbox bitrate [{}]", bitrate);
    return -1;
  }

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 130000
  if (!low_latency) {
    if (@available(macOS 13.0, *)) {
      CFNumberRef bit_rate_ref =
          CFNumberCreate(nullptr, kCFNumberSInt32Type, &bitrate);
      OSStatus cbr_status = VTSessionSetProperty(
          session, kVTCompressionPropertyKey_ConstantBitRate, bit_rate_ref);
      CFRelease(bit_rate_ref);
      if (cbr_status == noErr) {
        return 0;
      }
      LOG_WARN(
          "VideoToolbox CBR is unavailable; falling back to a constrained "
          "bitrate: status={}",
          cbr_status);
    }
  }
#endif

  CFNumberRef bit_rate_ref = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bitrate);
  OSStatus average_status =
      VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, bit_rate_ref);
  CFRelease(bit_rate_ref);

  int data_rate_limit[2] = {std::max(1, bitrate / 8), 1};
  CFNumberRef data_rate_limit_numbers[2] = {
      CFNumberCreate(nullptr, kCFNumberIntType, &data_rate_limit[0]),
      CFNumberCreate(nullptr, kCFNumberIntType, &data_rate_limit[1])};
  const void* data_rate_limit_values[2] = {data_rate_limit_numbers[0], data_rate_limit_numbers[1]};
  CFArrayRef data_rate_limits =
      CFArrayCreate(nullptr, data_rate_limit_values, 2, &kCFTypeArrayCallBacks);
  OSStatus limit_status =
      VTSessionSetProperty(session, kVTCompressionPropertyKey_DataRateLimits, data_rate_limits);
  for (CFNumberRef number : data_rate_limit_numbers) {
    CFRelease(number);
  }
  CFRelease(data_rate_limits);

  if (average_status != noErr || limit_status != noErr) {
    LOG_ERROR("Failed to set VideoToolbox bitrate [{}]: average={}, limit={}", bitrate,
              average_status, limit_status);
    return -1;
  }

  return 0;
}

int VideoToolboxEncoder::Impl::ApplyEncodingSpeedPriority(
    VTCompressionSessionRef session, bool prioritize_speed) {
  if (!session) {
    return -1;
  }

  OSStatus status = VTSessionSetProperty(
      session, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
      prioritize_speed ? kCFBooleanTrue : kCFBooleanFalse);
  if (status == kVTPropertyNotSupportedErr) return -1;
  if (status != noErr) {
    LOG_WARN(
        "Failed to set VideoToolbox encoding speed priority: enabled={} status={}",
        prioritize_speed, status);
    return -1;
  }
  return 0;
}

void VideoToolboxEncoder::Impl::ResetCodecState() {
  // A newly created compression session must emit a key frame before any
  // delta frame at the new resolution.
  seq_ = 0;
  force_idr_ = true;
  if (!hevc_) {
    lock_guard<mutex> parser_guard(h264_parser_lock_);
    h264_bitstream_parser_.Reset();
    h264_parser_generation_ = session_generation_.load();
  }
}

static CVPixelBufferRef CreateNV12PixelBufferFromData(const char* data, size_t width,
                                                      size_t height) {
  CVPixelBufferRef pixelBuffer = nullptr;

  NSDictionary* pixelAttributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};

  CVReturn status =
      CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                          (__bridge CFDictionaryRef)pixelAttributes, &pixelBuffer);
  if (status != kCVReturnSuccess || !pixelBuffer) {
    return nullptr;
  }

  CVPixelBufferLockBaseAddress(pixelBuffer, 0);

  uint8_t* dstY = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
  size_t strideY = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
  const uint8_t* srcY = (const uint8_t*)data;
  for (size_t row = 0; row < height; ++row) {
    memcpy(dstY + row * strideY, srcY + row * width, width);
  }

  uint8_t* dstUV = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
  size_t strideUV = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
  const uint8_t* srcUV = (const uint8_t*)(data + width * height);
  for (size_t row = 0; row < height / 2; ++row) {
    memcpy(dstUV + row * strideUV, srcUV + row * width, width);
  }

  CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
  return pixelBuffer;
}

int VideoToolboxEncoder::Impl::Encode(const RawFrame& raw_frame,
                                      function<int(const EncodedFrame&)> on_encoded_image) {
  lock_guard<mutex> guard(lock_);

  const int raw_width = static_cast<int>(raw_frame.Width());
  const int raw_height = static_cast<int>(raw_frame.Height());
  const uint64_t expected_size =
      static_cast<uint64_t>(raw_frame.Width()) * raw_frame.Height() * 3 / 2;
  const MiniRtcNativeVideoFrame* native_frame = raw_frame.NativeFrame();
  const bool has_native_pixel_buffer =
      native_frame &&
      native_frame->type == MiniRtcNativeVideoFrameCVPixelBuffer &&
      native_frame->payload.cv_pixel_buffer;
  if (raw_width <= 0 || raw_height <= 0 || raw_width % 2 != 0 ||
      raw_height % 2 != 0 ||
      (!has_native_pixel_buffer &&
       (!raw_frame.Buffer() || raw_frame.Size() < expected_size))) {
    LOG_ERROR("Invalid NV12 frame for VideoToolbox: {}x{}, size={}",
              raw_width, raw_height, raw_frame.Size());
    return -1;
  }

  CVPixelBufferRef pixel_buffer = nullptr;
  if (has_native_pixel_buffer) {
    pixel_buffer = static_cast<CVPixelBufferRef>(
        native_frame->payload.cv_pixel_buffer);
    const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
    if (CVPixelBufferGetWidth(pixel_buffer) != raw_frame.Width() ||
        CVPixelBufferGetHeight(pixel_buffer) != raw_frame.Height() ||
        !CVPixelBufferIsPlanar(pixel_buffer) ||
        CVPixelBufferGetPlaneCount(pixel_buffer) < 2 ||
        (pixel_format !=
             kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
         pixel_format !=
             kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
      LOG_ERROR(
          "Invalid native CVPixelBuffer for VideoToolbox: {}x{}, format={}",
          CVPixelBufferGetWidth(pixel_buffer),
          CVPixelBufferGetHeight(pixel_buffer), pixel_format);
      return -1;
    }
    CVPixelBufferRetain(pixel_buffer);
  } else {
    pixel_buffer = CreateNV12PixelBufferFromData(
        reinterpret_cast<const char*>(raw_frame.Buffer()), raw_frame.Width(),
        raw_frame.Height());
    if (!pixel_buffer) {
      LOG_ERROR("Failed to create VideoToolbox pixel buffer [{}x{}]", raw_width,
                raw_height);
      return -1;
    }
  }

  const bool recovering =
      !session_ || failed_generation_.load() == session_generation_.load();
  if (recovering && clock_->CurrentTimeMs() < next_recovery_attempt_ms_) {
    CFRelease(pixel_buffer);
    return -1;
  }
  if (recovering || raw_width != width_ || raw_height != height_) {
    if (ResetEncodeResolution(raw_width, raw_height) != 0) {
      CFRelease(pixel_buffer);
      LOG_ERROR("Failed to reset VideoToolbox encoder resolution to {}x{}",
                raw_width, raw_height);
      return -1;
    }
    if (recovering) {
      next_recovery_attempt_ms_ = clock_->CurrentTimeMs() + 1000;
      LOG_INFO("VideoToolbox encoder session recovered at {}x{}", raw_width,
               raw_height);
    }
  }

#ifdef SAVE_RECEIVED_NV12_STREAM
  if (raw_frame.Buffer()) {
    fwrite(raw_frame.Buffer(), 1, raw_frame.Size(), file_nv12_);
  } else if (native_frame) {
    vector<uint8_t> packed_nv12(expected_size);
    if (native_frame->copy_to_nv12(native_frame->owner, packed_nv12.data(),
                                   packed_nv12.size()) == 0) {
      fwrite(packed_nv12.data(), 1, packed_nv12.size(), file_nv12_);
    }
  }
#endif

  CMTime pts = CMTimeMake(raw_frame.CapturedTimestamp(), 1000000);
  const int keyframe_interval = std::max(1, keyframe_interval_);
  const bool periodic_keyframe = seq_++ % keyframe_interval == 0;
  const bool force_idr_requested = force_idr_.exchange(false);
  const bool force_keyframe = periodic_keyframe || force_idr_requested;
  const auto restore_force_idr_request = [this, force_idr_requested] {
    if (force_idr_requested) {
      force_idr_.store(true);
    }
  };
  CFDictionaryRef frame_options = nullptr;
  if (force_keyframe) {
    const void* keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
    const void* values[] = {kCFBooleanTrue};
    frame_options = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks);
    if (!frame_options) {
      CFRelease(pixel_buffer);
      restore_force_idr_request();
      LOG_ERROR("Failed to create VideoToolbox keyframe options");
      return -1;
    }
  }

  void* frame_callback =
      RegisterFrameCallback(std::move(on_encoded_image));
  if (!frame_callback) {
    CFRelease(pixel_buffer);
    if (frame_options) {
      CFRelease(frame_options);
    }
    restore_force_idr_request();
    return -1;
  }

  OSStatus status = VTCompressionSessionEncodeFrame(
      session_, pixel_buffer, pts, kCMTimeInvalid, frame_options,
      frame_callback, nullptr);
  CFRelease(pixel_buffer);
  if (frame_options) {
    CFRelease(frame_options);
  }
  if (status != noErr) {
    CancelFrameCallback(frame_callback);
    restore_force_idr_request();
    RecordSessionFailure(status, session_generation_.load());
    LOG_ERROR("VTCompressionSessionEncodeFrame failed: {}", status);
    return -2;
  }

  if (has_native_pixel_buffer && !native_input_logged_.exchange(true)) {
    LOG_INFO(
        "macOS ScreenCaptureKit-to-VideoToolbox native NV12 path enabled");
  }

  return 0;
}

void* VideoToolboxEncoder::Impl::RegisterFrameCallback(
    function<int(const EncodedFrame&)> callback) {
  if (!callback) {
    return nullptr;
  }

  FrameCallbackContext context{std::move(callback),
                               session_generation_.load()};

  lock_guard<mutex> guard(callback_lock_);
  if (shutting_down_) {
    return nullptr;
  }

  uintptr_t token = 0;
  do {
    token = next_frame_callback_token_++;
  } while (token == 0 ||
           pending_frame_callbacks_.find(token) !=
               pending_frame_callbacks_.end());
  pending_frame_callbacks_.emplace(token, std::move(context));
  return reinterpret_cast<void*>(token);
}

bool VideoToolboxEncoder::Impl::TakeFrameCallback(
    void* source_frame_ref_con, FrameCallbackContext* context) {
  if (!source_frame_ref_con || !context) {
    return false;
  }

  lock_guard<mutex> guard(callback_lock_);
  const uintptr_t token = reinterpret_cast<uintptr_t>(source_frame_ref_con);
  auto it = pending_frame_callbacks_.find(token);
  if (it == pending_frame_callbacks_.end()) {
    return false;
  }

  *context = std::move(it->second);
  pending_frame_callbacks_.erase(it);
  if (shutting_down_) {
    return false;
  }
  ++active_callbacks_;
  return true;
}

void VideoToolboxEncoder::Impl::CancelFrameCallback(void* context) {
  if (!context) {
    return;
  }
  lock_guard<mutex> guard(callback_lock_);
  const uintptr_t token = reinterpret_cast<uintptr_t>(context);
  pending_frame_callbacks_.erase(token);
}

void VideoToolboxEncoder::Impl::CancelPendingFrameCallbacks() {
  lock_guard<mutex> guard(callback_lock_);
  pending_frame_callbacks_.clear();
}

void VideoToolboxEncoder::Impl::FinishActiveCallback() {
  lock_guard<mutex> guard(callback_lock_);
  if (active_callbacks_ > 0) {
    --active_callbacks_;
  }
  if (active_callbacks_ == 0) {
    callback_cv_.notify_all();
  }
}

void VideoToolboxEncoder::Impl::WaitForActiveCallbacks() {
  unique_lock<mutex> lock(callback_lock_);
  callback_cv_.wait(lock, [this] { return active_callbacks_ == 0; });
}

void VideoToolboxEncoder::Impl::RecordSessionFailure(OSStatus status,
                                                    uint64_t generation) {
  if (generation != session_generation_.load()) return;
  auto failed = failed_generation_.load();
  while (failed < generation) {
    if (failed_generation_.compare_exchange_weak(failed, generation)) {
      LOG_ERROR("VideoToolbox encoder session failed: status={}, generation={}; "
                "recreating on the encode queue", status, generation);
      return;
    }
  }
}

int VideoToolboxEncoder::Impl::ForceIdr() {
  force_idr_ = true;
  return 0;
}

int VideoToolboxEncoder::Impl::SetTargetBitrate(int bitrate) {
  lock_guard<mutex> guard(lock_);
  if (bitrate <= 0) return -1;

  average_bitrate_ = ClampEncoderTargetBitrate(bitrate, max_bitrate_);
  if (average_bitrate_ != bitrate) {
    LOG_WARN("VideoToolbox target bitrate clamped: requested={} max={}",
             bitrate, max_bitrate_);
  }
  if (!session_) return -1;
  if (ApplyBitrateProperties(session_, average_bitrate_, low_latency_active_) != 0) {
    return -1;
  }
  return 0;
}

int VideoToolboxEncoder::Impl::SetTargetFrameRate(int frame_rate) {
  lock_guard<mutex> guard(lock_);
  if (!session_ || frame_rate < 1 || frame_rate > 60) return -1;
  if (frame_rate == max_fps_) return 0;
  CFNumberRef fps = CFNumberCreate(nullptr, kCFNumberIntType, &frame_rate);
  const OSStatus status = VTSessionSetProperty(
      session_, kVTCompressionPropertyKey_ExpectedFrameRate, fps);
  CFRelease(fps);
  if (status != noErr) return -1;
  max_fps_ = frame_rate;
  return 0;
}

int VideoToolboxEncoder::Impl::SetPrioritizeEncodingSpeedOverQuality(
    bool prioritize_speed) {
  lock_guard<mutex> guard(lock_);
  if (!session_ || !SupportsDynamicEncodingSpeedPriority()) {
    return -1;
  }
  if (prioritize_encoding_speed_ == prioritize_speed) {
    return 0;
  }
  if (ApplyEncodingSpeedPriority(session_, prioritize_speed) != 0) {
    return -1;
  }

  prioritize_encoding_speed_ = prioritize_speed;
  LOG_INFO("VideoToolbox encoding speed priority [{}]",
           prioritize_speed ? "ON" : "OFF");
  return 0;
}

int VideoToolboxEncoder::Impl::GetResolution(int* width, int* height) {
  const uint64_t resolution = resolution_snapshot_.load();
  if (width) *width = static_cast<int>(resolution >> 32);
  if (height) *height = static_cast<int>(resolution & 0xffffffffU);
  return resolution != 0 ? 0 : -1;
}

void VideoToolboxEncoder::Impl::CompressionOutputCallback(void* outputCallbackRefCon,
                                                          void* sourceFrameRefCon, OSStatus status,
                                                          VTEncodeInfoFlags infoFlags,
                                                          CMSampleBufferRef sampleBuffer) {
  VideoToolboxEncoder::Impl* encoder =
      static_cast<VideoToolboxEncoder::Impl*>(outputCallbackRefCon);
  if (!encoder) {
    return;
  }

  FrameCallbackContext context;
  if (!encoder->TakeFrameCallback(sourceFrameRefCon, &context)) {
    return;
  }

  struct ActiveCallbackGuard {
    VideoToolboxEncoder::Impl* encoder;
    ~ActiveCallbackGuard() { encoder->FinishActiveCallback(); }
  } callback_guard{encoder};

  if (status != noErr) {
    // Never rebuild or acquire lock_ from an output callback: EncodeFrame and
    // session invalidation can be waiting for this callback to return.
    encoder->RecordSessionFailure(status, context.generation);
    return;
  }
  if (!sampleBuffer ||
      !CMSampleBufferDataIsReady(sampleBuffer)) {
    return;
  }
  encoder->HandleEncodedSampleBuffer(sampleBuffer, std::move(context));
}

void VideoToolboxEncoder::Impl::HandleEncodedSampleBuffer(
    CMSampleBufferRef sampleBuffer, FrameCallbackContext context) {
  CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
  if (!formatDesc) {
    LOG_ERROR("VideoToolbox sample has no format description");
    return;
  }

  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, true);
  if (!attachments || CFArrayGetCount(attachments) == 0) {
    LOG_ERROR("VideoToolbox sample has no attachments");
    return;
  }

  CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
  const bool isKeyframe =
      CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync) != kCFBooleanTrue;
  auto get_parameter_set = hevc_ ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex
                                 : CMVideoFormatDescriptionGetH264ParameterSetAtIndex;
  int length_bytes = 0;
  size_t set_count = 0;
  OSStatus status = get_parameter_set(formatDesc, 0, nullptr, nullptr, &set_count, &length_bytes);
  if (status != noErr || (length_bytes != 1 && length_bytes != 2 && length_bytes != 4)) {
    LOG_ERROR("Invalid VideoToolbox NAL length field: status={}, bytes={}", status, length_bytes);
    return;
  }

  CMBlockBufferRef dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
  const size_t totalLength = dataBuffer ? CMBlockBufferGetDataLength(dataBuffer) : 0;
  if (!totalLength) {
    LOG_ERROR("VideoToolbox sample has no encoded data");
    return;
  }
  std::vector<uint8_t> annexBData;
  constexpr uint8_t startCode[] = {0, 0, 0, 1};

  if (isKeyframe) {
    for (size_t i = 0; i < set_count; ++i) {
      const uint8_t* data = nullptr;
      size_t size = 0;
      status = get_parameter_set(formatDesc, i, &data, &size, nullptr, nullptr);
      if (status != noErr || !data || !size) {
        LOG_ERROR("Failed to get VideoToolbox parameter set {}: {}", i, status);
        return;
      }
      annexBData.insert(annexBData.end(), std::begin(startCode), std::end(startCode));
      annexBData.insert(annexBData.end(), data, data + size);
    }
  }

  annexBData.reserve(annexBData.size() + totalLength);
  const size_t header_size = static_cast<size_t>(length_bytes);
  for (size_t offset = 0; offset < totalLength;) {
    // A CMBlockBuffer may span several memory blocks, even within one NAL.
    uint8_t header[4];
    if (header_size > totalLength - offset ||
        CMBlockBufferCopyDataBytes(dataBuffer, offset, header_size, header) != noErr) {
      LOG_ERROR("Failed to read VideoToolbox NAL length");
      return;
    }
    size_t nal_size = 0;
    for (size_t i = 0; i < header_size; ++i) nal_size = (nal_size << 8) | header[i];
    offset += header_size;
    if (!nal_size || nal_size > totalLength - offset) {
      LOG_ERROR("Invalid VideoToolbox NAL length: {}", nal_size);
      return;
    }
    annexBData.insert(annexBData.end(), std::begin(startCode), std::end(startCode));
    const size_t payload_offset = annexBData.size();
    annexBData.resize(payload_offset + nal_size);
    if (CMBlockBufferCopyDataBytes(dataBuffer, offset, nal_size,
                                   annexBData.data() + payload_offset) != noErr) {
      LOG_ERROR("Failed to read VideoToolbox NAL payload");
      return;
    }
    offset += nal_size;
  }

#ifdef SAVE_ENCODED_H264_STREAM
  if (file_h264_) fwrite(annexBData.data(), 1, annexBData.size(), file_h264_);
#endif

  EncodedFrame frame(annexBData.data(), annexBData.size());
  frame.SetFrameType(isKeyframe ? VideoFrameType::kVideoFrameKey
                                : VideoFrameType::kVideoFrameDelta);
  const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(formatDesc);
  frame.SetEncodedWidth(dimensions.width);
  frame.SetEncodedHeight(dimensions.height);
  frame.SetCapturedTimestamp(CMSampleBufferGetOutputPresentationTimeStamp(sampleBuffer).value);
  frame.SetEncodedTimestamp(clock_->CurrentTime());

  if (!hevc_) {
    lock_guard<mutex> parser_guard(h264_parser_lock_);
    const uint64_t current_generation = session_generation_.load();
    if (context.generation == current_generation &&
        context.generation == h264_parser_generation_) {
      h264_bitstream_parser_.ParseBitstream(annexBData.data(),
                                            annexBData.size());
      const std::optional<int> qp = h264_bitstream_parser_.GetLastSliceQp();
      if (qp) {
        // WebRTC's H264BitstreamParser reports the last slice's base QP, not
        // a macroblock-weighted frame average.
        frame.SetQp(*qp, 0, 51, false);
      }
    }
  }

  if (context.callback &&
      context.generation == session_generation_.load()) {
    context.callback(frame);
  }
}

//

VideoToolboxEncoder::VideoToolboxEncoder(std::shared_ptr<SystemClock> clock, VideoCodecType codec)
    : impl_(std::make_shared<Impl>(std::move(clock), codec)) {}

#if TARGET_OS_OSX
bool VideoToolboxEncoder::IsHardwareHevcSupported() {
  @autoreleasepool {
    NSDictionary* spec = @{
      (id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES
    };
    CFStringRef encoder_id = nullptr;
    OSStatus status = VTCopySupportedPropertyDictionaryForEncoder(
        MINIRTC_INIT_WIDTH, MINIRTC_INIT_HEIGHT, kCMVideoCodecType_HEVC,
        (__bridge CFDictionaryRef)spec, &encoder_id, nullptr);
    const bool supported = status == noErr && encoder_id != nullptr;
    if (encoder_id) CFRelease(encoder_id);
    return supported;
  }
}
#endif

VideoToolboxEncoder::~VideoToolboxEncoder() = default;

int VideoToolboxEncoder::Init(const MediaCodecConfig& config) { return impl_->Init(config); }

int VideoToolboxEncoder::Encode(const RawFrame& raw_frame,
                                function<int(const EncodedFrame&)> on_encoded_image) {
  return impl_->Encode(raw_frame, on_encoded_image);
}

int VideoToolboxEncoder::ForceIdr() { return impl_->ForceIdr(); }

int VideoToolboxEncoder::SetTargetBitrate(int bitrate) { return impl_->SetTargetBitrate(bitrate); }

int VideoToolboxEncoder::SetTargetFrameRate(int frame_rate) {
  return impl_->SetTargetFrameRate(frame_rate);
}

bool VideoToolboxEncoder::SupportsDynamicEncodingSpeedPriority() const {
  return impl_->SupportsDynamicEncodingSpeedPriority();
}

int VideoToolboxEncoder::SetPrioritizeEncodingSpeedOverQuality(
    bool prioritize_speed) {
  return impl_->SetPrioritizeEncodingSpeedOverQuality(prioritize_speed);
}

int VideoToolboxEncoder::GetResolution(int* width, int* height) const {
  return impl_->GetResolution(width, height);
}

std::string VideoToolboxEncoder::GetEncoderName() const { return impl_->GetEncoderName(); }
}  // namespace minirtc
