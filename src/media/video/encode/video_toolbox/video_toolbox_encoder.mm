#include "video_toolbox_encoder.h"
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <atomic>
#include <mutex>
#include "log.h"

// #define SAVE_RECEIVED_NV12_STREAM
// #define SAVE_ENCODED_H264_STREAM

using namespace std;

class VideoToolboxEncoder::Impl {
 public:
  Impl(std::shared_ptr<SystemClock> clock);
  ~Impl();

  int Init(int width, int height, int fps, int bitrate, int keyframe_interval);
  int Encode(const RawFrame& raw_frame, function<int(const EncodedFrame&)> on_encoded_image);
  int ForceIdr();
  int SetTargetBitrate(int bitrate);
  int GetResolution(int* width, int* height);
  int Release();

 private:
  static void CompressionOutputCallback(void* outputCallbackRefCon, void* sourceFrameRefCon,
                                        OSStatus status, VTEncodeInfoFlags infoFlags,
                                        CMSampleBufferRef sampleBuffer);

  std::shared_ptr<SystemClock> clock_;
  int width_ = 2880;
  int height_ = 1800;
  int fps_ = 30;
  int bitrate_ = 5000000;
  int keyframe_interval_ = 30;
  int seq_ = 0;
  int ref_buffer_count_ = 3;
  std::atomic<bool> force_idr_ = false;
  bool sps_pps_got_ = false;

  VTCompressionSessionRef session_ = nullptr;
  mutex lock_;
  atomic<int> frame_count_{0};

  function<int(const EncodedFrame&)> callback_;

  FILE* file_h264_ = nullptr;
  FILE* file_nv12_ = nullptr;

  std::string h264_file_name_;
  std::string nv12_file_name_;

  const uint8_t* sps = nullptr;
  const uint8_t* pps = nullptr;
  size_t spsSize = 0, ppsSize = 0;
  size_t spsCount, ppsCount;

  // 编码回调处理
  void HandleEncodedSampleBuffer(CMSampleBufferRef sampleBuffer);
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
  Release();
}

int VideoToolboxEncoder::Impl::Init(int width, int height, int fps, int bitrate,
                                    int keyframe_interval) {
  lock_guard<mutex> guard(lock_);
  width_ = width;
  height_ = height;
  fps_ = fps;
  bitrate_ = bitrate;
  keyframe_interval_ = keyframe_interval;

  if (session_) {
    VTCompressionSessionInvalidate(session_);
    CFRelease(session_);
    session_ = nullptr;
  }

  OSStatus status = VTCompressionSessionCreate(
      kCFAllocatorDefault, width_, height_, kCMVideoCodecType_H264, NULL, NULL, kCFAllocatorDefault,
      CompressionOutputCallback, this, &session_);
  if (status != noErr || session_ == nullptr) {
    return -1;
  }

  // kVTCompressionPropertyKey_MinAllowedFrameQP/kVTCompressionPropertyKey_MaxAllowedFrameQP

  VTSessionSetProperty(session_, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality,
                       kCFBooleanTrue);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_MoreFramesBeforeStart, kCFBooleanFalse);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_ProfileLevel,
                       kVTProfileLevel_H264_Baseline_5_2);

  CFNumberRef frameIntervalRef =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyframe_interval_);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_MaxKeyFrameInterval, frameIntervalRef);
  CFRelease(frameIntervalRef);

  CFNumberRef fpsRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &fps_);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
  CFRelease(fpsRef);

  CFNumberRef bitRateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bitrate_);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_AverageBitRate, bitRateRef);
  CFRelease(bitRateRef);

  // CFNumberRef refBufferCount =
  //     CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &ref_buffer_count_);
  // VTSessionSetProperty(session_, kVTCompressionPropertyKey_ReferenceBufferCount, refBufferCount);
  // CFRelease(refBufferCount);

  int maxFrameDelayCount = 1;
  CFNumberRef maxFrameDelayCountRef =
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maxFrameDelayCount);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_MaxFrameDelayCount,
                       maxFrameDelayCountRef);
  CFRelease(maxFrameDelayCountRef);

  int dataRateLimit[2] = {bitrate_ / 8, 1};
  CFNumberRef dataRateLimitNum[2] = {
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &dataRateLimit[0]),
      CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &dataRateLimit[1])};
  CFArrayRef dataRateLimits =
      CFArrayCreate(kCFAllocatorDefault, (const void**)dataRateLimitNum, 2, &kCFTypeArrayCallBacks);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_DataRateLimits, dataRateLimits);
  for (int i = 0; i < 2; ++i) {
    CFRelease(dataRateLimitNum[i]);
  }
  CFRelease(dataRateLimits);

  VTCompressionSessionPrepareToEncodeFrames(session_);

  frame_count_ = 0;

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

