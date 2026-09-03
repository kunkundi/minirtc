#include "video_toolbox_decoder.h"
#include "minirtc.h"
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <TargetConditionals.h>
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// #define SAVE_DECODED_NV12_STREAM
// #define SAVE_RECEIVED_H264_STREAM

namespace minirtc {
namespace {

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
  const size_t required_size = width * height * 3U / 2U;
  if (destination_size < required_size ||
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
  return result;
}

}  // namespace

struct NaluUnit {
  const uint8_t* data;
  size_t size;
  uint8_t type;
};

std::vector<NaluUnit> ExtractNalUnits(const uint8_t* buffer, size_t size) {
  std::vector<NaluUnit> nalus;

  size_t i = 0;
  while (i + 3 <= size) {  // need at least a 3-byte start code
    size_t start_code_len = 0;
    if (buffer[i] == 0x00 && buffer[i + 1] == 0x00) {
      if (i + 3 <= size && buffer[i + 2] == 0x01) {
        start_code_len = 3;
      } else if (i + 4 <= size && buffer[i + 2] == 0x00 && buffer[i + 3] == 0x01) {
        start_code_len = 4;
      }
    }

    if (start_code_len == 0) {
      ++i;
      continue;
    }

    size_t nalu_start = i + start_code_len;
    if (nalu_start >= size) {
      break;  // no more data
    }

    // find the next start code
    size_t next_start = nalu_start + 1;  // start searching after the NALU data
    while (next_start + 2 < size) {
      if (buffer[next_start] == 0x00 && buffer[next_start + 1] == 0x00 &&
          (buffer[next_start + 2] == 0x01 ||
           (next_start + 3 < size && buffer[next_start + 2] == 0x00 && buffer[next_start + 3] == 0x01))) {
        break;
      }
      ++next_start;
    }
    
    // if no next start code found, this is the last NALU; take until buffer end
    if (next_start + 2 >= size) {
      next_start = size;
    }

    size_t nalu_size = next_start - nalu_start;
    if (nalu_size > 0) {
      uint8_t type = buffer[nalu_start] & 0x1F;
      nalus.push_back(NaluUnit{buffer + nalu_start, nalu_size, type});
    }

    i = next_start;
  }

  return nalus;
}

bool ExtractSpsPps(const uint8_t* buffer, size_t size, std::vector<uint8_t>& sps,
                   std::vector<uint8_t>& pps) {
  auto nalus = ExtractNalUnits(buffer, size);
  for (const auto& nalu : nalus) {
    if (nalu.type == 7) {
      sps.assign(nalu.data, nalu.data + nalu.size);
    } else if (nalu.type == 8) {
      pps.assign(nalu.data, nalu.data + nalu.size);
    }
  }

  return !sps.empty() && !pps.empty();
}

std::vector<NaluUnit> ExtractVideoNalUnits(const uint8_t* buffer, size_t size) {
  auto all_nalus = ExtractNalUnits(buffer, size);
  std::vector<NaluUnit> filtered;
  for (const auto& nalu : all_nalus) {
    if (nalu.type != 7 && nalu.type != 8) {
      filtered.push_back(nalu);
    }
  }
  return filtered;
}

std::vector<uint8_t> ConvertAnnexBToAVCCFiltered(const uint8_t* data, size_t size) {
  std::vector<uint8_t> avcc_data;
  auto nalus = ExtractVideoNalUnits(data, size);
  for (const auto& nalu : nalus) {
    uint32_t len = htonl(static_cast<uint32_t>(nalu.size));
    avcc_data.insert(avcc_data.end(), reinterpret_cast<uint8_t*>(&len),
                     reinterpret_cast<uint8_t*>(&len) + 4);
    avcc_data.insert(avcc_data.end(), nalu.data, nalu.data + nalu.size);
  }
  return avcc_data;
}

class VideoToolboxDecoder::Impl {
 public:
  Impl(std::shared_ptr<SystemClock> clock, bool native_video_output);
  ~Impl();

  int Init();
  int Decode(std::unique_ptr<ReceivedFrame> frame,
             std::function<void(const DecodedFrame*)> on_decoded_cb);
  std::string GetDecoderName() const;

 private:
  bool CreateSession(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps);
  static void DecodeCallback(void* decompression_output_ref_con, void* source_frame_ref_con,
                             OSStatus status, VTDecodeInfoFlags info_flags,
                             CVImageBufferRef image_buffer, CMTime pts, CMTime duration);

 private:
  std::shared_ptr<SystemClock> clock_;
  const bool native_video_output_;
  std::function<void(const DecodedFrame*)> on_receive_decoded_frame_;
  std::atomic<uint64_t> submitted_frame_count_{0};
  std::atomic<uint64_t> decoded_frame_count_{0};

  VTDecompressionSessionRef decompression_session_;
  CMVideoFormatDescriptionRef format_desc_;
  std::vector<uint8_t> last_sps_;
  std::vector<uint8_t> last_pps_;

