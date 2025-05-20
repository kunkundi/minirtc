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

  VTCompressionSessionRef session_ = nullptr;
  mutex lock_;
  atomic<int> frame_count_{0};

  function<int(const EncodedFrame&)> callback_;

  FILE* file_h264_ = nullptr;
  FILE* file_nv12_ = nullptr;

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

  OSStatus status =
      VTCompressionSessionCreate(NULL, width_, height_, kCMVideoCodecType_H264, NULL, NULL, NULL,
                                 CompressionOutputCallback, this, &session_);
  if (status != noErr || session_ == nullptr) {
    return -1;
  }

  VTSessionSetProperty(session_, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_ProfileLevel,
                       kVTProfileLevel_H264_Main_AutoLevel);

  VTSessionSetProperty(session_, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);

  CFNumberRef frameIntervalRef = CFNumberCreate(NULL, kCFNumberIntType, &keyframe_interval_);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_MaxKeyFrameInterval, frameIntervalRef);
  CFRelease(frameIntervalRef);

  CFNumberRef fpsRef = CFNumberCreate(NULL, kCFNumberIntType, &fps_);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
  CFRelease(fpsRef);

  CFNumberRef bitRateRef = CFNumberCreate(NULL, kCFNumberSInt32Type, &bitrate_);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_AverageBitRate, bitRateRef);
  CFRelease(bitRateRef);

  int dataRateLimit[2] = {bitrate_ / 8, 1};
  CFNumberRef dataRateLimitNum[2] = {CFNumberCreate(NULL, kCFNumberIntType, &dataRateLimit[0]),
                                     CFNumberCreate(NULL, kCFNumberIntType, &dataRateLimit[1])};
  CFArrayRef dataRateLimits =
      CFArrayCreate(NULL, (const void**)dataRateLimitNum, 2, &kCFTypeArrayCallBacks);
  VTSessionSetProperty(session_, kVTCompressionPropertyKey_DataRateLimits, dataRateLimits);
  for (int i = 0; i < 2; ++i) CFRelease(dataRateLimitNum[i]);
  CFRelease(dataRateLimits);

  VTCompressionSessionPrepareToEncodeFrames(session_);

  frame_count_ = 0;

#ifdef SAVE_RECEIVED_NV12_STREAM
  file_nv12_ = fopen("received_nv12_stream.yuv", "w+b");
  if (!file_nv12_) {
    LOG_WARN("Fail to open received_nv12_stream.yuv");
  }
#endif

#ifdef SAVE_ENCODED_H264_STREAM
  file_h264_ = fopen("encoded_h264_stream.h264", "w+b");
  if (!file_h264_) {
    LOG_WARN("Fail to open encoded_h264_stream.h264");
  }
#endif

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

  CVPixelBufferRef pixel_buffer = nullptr;

  NSDictionary* pixelAttributes = @{
    (__bridge NSString*)
    kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    (__bridge NSString*)kCVPixelBufferWidthKey : @(raw_frame.Width()),
    (__bridge NSString*)kCVPixelBufferHeightKey : @(raw_frame.Height()),
  };

  CVReturn cv_status =
      CVPixelBufferCreate(kCFAllocatorDefault, raw_frame.Width(), raw_frame.Height(),
                          kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                          (__bridge CFDictionaryRef)pixelAttributes, &pixel_buffer);

  if (cv_status != kCVReturnSuccess || !pixel_buffer) {
    return -1;
  }

  CVPixelBufferLockBaseAddress(pixel_buffer, 0);

  uint8_t* dst_y = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0);
  uint8_t* dst_uv = (uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1);

  const uint8_t* src = raw_frame.Buffer();
  size_t y_size = raw_frame.Width() * raw_frame.Height();
  size_t uv_size = y_size / 2;

  memcpy(dst_y, src, y_size);
  memcpy(dst_uv, src + y_size, uv_size);

  CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);

  CMTime pts = CMTimeMake(raw_frame.CapturedTimestamp(), 1000000);

  OSStatus status = VTCompressionSessionEncodeFrame(session_, pixel_buffer, pts, kCMTimeInvalid,
                                                    nullptr, nullptr, nullptr);

  CFRelease(pixel_buffer);

  if (status != noErr) {
    return -2;
  }

  frame_count_++;
  return 0;
}

int VideoToolboxEncoder::Impl::ForceIdr() {
  lock_guard<mutex> guard(lock_);
  if (!session_) return -1;

  NSDictionary* properties = @{(__bridge NSString*)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES};
  OSStatus status =
      VTCompressionSessionEncodeFrame(session_, NULL, kCMTimeInvalid, kCMTimeInvalid,
                                      (__bridge CFDictionaryRef)properties, NULL, NULL);
  return (status == noErr) ? 0 : -1;
}

int VideoToolboxEncoder::Impl::SetTargetBitrate(int bitrate) {
  lock_guard<mutex> guard(lock_);
  bitrate_ = bitrate;
  if (!session_) return -1;

  CFNumberRef bitRateRef = CFNumberCreate(NULL, kCFNumberSInt32Type, &bitrate_);
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
  if (status != noErr || !sampleBuffer) return;

  VideoToolboxEncoder::Impl* encoder =
      static_cast<VideoToolboxEncoder::Impl*>(outputCallbackRefCon);
  encoder->HandleEncodedSampleBuffer(sampleBuffer);
}

void VideoToolboxEncoder::Impl::HandleEncodedSampleBuffer(CMSampleBufferRef sampleBuffer) {
  CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);

  CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 0, &sps, &spsSize, &spsCount,
                                                     nullptr);
  CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, 1, &pps, &ppsSize, &ppsCount,
                                                     nullptr);

  if (!CMSampleBufferDataIsReady(sampleBuffer)) {
    return;
  }

  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, true);
  if (!attachments) {
    LOG_ERROR("attachments is null");
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

std::string VideoToolboxEncoder::GetEncoderName() { return "VideoToolbox H264 Encoder"; }

int VideoToolboxEncoder::Release() { return impl_->Release(); }
