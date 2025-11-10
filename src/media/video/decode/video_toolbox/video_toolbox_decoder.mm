#include "video_toolbox_decoder.h"
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace minirtc {

struct NaluUnit {
  const uint8_t* data;
  size_t size;
  uint8_t type;
};

std::vector<NaluUnit> ExtractNalUnits(const uint8_t* buffer, size_t size) {
  std::vector<NaluUnit> nalus;

  size_t i = 0;
  while (i + 4 < size) {
    size_t start_code_len = 0;
    if (buffer[i] == 0x00 && buffer[i + 1] == 0x00) {
      if (buffer[i + 2] == 0x01) {
        start_code_len = 3;
      } else if (buffer[i + 2] == 0x00 && buffer[i + 3] == 0x01) {
        start_code_len = 4;
      }
    }

    if (start_code_len == 0) {
      ++i;
      continue;
    }

    size_t nalu_start = i + start_code_len;
    size_t next_start = nalu_start;
    while (next_start + 4 < size) {
      if (buffer[next_start] == 0x00 && buffer[next_start + 1] == 0x00 &&
          (buffer[next_start + 2] == 0x01 ||
           (buffer[next_start + 2] == 0x00 && buffer[next_start + 3] == 0x01))) {
        break;
      }
      ++next_start;
    }

    size_t nalu_size = next_start - nalu_start;
    if (nalu_size > 0 && nalu_start + nalu_size <= size) {
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
  Impl(std::shared_ptr<SystemClock> clock);
  ~Impl();

  int Init();
  int Decode(std::unique_ptr<ReceivedFrame> frame,
             std::function<void(const DecodedFrame*)> on_decoded_cb);
  std::string GetDecoderName();

 private:
  bool CreateSession(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps);
  static void DecodeCallback(void* decompression_output_ref_con, void* source_frame_ref_con,
                             OSStatus status, VTDecodeInfoFlags info_flags,
                             CVImageBufferRef image_buffer, CMTime pts, CMTime duration);

 private:
  std::shared_ptr<SystemClock> clock_;
  std::function<void(const DecodedFrame*)> on_receive_decoded_frame_;
  DecodedFrame* decoded_frame_ = nullptr;
  uint8_t* nv12_frame_ = nullptr;
  size_t nv12_frame_size_ = 0;
  uint32_t frame_width_ = 0;
  uint32_t frame_height_ = 0;

  VTDecompressionSessionRef decompression_session_;
  CMVideoFormatDescriptionRef format_desc_;
  std::vector<uint8_t> last_sps_;
  std::vector<uint8_t> last_pps_;

  FILE* file_nv12_ = nullptr;
  FILE* file_h264_ = nullptr;
  std::string h264_file_name_;
  std::string nv12_file_name_;
};

VideoToolboxDecoder::Impl::Impl(std::shared_ptr<SystemClock> clock)
    : clock_(std::move(clock)), decompression_session_(nullptr), format_desc_(nullptr) {}

VideoToolboxDecoder::Impl::~Impl() {
  if (decompression_session_) {
    VTDecompressionSessionInvalidate(decompression_session_);
    CFRelease(decompression_session_);
  }
  if (format_desc_) {
    CFRelease(format_desc_);
  }
  if (nv12_frame_) {
    delete[] nv12_frame_;
    nv12_frame_ = nullptr;
  }
  if (decoded_frame_) {
    delete decoded_frame_;
    decoded_frame_ = nullptr;
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
#ifdef SAVE_DECODED_NV12_STREAM
  nv12_file_name_ =
      "decoded_nv12_stream_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yuv";
  file_nv12_ = fopen(nv12_file_name_.c_str(), "w+b");
  if (!file_nv12_) {
    LOG_WARN("Fail to open {}", nv12_file_name_.c_str());
  }
#endif

#ifdef SAVE_RECEIVED_H264_STREAM
  h264_file_name_ =
      "received_h264_stream_" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".h264";
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

  if (size > 4 && (*(data + 4) & 0x1f) == 0x07) {
    std::vector<uint8_t> sps, pps;
    if (!ExtractSpsPps(data, size, sps, pps)) {
      LOG_ERROR("Failed to extract SPS/PPS from frame data");
      return -1;
    }

    if (sps != last_sps_ || pps != last_pps_) {
      if (!CreateSession(sps, pps)) {
        LOG_ERROR("Failed to create decompression session");
        return -1;
      }
      last_sps_ = sps;
      last_pps_ = pps;
    }
  }

  auto avcc_data = ConvertAnnexBToAVCCFiltered(data, size);
  const uint8_t* avcc_data_ptr = avcc_data.data();
  size_t avcc_data_size = avcc_data.size();

  if (!decompression_session_) {
    LOG_ERROR("Decompression session is not initialized");
    return -1;
  }

  CMBlockBufferRef block_buffer = nullptr;
  OSStatus status = CMBlockBufferCreateWithMemoryBlock(nullptr, (void*)avcc_data_ptr,
                                                       avcc_data_size, kCFAllocatorNull, nullptr, 0,
                                                       avcc_data_size, 0, &block_buffer);
  if (status != kCMBlockBufferNoErr) {
    LOG_ERROR("Failed to create block buffer");
    return -1;
  }

  CMSampleBufferRef sample_buffer = nullptr;
  CMSampleTimingInfo timing = {};
  timing.duration = kCMTimeInvalid;
  timing.presentationTimeStamp = CMTimeMake(received_frame->ReceivedTimestamp(), 1000);
  timing.decodeTimeStamp = kCMTimeInvalid;
  status = CMSampleBufferCreateReady(nullptr, block_buffer, format_desc_, 1, 1, &timing, 0, nullptr,
                                     &sample_buffer);
  CFRelease(block_buffer);
  if (status != noErr) {
    LOG_ERROR("Failed to create sample buffer");
    return -1;
  }

  status = VTDecompressionSessionDecodeFrame(decompression_session_, sample_buffer,
                                             kVTDecodeFrame_EnableAsynchronousDecompression,
                                             nullptr, nullptr);
  CFRelease(sample_buffer);
  return (status == noErr) ? 0 : -1;
}

std::string VideoToolboxDecoder::Impl::GetDecoderName() const { return "VideoToolboxH264"; }

bool VideoToolboxDecoder::Impl::CreateSession(const std::vector<uint8_t>& sps,
                                              const std::vector<uint8_t>& pps) {
  if (decompression_session_) {
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
  CFDictionarySetValue(decoder_spec,
                       kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
                       kCFBooleanTrue);

  status = VTDecompressionSessionCreate(nullptr, format_desc_, decoder_spec, nullptr, &callback,
                                        &decompression_session_);
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

  {
    if (!CVPixelBufferIsPlanar(image_buffer)) {
      LOG_ERROR("Image buffer is not planar, expected NV12 format");
      return;
    }

    CVPixelBufferRef pixelBuffer = static_cast<CVPixelBufferRef>(image_buffer);
    CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    impl->frame_width_ = CVPixelBufferGetWidth(pixelBuffer);
    impl->frame_height_ = CVPixelBufferGetHeight(pixelBuffer);
    size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
    size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

    const uint8_t* y_plane =
        static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
    const uint8_t* uv_plane =
        static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));

    size_t nv12_size = y_stride * impl->frame_height_ + uv_stride * impl->frame_height_ / 2;

    if (!impl->nv12_frame_) {
      impl->nv12_frame_ = new uint8_t[nv12_size];
      impl->nv12_frame_size_ = nv12_size;
    }

    if (impl->nv12_frame_ && impl->nv12_frame_size_ < nv12_size) {
      delete[] impl->nv12_frame_;
      impl->nv12_frame_ = new uint8_t[nv12_size];
      impl->nv12_frame_size_ = nv12_size;
    }

    if (!impl->decoded_frame_) {
      impl->decoded_frame_ =
          new DecodedFrame(impl->nv12_frame_size_, impl->frame_width_, impl->frame_height_);
    }

    for (size_t i = 0; i < impl->frame_height_; ++i) {
      memcpy(impl->nv12_frame_ + i * impl->frame_width_, y_plane + i * y_stride,
             impl->frame_width_);
    }

    uint8_t* uv_dst = impl->nv12_frame_ + impl->frame_width_ * impl->frame_height_;
    for (size_t i = 0; i < impl->frame_height_ / 2; ++i) {
      memcpy(uv_dst + i * impl->frame_width_, uv_plane + i * uv_stride, impl->frame_width_);
    }

    impl->decoded_frame_->UpdateBuffer(impl->nv12_frame_, impl->nv12_frame_size_);
    impl->decoded_frame_->SetWidth(impl->frame_width_);
    impl->decoded_frame_->SetHeight(impl->frame_height_);
    impl->decoded_frame_->SetDecodedWidth(impl->frame_width_);
    impl->decoded_frame_->SetDecodedHeight(impl->frame_height_);
    impl->decoded_frame_->SetDecodedTimestamp(
        static_cast<int64_t>(CMTimeGetSeconds(pts) * 1'000'000));

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
  }

  if (impl->on_receive_decoded_frame_) {
#ifdef SAVE_DECODED_NV12_STREAM
    fwrite((unsigned char*)impl->decoded_frame_->Buffer(), 1, impl->decoded_frame_->Size(),
           impl->file_nv12_);
#endif
    impl->on_receive_decoded_frame_(impl->decoded_frame_);
  }
}

//

VideoToolboxDecoder::VideoToolboxDecoder(std::shared_ptr<SystemClock> clock)
    : impl_(std::make_shared<Impl>(std::move(clock))) {}

VideoToolboxDecoder::~VideoToolboxDecoder() = default;

int VideoToolboxDecoder::Init() { return impl_->Init(); }

int VideoToolboxDecoder::Decode(std::unique_ptr<ReceivedFrame> received_frame,
                                std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
  return impl_->Decode(std::move(received_frame), std::move(on_receive_decoded_frame));
}

std::string VideoToolboxDecoder::GetDecoderName() { return impl_->GetDecoderName(); }

}  // namespace minirtc