#include "video_toolbox_encoder.h"
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <TargetConditionals.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "log.h"

// #define SAVE_RECEIVED_NV12_STREAM
// #define SAVE_ENCODED_H264_STREAM

namespace minirtc {

using namespace std;

class VideoToolboxEncoder::Impl {
 public:
  Impl(std::shared_ptr<SystemClock> clock);
  ~Impl();

  int Init(const MediaCodecConfig& config);
  int Encode(const RawFrame& raw_frame, function<int(const EncodedFrame&)> on_encoded_image);
  int ForceIdr();
  int SetTargetBitrate(int bitrate);
  int SetPrioritizeEncodingSpeedOverQuality(bool prioritize_speed);
  int GetResolution(int* width, int* height);
  std::string GetEncoderName() { return "VideoToolboxH264"; }

 private:
  struct FrameCallbackContext {
    function<int(const EncodedFrame&)> callback;
    uint64_t generation = 0;
  };

  int CreateCompressionSession(int width, int height, VTCompressionSessionRef* session_out);
  int ResetEncodeResolution(int width, int height);
  int ApplyBitrateProperties(VTCompressionSessionRef session, int bitrate);
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

 private:
  std::shared_ptr<SystemClock> clock_;
  int width_ = 2880;
  int height_ = 1800;
  int max_fps_ = 60;
  int average_bitrate_ = MINIRTC_AVERAGE_BITRATE;
  int max_bitrate_ = kDefaultMaxEncoderBitrateBps;
  int keyframe_interval_ = 30;
  int seq_ = 0;
  bool prioritize_encoding_speed_ = false;
  std::atomic<bool> force_idr_ = false;

  VTCompressionSessionRef session_ = nullptr;
  mutex lock_;
  atomic<uint64_t> session_generation_{0};

  mutex callback_lock_;
  condition_variable callback_cv_;
  unordered_map<uintptr_t, FrameCallbackContext> pending_frame_callbacks_;
  uintptr_t next_frame_callback_token_ = 1;
  size_t active_callbacks_ = 0;
  bool shutting_down_ = false;

  FILE* file_h264_ = nullptr;
  FILE* file_nv12_ = nullptr;

  std::string h264_file_name_;
  std::string nv12_file_name_;

  void HandleEncodedSampleBuffer(CMSampleBufferRef sampleBuffer,
                                 FrameCallbackContext context);
};

#pragma mark - Impl 实现

VideoToolboxEncoder::Impl::Impl(std::shared_ptr<SystemClock> clock) : clock_(clock) {}

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
  max_fps_ = config.max_frame_rate;
  max_bitrate_ = config.max_bitrate;
  prioritize_encoding_speed_ =
      config.video_degradation_preference ==
      VideoDegradationPreference::MaintainFrameRate;
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
  h264_file_name_ =
      "encoded_h264_stream_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".h264";
  file_h264_ = fopen(h264_file_name_.c_str(), "w+b");
  if (!file_h264_) {
    LOG_WARN("Fail to open {}", h264_file_name_.c_str());
  }
#endif

  return 0;
}