  FILE* file_nv12_ = nullptr;
  FILE* file_h264_ = nullptr;
  std::string h264_file_name_;
  std::string nv12_file_name_;
};

VideoToolboxDecoder::Impl::Impl(std::shared_ptr<SystemClock> clock,
                               bool native_video_output)
    : clock_(std::move(clock)),
      native_video_output_(native_video_output),
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
  last_sps_.clear();
  last_pps_.clear();

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
  h264_file_name_ =
      log_dir + "received_h264_stream_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".h264";
  file_h264_ = fopen(h264_file_name_.c_str(), "w+b");
  if (!file_h264_) {
    LOG_WARN("Fail to open {}", h264_file_name_.c_str());
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

  if (!on_receive_decoded_frame_) {
    on_receive_decoded_frame_ = on_receive_decoded_frame;
  }

  const uint8_t* data = received_frame->Buffer();
  size_t size = received_frame->Size();

#ifdef SAVE_RECEIVED_H264_STREAM
  if (file_h264_) {
    fwrite((unsigned char*)data, 1, size, file_h264_);
  }
#endif

  // check if frame contains SPS (NAL type 7) using a more robust method
  // no longer assume start code is 4 bytes, parse directly with ExtractNalUnits
  bool has_sps = false;
  bool is_keyframe = false;
  auto all_nalus = ExtractNalUnits(data, size);
  for (const auto& nalu : all_nalus) {
    if (nalu.type == 7) {  // SPS
      has_sps = true;
    } else if (nalu.type == 5) {  // IDR
      is_keyframe = true;
    }
  }

  if (has_sps) {
    std::vector<uint8_t> sps, pps;
    if (!ExtractSpsPps(data, size, sps, pps)) {
      LOG_ERROR("Failed to extract SPS/PPS from frame data");
      return -1;
    }

    if (sps != last_sps_ || pps != last_pps_) {
      LOG_INFO("Creating new decompression session with SPS/PPS");
      if (!CreateSession(sps, pps)) {
        LOG_ERROR("Failed to create decompression session");
        return -1;
      }
      last_sps_ = sps;
      last_pps_ = pps;
    }
  }

  auto avcc_data = ConvertAnnexBToAVCCFiltered(data, size);
  if (avcc_data.empty()) {
    LOG_ERROR("Failed to convert Annex B to AVCC format");
    return -1;
  }

  if (!decompression_session_) {
    LOG_ERROR("Decompression session is not initialized");
    return -1;
  }

  CMBlockBufferRef block_buffer = nullptr;
  OSStatus status = CMBlockBufferCreateWithMemoryBlock(
      kCFAllocatorDefault, nullptr, avcc_data.size(), kCFAllocatorDefault,
      nullptr, 0, avcc_data.size(), 0, &block_buffer);
  if (status != kCMBlockBufferNoErr) {
    LOG_ERROR("Failed to create block buffer, status: {}", status);
    return -1;
  }
  status = CMBlockBufferReplaceDataBytes(avcc_data.data(), block_buffer, 0,
                                         avcc_data.size());
  if (status != kCMBlockBufferNoErr) {
    LOG_ERROR("Failed to copy AVCC data into block buffer, status: {}", status);
    CFRelease(block_buffer);
    return -1;
  }

  CMSampleBufferRef sample_buffer = nullptr;
  CMSampleTimingInfo timing = {};
  timing.duration = kCMTimeInvalid;
  timing.presentationTimeStamp = CMTimeMake(received_frame->ReceivedTimestamp(), 1000);
  timing.decodeTimeStamp = kCMTimeInvalid;
  // A compressed video access unit is one sample. Supplying zero sample-size
  // entries creates a formally valid CMSampleBuffer whose sample size can be
  // reported as zero, which VideoToolbox accepts without producing output.
  const size_t sample_size = avcc_data.size();
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

  status = VTDecompressionSessionDecodeFrame(decompression_session_, sample_buffer,
                                             kVTDecodeFrame_EnableAsynchronousDecompression,
                                             nullptr, nullptr);
  CFRelease(sample_buffer);
  if (status != noErr) {
    LOG_ERROR("VTDecompressionSessionDecodeFrame failed, status: {}", status);
  } else {
    const uint64_t frame_count = ++submitted_frame_count_;
    if (frame_count == 1 || frame_count % 300 == 0) {
      LOG_INFO("VideoToolbox submitted frame {}, sample_size={}, keyframe={}",
               frame_count, sample_size, is_keyframe);
    }
  }
  return (status == noErr) ? 0 : -1;
}

std::string VideoToolboxDecoder::Impl::GetDecoderName() const { return "VideoToolboxH264"; }

bool VideoToolboxDecoder::Impl::CreateSession(const std::vector<uint8_t>& sps,
                                              const std::vector<uint8_t>& pps) {
  if (decompression_session_) {
    VTDecompressionSessionWaitForAsynchronousFrames(decompression_session_);
    VTDecompressionSessionInvalidate(decompression_session_);
    CFRelease(decompression_session_);
    decompression_session_ = nullptr;
  }
  if (format_desc_) {
    CFRelease(format_desc_);
    format_desc_ = nullptr;
  }
  const uint8_t* sets[] = {sps.data(), pps.data()};
  const size_t sizes[] = {sps.size(), pps.size()};
  size_t param_set_cnt = 2;  // at least 2 (SPS and PPS)
  size_t nalu_head_len = 4;  // 1, 2, 4 bytes are common, but 4 is safest for H.264
  OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
      nullptr, param_set_cnt, sets, sizes, nalu_head_len, &format_desc_);
  if (status != noErr) {
    LOG_ERROR("Failed to create format description from SPS/PPS");
    return false;
  }

