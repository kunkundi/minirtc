#include "harmony_video_decoder.h"
#include "dav1d/dav1d_av1_decoder.h"
#include "openh264/openh264_decoder.h"
#include <dav1d/dav1d.h>
#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avformat.h>
#include <libyuv.h>
#include <algorithm>
#include <cstring>

namespace minirtc {
namespace {
constexpr const char* kLogTag = "CrossDeskDecoder";
const char* Mime(VideoCodecType type) { return type == VideoCodecType::AV1 ? "video/av01" : "video/avc"; }
bool HardwareFailure(const char* stage, int error) {
  OH_LOG_Print(LOG_APP, LOG_WARN, 0, kLogTag, "Hardware decoder failure: stage=%{public}s code=%{public}d", stage, error);
  return false;
}
bool Size(int w, int h) { return w > 0 && h > 0 && w <= 8192 && h <= 8192 && !(w & 1) && !(h & 1); }
int Field(OH_AVFormat* format, const char* name, int fallback) {
  int32_t value = fallback; OH_AVFormat_GetIntValue(format, name, &value); return value;
}
}
HarmonyVideoDecoder::HarmonyVideoDecoder(std::shared_ptr<SystemClock> clock, VideoCodecType type)
    : clock_(std::move(clock)), type_(type) {}
HarmonyVideoDecoder::~HarmonyVideoDecoder() {
  StopHardware();
  OH_LOG_Print(LOG_APP, LOG_INFO, 0, kLogTag,
    "Decode summary: mime=%{public}s hardware_frames=%{public}llu software_frames=%{public}llu",
    Mime(type_), static_cast<unsigned long long>(hardware_frames_), static_cast<unsigned long long>(software_frames_));
}
bool HarmonyVideoDecoder::Supported(VideoCodecType type) {
  if (type != VideoCodecType::H264 && type != VideoCodecType::AV1) return false;
  auto* capability = OH_AVCodec_GetCapabilityByCategory(Mime(type), false, HARDWARE);
  return capability && OH_AVCapability_IsHardware(capability);
}
int HarmonyVideoDecoder::Init() {
  std::lock_guard lock(mutex_);
  if (initialized_) return -1;
  const bool hardware = Supported(type_);
  OH_LOG_Print(LOG_APP, LOG_INFO, 0, kLogTag, "Decode request: mime=%{public}s hardware_available=%{public}d", Mime(type_), int(hardware));
  if (!hardware && Fallback("no hardware capability") != 0) return -1;
  initialized_ = true; return 0;
}
void HarmonyVideoDecoder::StopHardware() {
  stop_ = true;
  if (output_.joinable()) output_.join();
  if (codec_) { OH_VideoDecoder_Stop(codec_); OH_VideoDecoder_Destroy(codec_); codec_ = nullptr; }
  std::lock_guard lock(pending_mutex_); pending_.clear();
}
int HarmonyVideoDecoder::Fallback(const char* reason) {
  StopHardware();
  if (type_ == VideoCodecType::H264) software_ = std::make_unique<OpenH264Decoder>(clock_, false);
  else software_ = std::make_unique<Dav1dAv1Decoder>(clock_, false);
  if (software_->Init() != 0) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, 0, kLogTag, "Software decoder initialization failed: mime=%{public}s", Mime(type_));
    software_.reset(); return -1;
  }
  OH_LOG_Print(LOG_APP, LOG_WARN, 0, kLogTag,
    "Decoder selected: mode=software mime=%{public}s name=%{public}s reason=%{public}s",
    Mime(type_), software_->GetDecoderName().c_str(), reason);
  return 0;
}
int HarmonyVideoDecoder::DecodeSoftware(std::unique_ptr<ReceivedFrame> frame, std::function<void(const DecodedFrame*)> callback) {
  return software_->Decode(std::move(frame), [this, callback = std::move(callback)](const DecodedFrame* output) {
    if (output && ++software_frames_ == 1) {
      OH_LOG_Print(LOG_APP, LOG_INFO, 0, kLogTag,
        "First decoded frame: mode=software mime=%{public}s name=%{public}s size=%{public}ux%{public}u",
        Mime(type_), software_->GetDecoderName().c_str(), output->DecodedWidth(), output->DecodedHeight());
    }
    if (callback) callback(output);
  });
}
bool HarmonyVideoDecoder::Configure(int w, int h) {
  StopHardware();
  auto* capability = OH_AVCodec_GetCapabilityByCategory(Mime(type_), false, HARDWARE);
  if (!capability || !OH_AVCapability_IsHardware(capability)) return HardwareFailure("hardware capability unavailable", 0);
  if (!OH_AVCapability_IsVideoSizeSupported(capability, w, h)) return HardwareFailure("unsupported video size", 0);
  const char* name = OH_AVCapability_GetName(capability);
  if (!name) return HardwareFailure("missing component name", 0);
  name_ = name; codec_ = OH_VideoDecoder_CreateByName(name);
  if (!codec_) return HardwareFailure("create", 0);
  auto* format = OH_AVFormat_Create();
  if (!format) return HardwareFailure("create format", 0);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, w); OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, h);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_ENABLE_SYNC_MODE, 1);
  OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);
  const auto configured = OH_VideoDecoder_Configure(codec_, format);
  OH_AVFormat_Destroy(format);
  if (configured != AV_ERR_OK) return HardwareFailure("configure", configured);
  const auto prepared = OH_VideoDecoder_Prepare(codec_);
  if (prepared != AV_ERR_OK) return HardwareFailure("prepare", prepared);
  const auto started = OH_VideoDecoder_Start(codec_);
  if (started != AV_ERR_OK) return HardwareFailure("start", started);
  OH_LOG_Print(LOG_APP, LOG_INFO, 0, kLogTag,
    "Decoder selected: mode=hardware mime=%{public}s name=%{public}s size=%{public}dx%{public}d",
    Mime(type_), name_.c_str(), w, h);
  configured_width_ = w; configured_height_ = h; width_ = w; height_ = h;
  failed_ = false; stop_ = false; output_ = std::thread([this] { Drain(); }); return true;
}
int HarmonyVideoDecoder::Decode(std::unique_ptr<ReceivedFrame> frame, std::function<void(const DecodedFrame*)> callback) {
  std::lock_guard lock(mutex_);
  if (!initialized_ || !frame || !frame->Buffer() || !frame->Size() || frame->Size() > 8*1024*1024) return -1;
  if (software_) return DecodeSoftware(std::move(frame), std::move(callback));
  int w = configured_width_, h = configured_height_;
  if (type_ == VideoCodecType::H264) { parser_.ParseBitstream(frame->Buffer(), frame->Size()); parser_.GetResolution(&w, &h); }
  else { Dav1dSequenceHeader header{}; if (dav1d_parse_sequence_header(&header, frame->Buffer(), frame->Size()) == 0) { w = header.max_width; h = header.max_height; } }
  if (!Size(w, h)) return -1; // Request an initial key frame rather than guessing dimensions.
  const bool configure = !codec_ || w != configured_width_ || h != configured_height_;
  if (configure && !Configure(w, h)) {
    if (Fallback("hardware configuration failed") != 0) return -1;
    return DecodeSoftware(std::move(frame), std::move(callback));
  }
  {
    std::lock_guard lock(pending_mutex_);
    if (pending_.size() >= 64 || (!pending_.empty() && clock_->CurrentTimeUs() - pending_.begin()->second.submitted > 2'000'000)) {
      HardwareFailure("output backlog or timeout", 0); failed_ = true;
    }
  }
  uint32_t index = 0;
  if (!failed_ && OH_VideoDecoder_QueryInputBuffer(codec_, &index, 10000) == AV_ERR_OK) {
    auto* buffer = OH_VideoDecoder_GetInputBuffer(codec_, index);
    auto* bytes = buffer ? OH_AVBuffer_GetAddr(buffer) : nullptr;
    if (bytes && OH_AVBuffer_GetCapacity(buffer) >= static_cast<int64_t>(frame->Size())) {
      std::memcpy(bytes, frame->Buffer(), frame->Size());
      const auto pts = next_pts_ = std::max(next_pts_ + 1, clock_->CurrentTimeUs());
      OH_AVCodecBufferAttr attr{}; attr.pts = pts; attr.size = static_cast<int32_t>(frame->Size());
      // Register metadata before handing the buffer to an asynchronous output thread.
      { std::lock_guard lock(pending_mutex_); pending_.emplace(pts, Pending{frame->CapturedTimestamp(), frame->ReceivedTimestamp(), clock_->CurrentTimeUs(), callback}); }
      if (OH_AVBuffer_SetBufferAttr(buffer, &attr) == AV_ERR_OK && OH_VideoDecoder_PushInputBuffer(codec_, index) == AV_ERR_OK) return 0;
    }
  }
  if (Fallback(failed_ ? "hardware output failed or timed out" : "hardware input submission failed") != 0) return -1;
  DecodeSoftware(std::move(frame), std::move(callback));
  return -1; // Abandoned reference frames require a fresh key frame in the same codec.
}
void HarmonyVideoDecoder::Drain() {
  while (!stop_ && !failed_) {
    uint32_t index = 0;
    const auto status = OH_VideoDecoder_QueryOutputBuffer(codec_, &index, 10000);
    if (status == AV_ERR_TRY_AGAIN_LATER || status == AV_ERR_STREAM_CHANGED) continue;
    if (status != AV_ERR_OK) { HardwareFailure("query output", status); failed_ = true; break; }
    const bool ok = Output(index);
    const auto released = OH_VideoDecoder_FreeOutputBuffer(codec_, index);
    if (released != AV_ERR_OK || !ok) {
      HardwareFailure(ok ? "release output" : "invalid output buffer or format", released); failed_ = true;
    }
  }
}
bool HarmonyVideoDecoder::Output(uint32_t index) {
  auto* buffer = OH_VideoDecoder_GetOutputBuffer(codec_, index);
  OH_AVCodecBufferAttr attr{};
  if (!buffer || OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK) return false;
  if (!attr.size) return true;
  Pending pending;
  { std::lock_guard lock(pending_mutex_);
    auto it = pending_.find(attr.pts); if (it == pending_.end()) return true;
    pending = std::move(it->second); pending_.erase(it);
  }
  auto* format = OH_VideoDecoder_GetOutputDescription(codec_); if (!format) return false;
  const int w = Field(format, OH_MD_KEY_VIDEO_PIC_WIDTH, configured_width_);
  const int h = Field(format, OH_MD_KEY_VIDEO_PIC_HEIGHT, configured_height_);
  const int stride = Field(format, OH_MD_KEY_VIDEO_STRIDE, 0), slice = Field(format, OH_MD_KEY_VIDEO_SLICE_HEIGHT, 0);
  const int color = Field(format, OH_MD_KEY_PIXEL_FORMAT, -1);
  const int left = Field(format, OH_MD_KEY_VIDEO_CROP_LEFT, 0), top = Field(format, OH_MD_KEY_VIDEO_CROP_TOP, 0);
  OH_AVFormat_Destroy(format);
  const int capacity = OH_AVBuffer_GetCapacity(buffer); const uint8_t* base = OH_AVBuffer_GetAddr(buffer);
  if (!Size(w,h) || stride < w || slice < h || stride > 16384 || slice > 16384 || (stride & 1) || (slice & 1) ||
      left < 0 || top < 0 || (left & 1) || (top & 1) || int64_t(left)+w > stride || int64_t(top)+h > slice ||
      !base || attr.offset < 0 || attr.size < 0 || int64_t(attr.offset)+attr.size > capacity ||
      int64_t(stride)*slice*3/2 > attr.size) return false;
  base += attr.offset;
  DecodedFrame output(size_t(w)*h*3/2, w, h); auto* y = output.MutableBuffer(); auto* uv = y+size_t(w)*h;
  const auto* input_y = base + size_t(top)*stride + left;
  const auto* input_uv = base + size_t(stride)*slice + size_t(top/2)*stride + left;
  int result = -1;
  if (color == AV_PIXEL_FORMAT_NV12) result = libyuv::NV12Copy(input_y, stride, input_uv, stride, y, w, uv, w, w, h);
  else if (color == AV_PIXEL_FORMAT_NV21) result = libyuv::NV21ToNV12(input_y, stride, input_uv, stride, y, w, uv, w, w, h);
  else if (color == AV_PIXEL_FORMAT_YUVI420) {
    const auto* u = base + size_t(stride)*slice + size_t(top/2)*(stride/2) + left/2;
    const auto* v = u + size_t(stride/2)*(slice/2);
    result = libyuv::I420ToNV12(input_y, stride, u, stride/2, v, stride/2, y, w, uv, w, w, h);
  }
  if (result != 0) return false;
  output.SetDecodedWidth(w); output.SetDecodedHeight(h); output.SetCapturedTimestamp(pending.captured);
  output.SetReceivedTimestamp(pending.received); output.SetDecodedTimestamp(clock_->CurrentTimeUs());
  width_ = w; height_ = h;
  if (++hardware_frames_ == 1) {
    OH_LOG_Print(LOG_APP, LOG_INFO, 0, kLogTag,
      "First decoded frame: mode=hardware mime=%{public}s name=%{public}s size=%{public}dx%{public}d",
      Mime(type_), name_.c_str(), w, h);
  }
  if (!stop_ && pending.callback) pending.callback(&output);
  return true;
}
int HarmonyVideoDecoder::GetResolution(int* w, int* h) const {
  std::lock_guard lock(mutex_); if (!w || !h) return -1;
  if (software_) return software_->GetResolution(w,h);
  *w = width_; *h = height_; return *w && *h ? 0 : -1;
}
std::string HarmonyVideoDecoder::GetDecoderName() const {
  std::lock_guard lock(mutex_); return software_ ? software_->GetDecoderName() : name_;
}
}