int VideoToolboxEncoder::Impl::CreateCompressionSession(int width, int height,
                                                        VTCompressionSessionRef* session_out) {
  if (!session_out || width <= 0 || height <= 0 || width % 2 != 0 || height % 2 != 0) {
    LOG_ERROR("Invalid VideoToolbox encoder resolution [{}x{}]", width, height);
    return -1;
  }

  *session_out = nullptr;

  CFMutableDictionaryRef encoder_spec = CFDictionaryCreateMutable(
      nullptr, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

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
      encoder_spec,
      kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder,
      kCFBooleanTrue);
#endif

  VTCompressionSessionRef new_session = nullptr;
  OSStatus status =
      VTCompressionSessionCreate(nullptr, width, height, kCMVideoCodecType_H264, encoder_spec,
                                 nullptr, nullptr, CompressionOutputCallback, this, &new_session);
  CFRelease(encoder_spec);
  if (status != noErr || new_session == nullptr) {
    LOG_ERROR("Failed to create VideoToolbox encoder session [{}x{}]: {}", width, height, status);
    return -1;
  }

  // kVTCompressionPropertyKey_MinAllowedFrameQP/kVTCompressionPropertyKey_MaxAllowedFrameQP

  ApplyEncodingSpeedPriority(new_session, prioritize_encoding_speed_);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_MoreFramesBeforeStart,
                       kCFBooleanFalse);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_AllowFrameReordering,
                       kCFBooleanFalse);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_ProfileLevel,
                       kVTProfileLevel_H264_High_5_2);

  CFNumberRef frameIntervalRef = CFNumberCreate(nullptr, kCFNumberIntType, &keyframe_interval_);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
                       frameIntervalRef);
  CFRelease(frameIntervalRef);

  CFNumberRef fpsRef = CFNumberCreate(nullptr, kCFNumberIntType, &max_fps_);
  VTSessionSetProperty(new_session, kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
  CFRelease(fpsRef);

  if (ApplyBitrateProperties(new_session, average_bitrate_) != 0) {
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

  status = VTCompressionSessionPrepareToEncodeFrames(new_session);
  if (status != noErr) {
    LOG_ERROR("Failed to prepare VideoToolbox encoder session [{}x{}]: {}", width, height, status);
    VTCompressionSessionInvalidate(new_session);
    CFRelease(new_session);
    return -1;
  }

  *session_out = new_session;
  return 0;
}

int VideoToolboxEncoder::Impl::ResetEncodeResolution(int width, int height) {
  VTCompressionSessionRef new_session = nullptr;
  if (CreateCompressionSession(width, height, &new_session) != 0) {
    return -1;
  }

  const int previous_width = width_;
  const int previous_height = height_;
  if (session_) {
    // Pending frames belong to the old resolution. Drop them so an old
    // callback cannot race with output from the newly selected session.
    session_generation_.fetch_add(1);
    CancelPendingFrameCallbacks();
    VTCompressionSessionInvalidate(session_);
    CFRelease(session_);
  }

  session_ = new_session;
  width_ = width;
  height_ = height;
  ResetCodecState();

  LOG_INFO("VideoToolbox encoder resolution changed: {}x{} -> {}x{}", previous_width,
           previous_height, width_, height_);
  return 0;
}

int VideoToolboxEncoder::Impl::ApplyBitrateProperties(VTCompressionSessionRef session,
                                                      int bitrate) {
  if (!session || bitrate <= 0) {
    LOG_ERROR("Invalid VideoToolbox bitrate [{}]", bitrate);
    return -1;
  }

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 130000
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
  if (!session_) return -1;

  const int raw_width = static_cast<int>(raw_frame.Width());
  const int raw_height = static_cast<int>(raw_frame.Height());
  const uint64_t expected_size =
      static_cast<uint64_t>(raw_frame.Width()) * raw_frame.Height() * 3 / 2;
  if (!raw_frame.Buffer() || raw_width <= 0 || raw_height <= 0 || raw_width % 2 != 0 ||
      raw_height % 2 != 0 || raw_frame.Size() < expected_size) {
    LOG_ERROR("Invalid NV12 frame for VideoToolbox: {}x{}, size={}", raw_width, raw_height,
              raw_frame.Size());
    return -1;
  }

  if (raw_width != width_ || raw_height != height_) {
    if (ResetEncodeResolution(raw_width, raw_height) != 0) {
      LOG_ERROR("Failed to reset VideoToolbox encoder resolution to {}x{}", raw_width, raw_height);
      return -1;
    }
  }

#ifdef SAVE_RECEIVED_NV12_STREAM
  fwrite(raw_frame.Buffer(), 1, raw_frame.Size(), file_nv12_);
#endif

  CVPixelBufferRef pixel_buffer = CreateNV12PixelBufferFromData(
      (const char*)raw_frame.Buffer(), raw_frame.Width(), raw_frame.Height());
  if (!pixel_buffer) {
    LOG_ERROR("Failed to create VideoToolbox pixel buffer [{}x{}]", raw_width, raw_height);
    return -1;
  }

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

  OSStatus status = VTCompressionSessionEncodeFrame(session_, pixel_buffer, pts, kCMTimeInvalid,
                                                    frame_options, frame_callback, nullptr);
  CFRelease(pixel_buffer);
  if (frame_options) {
    CFRelease(frame_options);
  }
  if (status != noErr) {
    CancelFrameCallback(frame_callback);
    restore_force_idr_request();
    LOG_ERROR("VTCompressionSessionEncodeFrame failed: {}", status);
    return -2;
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
  if (ApplyBitrateProperties(session_, average_bitrate_) != 0) {
    return -1;
  }
  return 0;
}

int VideoToolboxEncoder::Impl::SetPrioritizeEncodingSpeedOverQuality(
    bool prioritize_speed) {
  lock_guard<mutex> guard(lock_);
  if (!session_) {
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
  lock_guard<mutex> guard(lock_);
  if (width) *width = width_;
  if (height) *height = height_;
  return 0;
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

  if (status != noErr || !sampleBuffer ||
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
  bool isKeyframe = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);

  std::vector<uint8_t> sps;
  std::vector<uint8_t> pps;
  if (isKeyframe) {
    const uint8_t* sps_data = nullptr;
    const uint8_t* pps_data = nullptr;
    size_t sps_size = 0;
    size_t pps_size = 0;
    size_t sps_count = 0;
    size_t pps_count = 0;
    OSStatus sps_status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
        formatDesc, 0, &sps_data, &sps_size, &sps_count, nullptr);
    OSStatus pps_status = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
        formatDesc, 1, &pps_data, &pps_size, &pps_count, nullptr);
    if (sps_status != noErr || pps_status != noErr || !sps_data || !pps_data || sps_size == 0 ||
        pps_size == 0) {
      LOG_ERROR("Failed to get VideoToolbox SPS/PPS: sps={}, pps={}", sps_status, pps_status);
      return;
    }
    sps.assign(sps_data, sps_data + sps_size);
    pps.assign(pps_data, pps_data + pps_size);
  }

  CMBlockBufferRef dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
  if (!dataBuffer) {
    LOG_ERROR("VideoToolbox sample has no data buffer");
    return;
  }
  size_t totalLength = 0;
  char* dataPointer = nullptr;
  OSStatus block_status =
      CMBlockBufferGetDataPointer(dataBuffer, 0, nullptr, &totalLength, &dataPointer);
  if (block_status != noErr || !dataPointer) {
    LOG_ERROR("Failed to read VideoToolbox data buffer: {}", block_status);
    return;
  }

  std::vector<uint8_t> annexBData;

  const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};
  size_t offset = 0;

  if (isKeyframe) {
    annexBData.insert(annexBData.end(), startCode, startCode + 4);
    annexBData.insert(annexBData.end(), sps.begin(), sps.end());
    annexBData.insert(annexBData.end(), startCode, startCode + 4);
    annexBData.insert(annexBData.end(), pps.begin(), pps.end());
  }

  while (offset + 4 <= totalLength) {
    uint32_t nalLength = 0;
    memcpy(&nalLength, dataPointer + offset, 4);
    nalLength = CFSwapInt32BigToHost(nalLength);
    if (nalLength > totalLength - offset - 4) {
      LOG_ERROR("Invalid VideoToolbox NAL length [{}], remaining={}", nalLength,
                totalLength - offset - 4);
      return;
    }
    annexBData.insert(annexBData.end(), startCode, startCode + 4);
    annexBData.insert(annexBData.end(), dataPointer + offset + 4,
                      dataPointer + offset + 4 + nalLength);
    offset += 4 + nalLength;
  }

#ifdef SAVE_ENCODED_H264_STREAM
  fwrite(annexBData.data(), 1, annexBData.size(), file_h264_);
#endif

  EncodedFrame frame(annexBData.data(), annexBData.size());
  frame.SetFrameType(isKeyframe ? VideoFrameType::kVideoFrameKey
                                : VideoFrameType::kVideoFrameDelta);
  const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(formatDesc);
  frame.SetEncodedWidth(dimensions.width);
  frame.SetEncodedHeight(dimensions.height);
  frame.SetCapturedTimestamp(CMSampleBufferGetOutputPresentationTimeStamp(sampleBuffer).value);
  frame.SetEncodedTimestamp(clock_->CurrentTime());

  if (context.callback &&
      context.generation == session_generation_.load()) {
    context.callback(frame);
  }
}

//

VideoToolboxEncoder::VideoToolboxEncoder(std::shared_ptr<SystemClock> clock)
    : impl_(new Impl(clock)) {}

VideoToolboxEncoder::~VideoToolboxEncoder() = default;

int VideoToolboxEncoder::Init(const MediaCodecConfig& config) { return impl_->Init(config); }

int VideoToolboxEncoder::Encode(const RawFrame& raw_frame,
                                function<int(const EncodedFrame&)> on_encoded_image) {
  return impl_->Encode(raw_frame, on_encoded_image);
}

int VideoToolboxEncoder::ForceIdr() { return impl_->ForceIdr(); }

int VideoToolboxEncoder::SetTargetBitrate(int bitrate) { return impl_->SetTargetBitrate(bitrate); }

int VideoToolboxEncoder::SetPrioritizeEncodingSpeedOverQuality(
    bool prioritize_speed) {
  return impl_->SetPrioritizeEncodingSpeedOverQuality(prioritize_speed);
}

int VideoToolboxEncoder::GetResolution(int* width, int* height) const {
  return impl_->GetResolution(width, height);
}

std::string VideoToolboxEncoder::GetEncoderName() const { return impl_->GetEncoderName(); }
}  // namespace minirtc
