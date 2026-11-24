#include "video_toolbox_decoder.h"
#include <CoreMedia/CoreMedia.h>
#include <TargetConditionals.h>
#include <VideoToolbox/VideoToolbox.h>
#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "minirtc.h"
#include "native_video_frame.h"
#include "sdr_color.h"

// #define SAVE_DECODED_NV12_STREAM
// #define SAVE_RECEIVED_H264_STREAM

namespace minirtc {
namespace {

SdrColor PixelColor(CVPixelBufferRef buffer) {
  CFTypeRef matrix = CVBufferCopyAttachment(buffer, kCVImageBufferYCbCrMatrixKey, nullptr);
  const bool bt709 = matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
  if (matrix) CFRelease(matrix);
  return {CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
          bt709};
}

void RetainPixelBufferOwner(void* owner) {
  if (owner) {
    CVPixelBufferRetain(static_cast<CVPixelBufferRef>(owner));
  }
}

void ReleasePixelBufferOwner(void* owner) {
  if (owner) {
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(owner));
  }
}

int CopyPixelBufferToNv12(void* owner, uint8_t* destination,
                          size_t destination_size) {
  auto pixel_buffer = static_cast<CVPixelBufferRef>(owner);
  const OSType pixel_format =
      pixel_buffer ? CVPixelBufferGetPixelFormatType(pixel_buffer) : 0;
  if (!pixel_buffer || !destination ||
      !CVPixelBufferIsPlanar(pixel_buffer) ||
      CVPixelBufferGetPlaneCount(pixel_buffer) < 2 ||
      (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
       pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
    return -1;
  }
  const size_t width = CVPixelBufferGetWidth(pixel_buffer);
  const size_t height = CVPixelBufferGetHeight(pixel_buffer);
  size_t required_size;
  if (width > INT_MAX || height > INT_MAX ||
      !GetNv12FrameSize(width, height, &required_size) || destination_size < required_size ||
      CVPixelBufferLockBaseAddress(pixel_buffer,
                                   kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
    return -1;
  }

  const auto* y_plane = static_cast<const uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  const auto* uv_plane = static_cast<const uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
  const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
  int result = 0;
  if (!y_plane || !uv_plane || y_stride < width || uv_stride < width) {
    result = -1;
  } else {
    for (size_t row = 0; row < height; ++row) {
      memcpy(destination + row * width, y_plane + row * y_stride, width);
    }
    uint8_t* uv_destination = destination + width * height;
    for (size_t row = 0; row < height / 2U; ++row) {
      memcpy(uv_destination + row * width, uv_plane + row * uv_stride, width);
    }
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  if (result == 0) PixelColor(pixel_buffer).Normalize(destination, width, height);
  return result;
}

struct NaluUnit {
  const uint8_t* data;
  size_t size;
};

std::vector<NaluUnit> ExtractNalUnits(const uint8_t* buffer, size_t size) {
  std::vector<NaluUnit> nalus;

  size_t payload = size;
  for (size_t i = 0; i + 2 < size;) {
    if (buffer[i] || buffer[i + 1]) {
      ++i;
      continue;
    }
    size_t prefix = 0;
    if (buffer[i + 2] == 1) {
      prefix = 3;
    } else if (buffer[i + 2] == 0 && i + 3 < size && buffer[i + 3] == 1) {
      prefix = 4;
    } else {
      ++i;
      continue;
    }
    if (i > payload) nalus.push_back({buffer + payload, i - payload});
    i += prefix;
    payload = i;
  }
  if (payload < size) nalus.push_back({buffer + payload, size - payload});
  return nalus;
}

}  // namespace

class VideoToolboxDecoder::Impl {
 public:
  Impl(std::shared_ptr<SystemClock> clock, bool native_video_output, VideoCodecType codec);
  ~Impl();

  void RecoverAtKeyFrame() { session_needs_recovery_.store(true); }
  int Init();
  int Decode(std::unique_ptr<ReceivedFrame> frame,
             std::function<void(const DecodedFrame*)> on_decoded_cb);
  std::string GetDecoderName() const;

 private:
  bool CreateSession();
  static void DecodeCallback(void* decompression_output_ref_con, void* source_frame_ref_con,
                             OSStatus status, VTDecodeInfoFlags info_flags,
                             CVImageBufferRef image_buffer, CMTime pts, CMTime duration);

 private:
  std::shared_ptr<SystemClock> clock_;
  const bool native_video_output_;
  const bool hevc_;
  std::atomic<uint64_t> submitted_frame_count_{0};
  std::atomic<uint64_t> decoded_frame_count_{0};
  std::atomic<bool> session_needs_recovery_{false};
  struct FrameTimestamps {
    int64_t captured_us;
    int64_t received_us;
    std::function<void(const DecodedFrame*)> on_decoded;
  };
  std::mutex timestamps_mutex_;
  std::unordered_map<uintptr_t, FrameTimestamps> pending_timestamps_;
  uintptr_t next_frame_id_ = 0;

  VTDecompressionSessionRef decompression_session_;
  CMVideoFormatDescriptionRef format_desc_;
  std::vector<uint8_t> last_vps_;
  std::vector<uint8_t> last_sps_;
  std::vector<uint8_t> last_pps_;

  FILE* file_nv12_ = nullptr;
  FILE* file_h264_ = nullptr;
  std::string h264_file_name_;
  std::string nv12_file_name_;
};

VideoToolboxDecoder::Impl::Impl(std::shared_ptr<SystemClock> clock,
                               bool native_video_output, VideoCodecType codec)
    : clock_(std::move(clock)),
      native_video_output_(native_video_output),
      hevc_(codec == VideoCodecType::H265),
      decompression_session_(nullptr),
      format_desc_(nullptr) {}

VideoToolboxDecoder::Impl::~Impl() {
  if (decompression_session_) {
    VTDecompressionSessionWaitForAsynchronousFrames(decompression_session_);
    VTDecompressionSessionInvalidate(decompression_session_);
    CFRelease(decompression_session_);
  }
  if (format_desc_) {
    CFRelease(format_desc_);
  }

#ifdef SAVE_DECODED_NV12_STREAM
  if (file_nv12_) {
    fflush(file_nv12_);
    file_nv12_ = nullptr;
  }
#endif

#ifdef SAVE_RECEIVED_H264_STREAM
  if (file_h264_) {
    fflush(file_h264_);
    file_h264_ = nullptr;
  }
#endif
}

int VideoToolboxDecoder::Impl::Init() {
#if defined(SAVE_DECODED_NV12_STREAM) || defined(SAVE_RECEIVED_H264_STREAM)
#if defined(MINIRTC_IOS)
  std::string log_dir = std::string(getenv("TMPDIR")) + "/CrossDesk/";
#else
  std::string log_dir = std::string(getenv("HOME")) + "/Library/Logs/CrossDesk/";
  // Create directory if not exists
  std::string mkdir_cmd = "mkdir -p " + log_dir;
  system(mkdir_cmd.c_str());
#endif
#endif

#ifdef SAVE_DECODED_NV12_STREAM
  nv12_file_name_ =
      log_dir + "decoded_nv12_stream_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yuv";
  file_nv12_ = fopen(nv12_file_name_.c_str(), "w+b");
  if (!file_nv12_) {
    LOG_WARN("Fail to open {}", nv12_file_name_.c_str());
  }
#endif

#ifdef SAVE_RECEIVED_H264_STREAM
  if (!hevc_) {
    h264_file_name_ =
        log_dir + "received_h264_stream_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".h264";
    file_h264_ = fopen(h264_file_name_.c_str(), "w+b");
    if (!file_h264_) {
      LOG_WARN("Fail to open {}", h264_file_name_.c_str());
    }
  }
#endif
  return 0;
}

int VideoToolboxDecoder::Impl::Decode(
    std::unique_ptr<ReceivedFrame> received_frame,
    std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
  if (!received_frame) {
    LOG_ERROR("Received frame is null");
    return -1;
  }

  const uint8_t* data = received_frame->Buffer();
  size_t size = received_frame->Size();

#ifdef SAVE_RECEIVED_H264_STREAM
  if (file_h264_) {
    fwrite((unsigned char*)data, 1, size, file_h264_);
  }
#endif

  if (!data || !size) return -1;
  bool is_keyframe = false;
  bool has_picture = false;
  std::vector<uint8_t> sample_data;
  sample_data.reserve(size);
  for (const auto& nalu : ExtractNalUnits(data, size)) {
    if (nalu.size > UINT32_MAX ||
        (hevc_ && (nalu.size < 2 || (nalu.data[0] & 0x80) || !(nalu.data[1] & 7)))) {
      return -1;
    }
    const unsigned type = hevc_ ? (nalu.data[0] >> 1) & 0x3f : nalu.data[0] & 0x1f;
    std::vector<uint8_t>* parameter_set = nullptr;
    if (hevc_ && type == 32) {
      parameter_set = &last_vps_;
    } else if (type == (hevc_ ? 33 : 7)) {
      parameter_set = &last_sps_;
    } else if (type == (hevc_ ? 34 : 8)) {
      parameter_set = &last_pps_;
    }
    if (parameter_set) {
      if (!std::equal(nalu.data, nalu.data + nalu.size,
                      parameter_set->begin(), parameter_set->end())) {
        parameter_set->assign(nalu.data, nalu.data + nalu.size);
        session_needs_recovery_.store(true);
      }
      continue;
    }

    // HEVC random access pictures include BLA, IDR and CRA.
    is_keyframe |= hevc_ ? (type >= 16 && type <= 21) : type == 5;
    has_picture |= hevc_ ? type <= 31 : (type >= 1 && type <= 5);
    const uint32_t length = htonl(static_cast<uint32_t>(nalu.size));
    const auto* field = reinterpret_cast<const uint8_t*>(&length);
    sample_data.insert(sample_data.end(), field, field + sizeof(length));
    sample_data.insert(sample_data.end(), nalu.data, nalu.data + nalu.size);
  }
  if (!has_picture) return -1;

  if (!decompression_session_ || session_needs_recovery_.load()) {
    // A replacement session cannot use pictures referenced by the old one.
    if (!is_keyframe || (hevc_ && last_vps_.empty()) ||
        last_sps_.empty() || last_pps_.empty()) {
      return -1;
    }
    LOG_INFO("Creating/recovering {} decompression session at keyframe", GetDecoderName());
    if (!CreateSession()) return -1;
  }

  CMBlockBufferRef block_buffer = nullptr;
  OSStatus status = CMBlockBufferCreateWithMemoryBlock(
      kCFAllocatorDefault, nullptr, sample_data.size(), kCFAllocatorDefault,
      nullptr, 0, sample_data.size(), 0, &block_buffer);
  if (status != kCMBlockBufferNoErr) {
    LOG_ERROR("Failed to create block buffer, status: {}", status);
    return -1;
  }
  status = CMBlockBufferReplaceDataBytes(sample_data.data(), block_buffer, 0,
                                         sample_data.size());
  if (status != kCMBlockBufferNoErr) {
    LOG_ERROR("Failed to copy sample data into block buffer, status: {}", status);
    CFRelease(block_buffer);
    return -1;
  }

  CMSampleBufferRef sample_buffer = nullptr;
  CMSampleTimingInfo timing = {};
  timing.duration = kCMTimeInvalid;
  timing.presentationTimeStamp = CMTimeMake(received_frame->ReceivedTimestamp(), 1'000'000);
  timing.decodeTimeStamp = kCMTimeInvalid;
  // A compressed video access unit is one sample. Supplying zero sample-size
  // entries creates a formally valid CMSampleBuffer whose sample size can be
  // reported as zero, which VideoToolbox accepts without producing output.
  const size_t sample_size = sample_data.size();
  status = CMSampleBufferCreateReady(nullptr, block_buffer, format_desc_, 1, 1,
                                     &timing, 1, &sample_size, &sample_buffer);
  CFRelease(block_buffer);
  if (status != noErr) {
    LOG_ERROR("Failed to create sample buffer, status: {}", status);
    return -1;
  }

  CFArrayRef attachments =
      CMSampleBufferGetSampleAttachmentsArray(sample_buffer, true);
  if (attachments && CFArrayGetCount(attachments) > 0) {
    auto attachment = static_cast<CFMutableDictionaryRef>(
        const_cast<void*>(CFArrayGetValueAtIndex(attachments, 0)));
    CFDictionarySetValue(attachment, kCMSampleAttachmentKey_DisplayImmediately,
                         kCFBooleanTrue);
    CFDictionarySetValue(attachment, kCMSampleAttachmentKey_NotSync,
                         is_keyframe ? kCFBooleanFalse : kCFBooleanTrue);
  }

  uintptr_t frame_id;
  {
    std::lock_guard lock(timestamps_mutex_);
    frame_id = ++next_frame_id_;
    pending_timestamps_[frame_id] = {received_frame->CapturedTimestamp(),
                                     received_frame->ReceivedTimestamp(),
                                     std::move(on_receive_decoded_frame)};
  }
  VTDecodeInfoFlags decode_flags = 0;
  status = VTDecompressionSessionDecodeFrame(decompression_session_, sample_buffer,
                                             kVTDecodeFrame_EnableAsynchronousDecompression,
                                             reinterpret_cast<void*>(frame_id), &decode_flags);
  if (status != noErr || (decode_flags & kVTDecodeInfo_FrameDropped)) {
    std::lock_guard lock(timestamps_mutex_);
    pending_timestamps_.erase(frame_id);
  }
  CFRelease(sample_buffer);
  if (status != noErr) {
    session_needs_recovery_.store(true);
    LOG_ERROR("VTDecompressionSessionDecodeFrame failed, status: {}", status);
  } else {
    const uint64_t frame_count = ++submitted_frame_count_;
    if (frame_count == 1 || frame_count % 300 == 0) {
      LOG_DEBUG("VideoToolbox submitted frame {}, sample_size={}, keyframe={}",
               frame_count, sample_size, is_keyframe);
    }
  }
  return (status == noErr) ? 0 : -1;
}

std::string VideoToolboxDecoder::Impl::GetDecoderName() const {
  return hevc_ ? "VideoToolbox (H.265)" : "VideoToolbox (H.264)";
}

bool VideoToolboxDecoder::Impl::CreateSession() {
  if (decompression_session_) {
    VTDecompressionSessionWaitForAsynchronousFrames(decompression_session_);
    VTDecompressionSessionInvalidate(decompression_session_);
    CFRelease(decompression_session_);
    decompression_session_ = nullptr;
  }
  {
    std::lock_guard lock(timestamps_mutex_);
    pending_timestamps_.clear();
  }
  if (format_desc_) {
    CFRelease(format_desc_);
    format_desc_ = nullptr;
  }
  // WaitForAsynchronousFrames above can deliver errors from the old session.
  // Clear them only after it has drained; callbacks never recreate a session.
  session_needs_recovery_.store(false);
  const uint8_t* sets[] = {last_vps_.data(), last_sps_.data(), last_pps_.data()};
  const size_t sizes[] = {last_vps_.size(), last_sps_.size(), last_pps_.size()};
  OSStatus status = hevc_
      ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            nullptr, 3, sets, sizes, 4, nullptr, &format_desc_)
      : CMVideoFormatDescriptionCreateFromH264ParameterSets(
            nullptr, 2, sets + 1, sizes + 1, 4, &format_desc_);
  if (status != noErr) {
    LOG_ERROR("Failed to create {} format description: {}", GetDecoderName(), status);
    return false;
  }

  VTDecompressionOutputCallbackRecord callback = {};
  callback.decompressionOutputCallback = &DecodeCallback;
  callback.decompressionOutputRefCon = this;

  CFMutableDictionaryRef decoder_spec = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  // Leave the simulator free to select its available software decoder.
#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR
  if (@available(iOS 17.0, *)) {
    CFDictionarySetValue(
        decoder_spec,
        kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
        kCFBooleanTrue);
  }
#elif !TARGET_OS_IOS
  CFDictionarySetValue(
      decoder_spec, hevc_ ? kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder
                          : kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
      kCFBooleanTrue);
#endif

  CFMutableDictionaryRef output_attributes = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  int32_t pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  CFNumberRef pixel_format_number = CFNumberCreate(
      kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
  CFDictionarySetValue(output_attributes, kCVPixelBufferPixelFormatTypeKey,
                       pixel_format_number);
  CFDictionarySetValue(output_attributes, kCVPixelBufferMetalCompatibilityKey,
                       kCFBooleanTrue);
  CFDictionaryRef io_surface_properties = CFDictionaryCreate(
      kCFAllocatorDefault, nullptr, nullptr, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(output_attributes, kCVPixelBufferIOSurfacePropertiesKey,
                       io_surface_properties);

  status = VTDecompressionSessionCreate(nullptr, format_desc_, decoder_spec,
                                        output_attributes, &callback,
                                        &decompression_session_);
  CFRelease(io_surface_properties);
  CFRelease(pixel_format_number);
  CFRelease(output_attributes);
  CFRelease(decoder_spec);
  if (status != noErr) {
    LOG_ERROR("Failed to create VideoToolbox decompression session, status: {}",
              status);
  }
  return status == noErr;
}

void VideoToolboxDecoder::Impl::DecodeCallback(void* decompression_output_ref_con,
                                               void* source_frame_ref_con, OSStatus status,
                                               VTDecodeInfoFlags info_flags,
                                               CVImageBufferRef image_buffer, CMTime pts,
                                               CMTime duration) {
  VideoToolboxDecoder::Impl* impl =
      static_cast<VideoToolboxDecoder::Impl*>(decompression_output_ref_con);
  if (!impl) {
    LOG_ERROR("Decode callback received null decompression output ref con");
    return;
  }

  FrameTimestamps timestamps{};
  {
    std::lock_guard lock(impl->timestamps_mutex_);
    const auto it =
        impl->pending_timestamps_.find(reinterpret_cast<uintptr_t>(source_frame_ref_con));
    if (it == impl->pending_timestamps_.end()) return;
    timestamps = std::move(it->second);
    impl->pending_timestamps_.erase(it);
  }

  if (status != noErr) {
    impl->session_needs_recovery_.store(true);
    LOG_ERROR("Decode callback received error status: {}", status);
    return;
  }

  if (impl->session_needs_recovery_.load()) return;

  if (!image_buffer) {
    LOG_ERROR("Decode callback received null image buffer");
    return;
  }

  CVPixelBufferRef pixel_buffer = static_cast<CVPixelBufferRef>(image_buffer);
  const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
  if (!CVPixelBufferIsPlanar(pixel_buffer) ||
      CVPixelBufferGetPlaneCount(pixel_buffer) < 2 ||
      (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
       pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
    LOG_ERROR("Unexpected VideoToolbox output pixel format: {}, planes: {}",
              pixel_format, CVPixelBufferGetPlaneCount(pixel_buffer));
    return;
  }

  const uint32_t width = static_cast<uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
  const uint32_t height = static_cast<uint32_t>(CVPixelBufferGetHeight(pixel_buffer));

  size_t frame_size;
  if (!GetNv12FrameSize(width, height, &frame_size)) return;
  DecodedFrame decoded_frame;
  MiniRtcNativeVideoFrame native_frame{};
  bool native_output = impl->native_video_output_;
#if defined(MINIRTC_IOS)
  native_output = native_output && !PixelColor(pixel_buffer).NeedsConversion();
#endif
  if (native_output) {
    // All Apple consumers can present the IOSurface-backed image directly.
    // The descriptor remains borrowed and is valid only during this callback.
    native_frame.struct_size = sizeof(native_frame);
    native_frame.type = MiniRtcNativeVideoFrameCVPixelBuffer;
    native_frame.width = width;
    native_frame.height = height;
    native_frame.payload.cv_pixel_buffer = pixel_buffer;
    native_frame.owner = pixel_buffer;
    native_frame.retain = &RetainPixelBufferOwner;
    native_frame.release = &ReleasePixelBufferOwner;
    native_frame.copy_to_nv12 = &CopyPixelBufferToNv12;
    decoded_frame.SetNativeFrame(&native_frame);
    decoded_frame.SetSize(frame_size);
  } else {
    decoded_frame = DecodedFrame(frame_size, width, height);
    if (CopyPixelBufferToNv12(pixel_buffer, decoded_frame.MutableBuffer(), frame_size) != 0) {
      return;
    }
  }
  decoded_frame.SetDecodedWidth(width);
  decoded_frame.SetDecodedHeight(height);
  decoded_frame.SetCapturedTimestamp(timestamps.captured_us);
  decoded_frame.SetReceivedTimestamp(timestamps.received_us);
  decoded_frame.SetDecodedTimestamp(impl->clock_->CurrentTimeUs());

  const uint64_t frame_count = ++impl->decoded_frame_count_;
  if (frame_count == 1 || frame_count % 300 == 0) {
    LOG_DEBUG("VideoToolbox decoded frame {}, size={}x{}, pixel_format={}",
             frame_count, width, height, pixel_format);
  }

  if (timestamps.on_decoded) {
#ifdef SAVE_DECODED_NV12_STREAM
    fwrite((unsigned char*)decoded_frame.Buffer(), 1, decoded_frame.Size(),
           impl->file_nv12_);
#endif
    timestamps.on_decoded(&decoded_frame);
  }
}

//

VideoToolboxDecoder::VideoToolboxDecoder(std::shared_ptr<SystemClock> clock,
                                         bool native_video_output, VideoCodecType codec)
    : impl_(std::make_shared<Impl>(std::move(clock), native_video_output, codec)) {}

VideoToolboxDecoder::~VideoToolboxDecoder() = default;

void VideoToolboxDecoder::RecoverAtKeyFrame() { impl_->RecoverAtKeyFrame(); }

int VideoToolboxDecoder::Init() { return impl_->Init(); }

int VideoToolboxDecoder::Decode(std::unique_ptr<ReceivedFrame> received_frame,
                                std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
  return impl_->Decode(std::move(received_frame), std::move(on_receive_decoded_frame));
}

std::string VideoToolboxDecoder::GetDecoderName() const { return impl_->GetDecoderName(); }

}  // namespace minirtc