static CVPixelBufferRef CreateNV12PixelBufferFromData(const char* data, size_t width,
                                                      size_t height) {
  CVPixelBufferRef pixelBuffer = nullptr;

  NSDictionary* pixelAttributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};

  CVReturn status = CVPixelBufferCreate(kCFAllocatorDefault, width, height,
                                        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
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

static int CreateEncoderDictH264(bool i_frame, CFDictionaryRef* dict_out) {
  CFDictionaryRef dict = NULL;
  if (i_frame) {
    const void* keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
    const void* vals[] = {kCFBooleanTrue};

    dict = CFDictionaryCreate(NULL, keys, vals, 1, NULL, NULL);
    if (!dict) return -1;
  }

  *dict_out = dict;
  return 0;
}

int VideoToolboxEncoder::Impl::Encode(const RawFrame& raw_frame,
                                      function<int(const EncodedFrame&)> on_encoded_image) {
  lock_guard<mutex> guard(lock_);
  if (!session_) return -1;

  callback_ = on_encoded_image;

#ifdef SAVE_RECEIVED_NV12_STREAM
  fwrite(raw_frame.Buffer(), 1, raw_frame.Size(), file_nv12_);
#endif

  CVPixelBufferRef pixel_buffer = CreateNV12PixelBufferFromData(
      (const char*)raw_frame.Buffer(), raw_frame.Width(), raw_frame.Height());

  CMTime pts = CMTimeMake(raw_frame.CapturedTimestamp(), 1000000);
  CFDictionaryRef frame_dict = NULL;

  if (0 == seq_++ % keyframe_interval_ || force_idr_) {
    CreateEncoderDictH264(true, &frame_dict);
    NSDictionary* properties = @{(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES};
    OSStatus status =
        VTCompressionSessionEncodeFrame(session_, pixel_buffer, pts, kCMTimeInvalid,
                                        (__bridge CFDictionaryRef)properties, nullptr, nullptr);
    if (status != noErr) {
      LOG_ERROR("VTCompressionSessionEncodeFrame failed: {}", status);
      return -2;
    }
    force_idr_ = false;
  } else {
    CreateEncoderDictH264(false, &frame_dict);
    OSStatus status = VTCompressionSessionEncodeFrame(session_, pixel_buffer, pts, kCMTimeInvalid,
                                                      nullptr, nullptr, nullptr);
    if (status != noErr) {
      LOG_ERROR("VTCompressionSessionEncodeFrame failed: {}", status);
      return -2;
    }
  }

  CFRelease(pixel_buffer);
  if (frame_dict) {
    CFRelease(frame_dict);
  }

  return 0;
}

int VideoToolboxEncoder::Impl::ForceIdr() {
  force_idr_ = true;
  return 0;
}

int VideoToolboxEncoder::Impl::SetTargetBitrate(int bitrate) {
  lock_guard<mutex> guard(lock_);
  bitrate_ = bitrate;
  if (!session_) return -1;

  CFNumberRef bitRateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &bitrate_);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_AverageBitRate, bitRateRef);
  CFRelease(bitRateRef);
  return 0;
}

