#include "video_toolbox_decoder.h"
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

struct NaluUnit {
  const uint8_t* data;
  size_t size;
  uint8_t type;
};

std::vector<NaluUnit> ExtractNalUnits(const uint8_t* buffer, size_t size) {
  std::vector<NaluUnit> nalus;

  size_t i = 0;
  while (i + 4 < size) {
    // 查找 start code
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

    // 查找下一个起始码
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

// 简单的转换函数
static std::unique_ptr<DecodedFrame> ConvertToDecodedFrame(CVImageBufferRef imageBuffer,
                                                           CMTime pts) {
  if (!imageBuffer || !CVPixelBufferIsPlanar(imageBuffer)) {
    return nullptr;
  }

  CVPixelBufferRef pixelBuffer = static_cast<CVPixelBufferRef>(imageBuffer);
  CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

  size_t width = CVPixelBufferGetWidth(pixelBuffer);
  size_t height = CVPixelBufferGetHeight(pixelBuffer);
  size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
  size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

  const uint8_t* y_plane =
      static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0));
  const uint8_t* uv_plane =
      static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1));

  // NV12: Y plane (W*H) + interleaved UV plane (W*H/2)
  size_t nv12_size = y_stride * height + uv_stride * height / 2;

  size_t nv12_frame_capacity = width * height * 3 / 2;
  uint8_t* nv12_frame = new uint8_t[nv12_size];
  std::unique_ptr<DecodedFrame> frame = std::make_unique<DecodedFrame>(
      nv12_size, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

  // Copy Y plane
  for (size_t i = 0; i < height; ++i) {
    memcpy(nv12_frame + i * width, y_plane + i * y_stride, width);
  }

  // Copy UV plane
  uint8_t* uv_dst = nv12_frame + width * height;
  for (size_t i = 0; i < height / 2; ++i) {
    memcpy(uv_dst + i * width, uv_plane + i * uv_stride, width);
  }

  frame->UpdateBuffer(nv12_frame, nv12_size);
  frame->SetWidth(width);
  frame->SetHeight(height);
  frame->SetDecodedWidth(static_cast<uint32_t>(width));
  frame->SetDecodedHeight(static_cast<uint32_t>(height));
  frame->SetDecodedTimestamp(static_cast<int64_t>(CMTimeGetSeconds(pts) * 1'000'000));

  CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

  delete[] nv12_frame;
  return frame;
}

class VideoToolboxDecoder::Impl {
 public:
  explicit Impl(std::shared_ptr<SystemClock> clock)
      : clock_(std::move(clock)), decompression_session_(nullptr), format_desc_(nullptr) {}

  ~Impl() {
    if (decompression_session_) {
      VTDecompressionSessionInvalidate(decompression_session_);
      CFRelease(decompression_session_);
    }
    if (format_desc_) {
      CFRelease(format_desc_);
    }
  }

  int Init() {
    // 不做SPS/PPS解析，等收到码流再处理
    return 0;
  }

  int Decode(std::unique_ptr<ReceivedFrame> frame,
             std::function<void(const DecodedFrame*)> on_decoded_cb) {
    if (!frame) {
      LOG_ERROR("Received frame is null");
      return -1;
    }

    const uint8_t* data = frame->Buffer();
    size_t size = frame->Size();

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

    auto avcc_data = ConvertAnnexBToAVCCFiltered(frame->Buffer(), frame->Size());
    const uint8_t* data1 = avcc_data.data();
    size_t size1 = avcc_data.size();

    if (!decompression_session_) {
      LOG_ERROR("Decompression session is not initialized");
      return -1;
    }

    // 创建CMBlockBuffer
    CMBlockBufferRef block_buffer = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        nullptr, (void*)data1, size1, kCFAllocatorNull, nullptr, 0, size1, 0, &block_buffer);
    if (status != kCMBlockBufferNoErr) {
      LOG_ERROR("Failed to create block buffer");
      return -1;
    }

    // 创建CMSampleBuffer
    CMSampleBufferRef sample_buffer = nullptr;
    CMSampleTimingInfo timing = {};
    timing.duration = kCMTimeInvalid;
    timing.presentationTimeStamp = CMTimeMake(frame->ReceivedTimestamp(), 1000);  // 假设 Pts 是毫秒
    timing.decodeTimeStamp = kCMTimeInvalid;
    status = CMSampleBufferCreateReady(nullptr, block_buffer, format_desc_, 1, 1, &timing, 0,
                                       nullptr, &sample_buffer);
    CFRelease(block_buffer);
    if (status != noErr) {
      LOG_ERROR("Failed to create sample buffer");
      return -1;
    }

    // 送入解码
    VTDecodeFrameFlags flags = kVTDecodeFrame_EnableAsynchronousDecompression;
    VTDecodeInfoFlags flag_out = 0;
    auto* cb_ptr = new std::function<void(const DecodedFrame*)>(on_decoded_cb);

    status = VTDecompressionSessionDecodeFrame(decompression_session_, sample_buffer, flags, cb_ptr,
                                               &flag_out);
    CFRelease(sample_buffer);
    return (status == noErr) ? 0 : -1;
  }

  std::string GetDecoderName() { return "VideoToolboxH264"; }

 private:
  bool CreateSession(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
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
    OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, 2, sets, sizes,
                                                                          4, &format_desc_);
    if (status != noErr) {
      LOG_ERROR("Failed to create format description from SPS/PPS");
      return false;
    }

    VTDecompressionOutputCallbackRecord callback = {};
    callback.decompressionOutputCallback = &DecodeCallback;
    callback.decompressionOutputRefCon = this;

    status = VTDecompressionSessionCreate(nullptr, format_desc_, nullptr, nullptr, &callback,
                                          &decompression_session_);
    return status == noErr;
  }

  static void DecodeCallback(void* decompressionOutputRefCon, void* sourceFrameRefCon,
                             OSStatus status, VTDecodeInfoFlags infoFlags,
                             CVImageBufferRef imageBuffer, CMTime pts, CMTime duration) {
    if (status != noErr || !imageBuffer) return;
    auto* cb_ptr = reinterpret_cast<std::function<void(const DecodedFrame*)>*>(sourceFrameRefCon);
    auto decoded_frame = ConvertToDecodedFrame(imageBuffer, pts);
    (*cb_ptr)(decoded_frame.get());
    delete cb_ptr;
  }

  std::shared_ptr<SystemClock> clock_;
  VTDecompressionSessionRef decompression_session_;
  CMVideoFormatDescriptionRef format_desc_;
  std::vector<uint8_t> last_sps_;
  std::vector<uint8_t> last_pps_;
};

VideoToolboxDecoder::VideoToolboxDecoder(std::shared_ptr<SystemClock> clock)
    : impl_(std::make_shared<Impl>(std::move(clock))) {}

VideoToolboxDecoder::~VideoToolboxDecoder() = default;

int VideoToolboxDecoder::Init() { return impl_->Init(); }

int VideoToolboxDecoder::Decode(std::unique_ptr<ReceivedFrame> received_frame,
                                std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
  return impl_->Decode(std::move(received_frame), std::move(on_receive_decoded_frame));
}

std::string VideoToolboxDecoder::GetDecoderName() { return impl_->GetDecoderName(); }