  VTDecompressionOutputCallbackRecord callback = {};
  callback.decompressionOutputCallback = &DecodeCallback;
  callback.decompressionOutputRefCon = this;

  CFMutableDictionaryRef decoder_spec = CFDictionaryCreateMutable(
      kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
#if TARGET_OS_IOS
  if (@available(iOS 17.0, *)) {
    CFDictionarySetValue(
        decoder_spec,
        kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
        kCFBooleanTrue);
  }
#else
  CFDictionarySetValue(
      decoder_spec,
      kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
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

  if (status != noErr) {
    LOG_ERROR("Decode callback received error status: {}", status);
    return;
  }

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

  DecodedFrame decoded_frame;
  XNativeVideoFrame native_frame{};
  if (impl->native_video_output_) {
    // All Apple consumers can present the IOSurface-backed image directly.
    // The descriptor remains borrowed and is valid only during this callback.
    native_frame.struct_size = sizeof(native_frame);
    native_frame.type = XNativeVideoFrameCVPixelBuffer;
    native_frame.width = width;
    native_frame.height = height;
    native_frame.payload.cv_pixel_buffer = pixel_buffer;
    native_frame.owner = pixel_buffer;
    native_frame.retain = &RetainPixelBufferOwner;
    native_frame.release = &ReleasePixelBufferOwner;
    native_frame.copy_to_nv12 = &CopyPixelBufferToNv12;
    decoded_frame.SetNativeFrame(&native_frame);
  } else {
    CVReturn lock_status =
        CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    if (lock_status != kCVReturnSuccess) {
      LOG_ERROR("Failed to lock decoded pixel buffer: {}", lock_status);
      return;
    }

    const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
    const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
    const uint8_t* y_plane = static_cast<const uint8_t*>(
        CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
    const uint8_t* uv_plane = static_cast<const uint8_t*>(
        CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));

    if (!y_plane || !uv_plane || y_stride < width || uv_stride < width) {
      LOG_ERROR("Invalid decoded NV12 planes: {}x{}, strides {} / {}", width,
                height, y_stride, uv_stride);
      CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
      return;
    }

    const size_t packed_size = static_cast<size_t>(width) * height * 3 / 2;
    std::vector<uint8_t> packed_nv12(packed_size);
    for (size_t row = 0; row < height; ++row) {
      memcpy(packed_nv12.data() + row * width, y_plane + row * y_stride,
             width);
    }
    uint8_t* uv_dst =
        packed_nv12.data() + static_cast<size_t>(width) * height;
    for (size_t row = 0; row < height / 2; ++row) {
      memcpy(uv_dst + row * width, uv_plane + row * uv_stride, width);
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer,
                                   kCVPixelBufferLock_ReadOnly);

    decoded_frame.UpdateBuffer(packed_nv12.data(), packed_nv12.size());
  }
  decoded_frame.SetDecodedWidth(width);
  decoded_frame.SetDecodedHeight(height);
  decoded_frame.SetDecodedTimestamp(
      CMTIME_IS_NUMERIC(pts)
          ? static_cast<int64_t>(CMTimeGetSeconds(pts) * 1'000'000)
          : 0);

  const uint64_t frame_count = ++impl->decoded_frame_count_;
  if (frame_count == 1 || frame_count % 300 == 0) {
    LOG_INFO("VideoToolbox decoded frame {}, size={}x{}, pixel_format={}",
             frame_count, width, height, pixel_format);
  }

  if (impl->on_receive_decoded_frame_) {
#ifdef SAVE_DECODED_NV12_STREAM
    fwrite((unsigned char*)decoded_frame.Buffer(), 1, decoded_frame.Size(),
           impl->file_nv12_);
#endif
    impl->on_receive_decoded_frame_(&decoded_frame);
  }
}

//

VideoToolboxDecoder::VideoToolboxDecoder(std::shared_ptr<SystemClock> clock,
                                         bool native_video_output)
    : impl_(std::make_shared<Impl>(std::move(clock), native_video_output)) {}

VideoToolboxDecoder::~VideoToolboxDecoder() = default;

int VideoToolboxDecoder::Init() { return impl_->Init(); }

int VideoToolboxDecoder::Decode(std::unique_ptr<ReceivedFrame> received_frame,
                                std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
  return impl_->Decode(std::move(received_frame), std::move(on_receive_decoded_frame));
}

std::string VideoToolboxDecoder::GetDecoderName() const { return impl_->GetDecoderName(); }

}  // namespace minirtc