int VideoToolboxEncoder::Impl::GetResolution(int* width, int* height) {
  if (width) *width = width_;
  if (height) *height = height_;
  return 0;
}

int VideoToolboxEncoder::Impl::Release() {
  lock_guard<mutex> guard(lock_);
  if (session_) {
    VTCompressionSessionCompleteFrames(session_, kCMTimeInvalid);
    VTCompressionSessionInvalidate(session_);
    CFRelease(session_);
    session_ = nullptr;
  }
  return 0;
}

void VideoToolboxEncoder::Impl::CompressionOutputCallback(void* outputCallbackRefCon,
                                                          void* sourceFrameRefCon, OSStatus status,
                                                          VTEncodeInfoFlags infoFlags,
                                                          CMSampleBufferRef sampleBuffer) {
  if (status != noErr || !sampleBuffer || !CMSampleBufferDataIsReady(sampleBuffer)) return;

  VideoToolboxEncoder::Impl* encoder =
      static_cast<VideoToolboxEncoder::Impl*>(outputCallbackRefCon);
  encoder->HandleEncodedSampleBuffer(sampleBuffer);
}

void VideoToolboxEncoder::Impl::HandleEncodedSampleBuffer(CMSampleBufferRef sampleBuffer) {
  if (!sps_pps_got_) {
    CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);

    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 0, &sps, &spsSize, &spsCount,
                                                       nullptr);
    CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 1, &pps, &ppsSize, &ppsCount,
                                                       nullptr);
    sps_pps_got_ = true;
  }

  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, true);
  if (!attachments) {
    LOG_ERROR("attachments is kCFAllocatorDefault");
    return;
  }

  CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
  bool isKeyframe = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);

  CMBlockBufferRef dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
  size_t totalLength = 0;
  char* dataPointer = nullptr;
  CMBlockBufferGetDataPointer(dataBuffer, 0, nullptr, &totalLength, &dataPointer);

  std::vector<uint8_t> annexBData;

  const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};
  size_t offset = 0;

  if (isKeyframe) {
    annexBData.insert(annexBData.begin(), pps, pps + ppsSize);
    annexBData.insert(annexBData.begin(), startCode, startCode + 4);
    annexBData.insert(annexBData.begin(), sps, sps + spsSize);
    annexBData.insert(annexBData.begin(), startCode, startCode + 4);
  }

  while (offset + 4 <= totalLength) {
    uint32_t nalLength = 0;
    memcpy(&nalLength, dataPointer + offset, 4);
    nalLength = CFSwapInt32BigToHost(nalLength);
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
  frame.SetEncodedWidth(width_);
  frame.SetEncodedHeight(height_);
  frame.SetCapturedTimestamp(CMSampleBufferGetOutputPresentationTimeStamp(sampleBuffer).value);
  frame.SetEncodedTimestamp(clock_->CurrentTime());

  if (callback_) {
    callback_(frame);
  }
}

//

VideoToolboxEncoder::VideoToolboxEncoder(std::shared_ptr<SystemClock> clock)
    : impl_(new Impl(clock)) {}

VideoToolboxEncoder::~VideoToolboxEncoder() = default;

int VideoToolboxEncoder::Init() {
  return impl_->Init(frame_width_, frame_height_, fps_, target_bitrate_, key_frame_interval_);
}

int VideoToolboxEncoder::Encode(const RawFrame& raw_frame,
                                function<int(const EncodedFrame&)> on_encoded_image) {
  return impl_->Encode(raw_frame, on_encoded_image);
}

int VideoToolboxEncoder::ForceIdr() { return impl_->ForceIdr(); }

int VideoToolboxEncoder::SetTargetBitrate(int bitrate) { return impl_->SetTargetBitrate(bitrate); }

int VideoToolboxEncoder::GetResolution(int* width, int* height) {
  return impl_->GetResolution(width, height);
}

std::string VideoToolboxEncoder::GetEncoderName() { return "VideoToolboxH264"; }

int VideoToolboxEncoder::Release() { return impl_->Release(); }
