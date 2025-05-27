#include "video_toolbox_decoder.h"
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// 提取第一个SPS和PPS
static bool ExtractSPSPPS(const std::vector<uint8_t>& data, std::vector<uint8_t>& sps,
                          std::vector<uint8_t>& pps) {
  size_t pos = 0;
  while (pos + 4 < data.size()) {
    size_t start = pos;
    while (start + 3 < data.size() &&
           !(data[start] == 0 && data[start + 1] == 0 &&
             ((data[start + 2] == 1) || (data[start + 2] == 0 && data[start + 3] == 1))))
      ++start;
    if (start + 3 >= data.size()) break;
    size_t start_code_len = (data[start + 2] == 1) ? 3 : 4;
    size_t nalu_start = start + start_code_len;
    size_t nalu_end = nalu_start;
    while (nalu_end + 3 < data.size() &&
           !(data[nalu_end] == 0 && data[nalu_end + 1] == 0 &&
             ((data[nalu_end + 2] == 1) || (data[nalu_end + 2] == 0 && data[nalu_end + 3] == 1))))
      ++nalu_end;
    if (nalu_start >= data.size() || nalu_end > data.size() || nalu_start >= nalu_end) break;
    uint8_t type = data[nalu_start] & 0x1F;
    if (type == 7 && sps.empty()) sps.assign(data.begin() + nalu_start, data.begin() + nalu_end);
    if (type == 8 && pps.empty()) pps.assign(data.begin() + nalu_start, data.begin() + nalu_end);
    pos = nalu_end;
    if (!sps.empty() && !pps.empty()) break;
  }
  return !sps.empty() && !pps.empty();
}

// 简单的转换函数
static std::unique_ptr<DecodedFrame> ConvertToDecodedFrame(CVImageBufferRef imageBuffer,
                                                           CMTime pts) {
  return std::unique_ptr<DecodedFrame>(new DecodedFrame());
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
    if (!frame) return -1;

    std::vector<uint8_t> sps, pps;
    std::vector<uint8_t> data(frame->Buffer(), frame->Buffer() + frame->Size());
    if (!ExtractSPSPPS(data, sps, pps)) {
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

    if (!decompression_session_) return -1;

    // 转换为AVCC格式
    std::vector<uint8_t> avcc_data;
    size_t pos = 0;
    while (pos + 4 < data.size()) {
      size_t start = pos;
      while (start + 3 < data.size() &&
             !(data[start] == 0 && data[start + 1] == 0 &&
               ((data[start + 2] == 1) || (data[start + 2] == 0 && data[start + 3] == 1))))
        ++start;
      if (start + 3 >= data.size()) break;
      size_t start_code_len = (data[start + 2] == 1) ? 3 : 4;
      size_t nalu_start = start + start_code_len;
      size_t nalu_end = nalu_start;
      while (nalu_end + 3 < data.size() &&
             !(data[nalu_end] == 0 && data[nalu_end + 1] == 0 &&
               ((data[nalu_end + 2] == 1) || (data[nalu_end + 2] == 0 && data[nalu_end + 3] == 1))))
        ++nalu_end;
      if (nalu_start >= data.size() || nalu_end > data.size() || nalu_start >= nalu_end) break;
      uint32_t len = htonl(static_cast<uint32_t>(nalu_end - nalu_start));
      avcc_data.insert(avcc_data.end(), reinterpret_cast<uint8_t*>(&len),
                       reinterpret_cast<uint8_t*>(&len) + 4);
      avcc_data.insert(avcc_data.end(), data.begin() + nalu_start, data.begin() + nalu_end);
      pos = nalu_end;
    }

    // 创建CMBlockBuffer
    CMBlockBufferRef block_buffer = nullptr;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        nullptr, avcc_data.data(), avcc_data.size(), kCFAllocatorNull, nullptr, 0, avcc_data.size(),
        0, &block_buffer);
    if (status != kCMBlockBufferNoErr) return -1;

    // 创建CMSampleBuffer
    CMSampleBufferRef sample_buffer = nullptr;
    CMSampleTimingInfo timing = {};
    status = CMSampleBufferCreateReady(nullptr, block_buffer, format_desc_, 1, 1, &timing, 0,
                                       nullptr, &sample_buffer);
    CFRelease(block_buffer);
    if (status != noErr) return -1;

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
    if (status != noErr) return false;

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