#include "android_media_codec.h"

#include <android/native_window.h>
#include <dav1d/dav1d.h>
#include <dlfcn.h>
#include <libyuv.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>

#include "android_codec_support.h"

namespace minirtc {
namespace {
using Format = std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)>;
Format NewFormat() { return {AMediaFormat_new(), AMediaFormat_delete}; }
int Field(AMediaFormat* f, const char* key, int fallback) {
  int32_t value = fallback;
  AMediaFormat_getInt32(f, key, &value);
  return value;
}
// MediaCodec normally emits Annex B. Normalize four-byte length prefixes too;
// never forward avcC/invalid lengths as an RTP H.264 access unit.
std::vector<uint8_t> AnnexB(const uint8_t* p, size_t n) {
  if (!p || n < 4) return {};
  if (p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p[3] == 1)))
    return {p, p + n};
  std::vector<uint8_t> result;
  for (size_t offset = 0; offset < n;) {
    if (n - offset < 4) return {};
    size_t size = (uint32_t(p[offset]) << 24) |
                  (uint32_t(p[offset + 1]) << 16) |
                  (uint32_t(p[offset + 2]) << 8) | p[offset + 3];
    offset += 4;
    if (!size || size > n - offset) return {};
    result.insert(result.end(), {0, 0, 0, 1});
    result.insert(result.end(), p + offset, p + offset + size);
    offset += size;
  }
  return result;
}
std::vector<uint8_t> Av1Config(const uint8_t* p, size_t n) {
  if (!p || !n) return {};
  // Android CSD may be AV1CodecConfigurationRecord, whose four-byte av1C
  // header is not part of the low-overhead OBU stream used by RTP.
  if (p[0] == 0x81) {
    if (n < 4) return {};
    p += 4;
    n -= 4;
  }
  Dav1dSequenceHeader header{};
  if (dav1d_parse_sequence_header(&header, p, n) != 0) return {};
  return {p, p + n};
}
struct Pending {
  int64_t captured = 0, received = 0, submitted = 0;
  std::function<int(const EncodedFrame&)> encoded;
  std::function<void(const DecodedFrame*)> decoded;
};
}  // namespace

struct AndroidCodecState {
  std::mutex mutex;
  AMediaCodec* codec = nullptr;
  bool started = false, failed = false;
  bool encoder = false, surface_output = false;
  VideoCodecType type = VideoCodecType::H264;
  std::string name;
  std::unique_ptr<AndroidSurface> placeholder;
  ANativeWindow* output_window = nullptr;
  int width = 0, height = 0, stride = 0, slice_height = 0, color = 0;
  int crop_left = 0, crop_top = 0, crop_right = 0, crop_bottom = 0;
  std::vector<uint8_t> csd;
  std::map<int64_t, Pending> pending;
  void Stop() {
    std::lock_guard<std::mutex> lock(mutex);
    if (codec) {
      if (started) AMediaCodec_stop(codec);
      AMediaCodec_delete(codec);
      codec = nullptr;
    }
    started = false;
    pending.clear();
    if (output_window) ANativeWindow_release(output_window);
    output_window = nullptr;
    placeholder.reset();
  }
  ~AndroidCodecState() { Stop(); }
};

namespace {
struct SurfaceBuffer {
  std::atomic<int> refs{1};
  std::shared_ptr<AndroidCodecState> state;
  size_t index;
  bool consumed = false;  // Protected by state->mutex.
  MiniRtcNativeVideoFrame descriptor{};
  SurfaceBuffer(std::shared_ptr<AndroidCodecState> s, size_t i, int w, int h)
      : state(std::move(s)), index(i) {
    descriptor.struct_size = sizeof(descriptor);
    descriptor.type = MiniRtcNativeVideoFrameAndroidMediaCodec;
    descriptor.width = w;
    descriptor.height = h;
    descriptor.owner = this;
    descriptor.retain = [](void* p) { ++static_cast<SurfaceBuffer*>(p)->refs; };
    descriptor.release = [](void* p) {
      auto* b = static_cast<SurfaceBuffer*>(p);
      if (--b->refs == 0) delete b;
    };
    descriptor.payload.android_media_codec.render = [](void* p, void* window) {
      return static_cast<SurfaceBuffer*>(p)->Render(
          static_cast<ANativeWindow*>(window));
    };
  }
  ~SurfaceBuffer() {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!consumed && state->codec && state->started &&
        AMediaCodec_releaseOutputBuffer(state->codec, index, false) !=
            AMEDIA_OK)
      state->failed = true;
  }
  int Render(ANativeWindow* window) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->codec || !state->started || state->failed) return -1;
    if (window != state->output_window) {
      auto* target = window ? window : state->placeholder->window;
      if (AMediaCodec_setOutputSurface(state->codec, target) != AMEDIA_OK) {
        state->failed = true;
        return -1;
      }
      if (window) ANativeWindow_acquire(window);
      if (state->output_window) ANativeWindow_release(state->output_window);
      state->output_window = window;
    }
    if (!window) return 0;  // Detach is also allowed after consuming the token.
    if (consumed) return -1;
    consumed = true;
    if (AMediaCodec_releaseOutputBuffer(state->codec, index, true) !=
        AMEDIA_OK) {
      state->failed = true;
      return -1;
    }
    return 0;
  }
};

void ReadOutputFormat(AndroidCodecState& s) {
  Format f(AMediaCodec_getOutputFormat(s.codec), AMediaFormat_delete);
  if (!f) {
    s.failed = true;
    return;
  }
  if (s.encoder) {
    if (s.type == VideoCodecType::H264) {
      std::vector<uint8_t> csd;
      for (const char* key : {"csd-0", "csd-1"}) {
        void* data = nullptr;
        size_t size = 0;
        if (AMediaFormat_getBuffer(f.get(), key, &data, &size)) {
          auto bytes = AnnexB(static_cast<uint8_t*>(data), size);
          csd.insert(csd.end(), bytes.begin(), bytes.end());
        }
      }
      if (!csd.empty()) s.csd = std::move(csd);
    } else {
      void* data = nullptr;
      size_t size = 0;
      if (AMediaFormat_getBuffer(f.get(), "csd-0", &data, &size))
        s.csd = Av1Config(static_cast<uint8_t*>(data), size);
    }
    return;
  }
  s.width = Field(f.get(), "width", s.width);
  s.height = Field(f.get(), "height", s.height);
  s.stride = Field(f.get(), "stride", s.width);
  s.slice_height = Field(f.get(), "slice-height", s.height);
  if (s.slice_height == 0) s.slice_height = s.height;
  s.color = Field(f.get(), "color-format", s.color);
  s.crop_left = Field(f.get(), "crop-left", 0);
  s.crop_top = Field(f.get(), "crop-top", 0);
  s.crop_right = Field(f.get(), "crop-right", s.crop_right);
  s.crop_bottom = Field(f.get(), "crop-bottom", s.crop_bottom);
  // API 26/27 have no NDK rectangle accessor. Keep the parsed SPS dimensions
  // there; resolve the API 28 accessor without raising the library's min API.
  using GetRect = bool (*)(AMediaFormat*, const char*, int32_t*, int32_t*,
                           int32_t*, int32_t*);
  static const auto get_rect =
      reinterpret_cast<GetRect>(dlsym(RTLD_DEFAULT, "AMediaFormat_getRect"));
  if (get_rect)
    get_rect(f.get(), "crop", &s.crop_left, &s.crop_top, &s.crop_right,
             &s.crop_bottom);
  LOG_INFO("MediaCodec [{}] output: {}", s.name,
           AMediaFormat_toString(f.get()));
}

// Returns a callback to run without any codec locks. A retained Surface token
// pins the codec state, but Stop() invalidates it before reusing the Surface.
std::function<void()> DrainOne(const std::shared_ptr<AndroidCodecState>& state,
                               const std::shared_ptr<SystemClock>& clock) {
  std::lock_guard<std::mutex> lock(state->mutex);
  auto& s = *state;
  if (!s.codec || !s.started || s.failed) return {};
  AMediaCodecBufferInfo info{};
  const ssize_t index = AMediaCodec_dequeueOutputBuffer(s.codec, &info, 0);
  if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
    ReadOutputFormat(s);
    return [] {};
  }
  if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) return [] {};
  if (index < 0) {
    if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) s.failed = true;
    // Bound outstanding metadata and detect a wedged vendor component.
    if (!s.pending.empty() &&
        (s.pending.size() > 120 ||
         clock->CurrentTimeUs() - s.pending.begin()->second.submitted >
             2000000))
      s.failed = true;
    return {};
  }
  auto discard = [&] {
    if (AMediaCodec_releaseOutputBuffer(s.codec, index, false) != AMEDIA_OK)
      s.failed = true;
  };
  size_t capacity = 0;
  uint8_t* buffer =
      s.surface_output ? nullptr
                       : AMediaCodec_getOutputBuffer(s.codec, index, &capacity);
  if (!s.surface_output &&
      (!buffer || info.offset < 0 || info.size < 0 ||
       size_t(info.offset) > capacity ||
       size_t(info.size) > capacity - size_t(info.offset))) {
    discard();
    s.failed = true;
    return {};
  }
  if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
    if (s.encoder && info.size) {
      auto* p = buffer + info.offset;
      s.csd = s.type == VideoCodecType::H264 ? AnnexB(p, info.size)
                                             : Av1Config(p, info.size);
    }
    discard();
    return [] {};
  }
  auto found = s.pending.find(info.presentationTimeUs);
  if (found == s.pending.end()) {
    discard();
    return [] {};
  }
  auto metadata = std::move(found->second);
  s.pending.erase(found);
  if (s.encoder) {
    auto* p = buffer + info.offset;
    auto bytes = s.type == VideoCodecType::H264
                     ? AnnexB(p, info.size)
                     : std::vector<uint8_t>(p, p + info.size);
    const bool key = (info.flags & 1 /* BUFFER_FLAG_KEY_FRAME, API 21 */) != 0;
    if (key && !s.csd.empty())
      bytes.insert(bytes.begin(), s.csd.begin(), s.csd.end());
    discard();
    if (bytes.empty()) {
      s.failed = true;
      return {};
    }
    if (key && s.type == VideoCodecType::H264) {
      H264BitstreamParser parser;
      parser.ParseBitstream(bytes.data(), bytes.size());
      int width = 0, height = 0;
      if (!parser.GetResolution(&width, &height) || width != s.width ||
          height != s.height) {
        s.failed = true;
        return {};
      }
    }
    if (key && s.type == VideoCodecType::AV1) {
      Dav1dSequenceHeader header{};
      if (dav1d_parse_sequence_header(&header, bytes.data(), bytes.size()) !=
          0) {
        s.failed = true;
        return {};
      }
    }
    EncodedFrame frame(bytes.data(), bytes.size(), s.width, s.height);
    frame.SetCapturedTimestamp(metadata.captured);
    frame.SetEncodedTimestamp(clock->CurrentTimeUs());
    frame.SetEncodedWidth(s.width);
    frame.SetEncodedHeight(s.height);
    frame.SetFrameType(key ? kVideoFrameKey : kVideoFrameDelta);
    return [metadata = std::move(metadata), frame = std::move(frame)] {
      if (metadata.encoded) metadata.encoded(frame);
    };
  }
  const int w = s.crop_right - s.crop_left + 1,
            h = s.crop_bottom - s.crop_top + 1;
  if (!AndroidMediaCodec::IsValidSize(w, h) || s.crop_left < 0 ||
      s.crop_top < 0 || (s.crop_left & 1) || (s.crop_top & 1) ||
      s.crop_right >= s.width || s.crop_bottom >= s.height) {
    discard();
    s.failed = true;
    return {};
  }
  DecodedFrame frame;
  frame.SetWidth(w);
  frame.SetHeight(h);
  frame.SetDecodedWidth(w);
  frame.SetDecodedHeight(h);
  frame.SetCapturedTimestamp(metadata.captured);
  frame.SetReceivedTimestamp(metadata.received);
  frame.SetDecodedTimestamp(clock->CurrentTimeUs());
  frame.SetSize(size_t(w) * h * 3 / 2);
  if (s.surface_output) {
    auto* token = new SurfaceBuffer(state, index, w, h);
    std::shared_ptr<SurfaceBuffer> owner(
        token, [](SurfaceBuffer* b) { b->descriptor.release(b); });
    return [metadata = std::move(metadata), frame,
            owner = std::move(owner)]() mutable {
      frame.SetNativeFrame(&owner->descriptor);
      if (metadata.decoded) metadata.decoded(&frame);
    };
  }
  if ((s.color != 19 && s.color != 21) || s.stride < s.width ||
      s.stride > 16384 || s.slice_height < s.height || s.slice_height > 16384 ||
      (s.stride & 1) || (s.slice_height & 1) ||
      size_t(s.stride) * s.slice_height * 3 / 2 > size_t(info.size)) {
    discard();
    s.failed = true;
    return {};
  }
  std::vector<uint8_t> nv12(frame.Size());
  auto* base = buffer + info.offset;
  auto* y = base + size_t(s.crop_top) * s.stride + s.crop_left;
  auto* chroma = base + size_t(s.stride) * s.slice_height;
  int status = 0;
  if (s.color == 21) {
    status = libyuv::NV12Copy(
        y, s.stride, chroma + size_t(s.crop_top / 2) * s.stride + s.crop_left,
        s.stride, nv12.data(), w, nv12.data() + size_t(w) * h, w, w, h);
  } else {
    auto* u =
        chroma + size_t(s.crop_top / 2) * (s.stride / 2) + s.crop_left / 2;
    auto* v = u + size_t(s.stride / 2) * (s.slice_height / 2);
    status = libyuv::I420ToNV12(y, s.stride, u, s.stride / 2, v, s.stride / 2,
                                nv12.data(), w, nv12.data() + size_t(w) * h, w,
                                w, h);
  }
  discard();
  if (status) {
    s.failed = true;
    return {};
  }
  frame.UpdateBuffer(nv12.data(), nv12.size());
  return [metadata = std::move(metadata), frame = std::move(frame)] {
    if (metadata.decoded) metadata.decoded(&frame);
  };
}
}  // namespace

AndroidMediaCodec::AndroidMediaCodec(std::shared_ptr<SystemClock> clock,
                                     VideoCodecType type, Mode mode,
                                     bool native_output)
    : clock_(std::move(clock)),
      type_(type),
      encoder_(mode == Mode::Encoder),
      native_output_(native_output) {}
AndroidMediaCodec::~AndroidMediaCodec() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (drain_thread_.joinable()) drain_thread_.join();
  StopCodec();
}
void AndroidMediaCodec::StopCodec() {
  if (state_) state_->Stop();
  state_.reset();
}
void AndroidMediaCodec::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  StopCodec();
}
bool AndroidMediaCodec::IsValidSize(int w, int h) {
  return w > 0 && h > 0 && w <= 8192 && h <= 8192 && !(w & 1) && !(h & 1) &&
         int64_t(w) * h <= 33554432;
}
int AndroidMediaCodec::InitDecoder() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (encoder_ || drain_thread_.joinable()) return -1;
  if (FindAndroidCodecs(type_, false).empty()) return -1;
  drain_thread_ = std::thread(&AndroidMediaCodec::DrainLoop, this);
  return 0;
}
int AndroidMediaCodec::InitEncoder(const MediaCodecConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!encoder_ || drain_thread_.joinable()) return -1;
  config_ = config;
  config_.init_bitrate =
      ClampEncoderTargetBitrate(config.init_bitrate, config.max_bitrate);
  if (!Configure(config.init_width, config.init_height)) return -1;
  drain_thread_ = std::thread(&AndroidMediaCodec::DrainLoop, this);
  return 0;
}
bool AndroidMediaCodec::Configure(int w, int h) {
  StopCodec();
  width_ = w;
  height_ = h;
  if (!AndroidMediaCodec::IsValidSize(w, h)) return false;
  for (const auto& info : FindAndroidCodecs(type_, encoder_, w, h)) {
    if (!encoder_ && !native_output_ && !info.color) continue;
    auto s = std::make_shared<AndroidCodecState>();
    s->encoder = encoder_;
    s->surface_output = !encoder_ && native_output_;
    s->type = type_;
    s->name = info.name;
    s->width = w;
    s->height = h;
    s->stride = w;
    s->slice_height = h;
    s->crop_right = w - 1;
    s->crop_bottom = h - 1;
    s->color = info.color;
    s->codec = AMediaCodec_createCodecByName(info.name.c_str());
    if (!s->codec) continue;
    auto f = NewFormat();
    AMediaFormat_setString(
        f.get(), "mime",
        type_ == VideoCodecType::H264 ? "video/avc" : "video/av01");
    AMediaFormat_setInt32(f.get(), "width", w);
    AMediaFormat_setInt32(f.get(), "height", h);
    AMediaFormat_setInt32(f.get(), "priority", 0);  // Real-time.
    if (encoder_) {
      AMediaFormat_setInt32(f.get(), "color-format", info.color);
      AMediaFormat_setInt32(f.get(), "bitrate", config_.init_bitrate);
      AMediaFormat_setInt32(f.get(), "frame-rate",
                            std::max(1, config_.max_frame_rate));
      AMediaFormat_setInt32(
          f.get(), "i-frame-interval",
          std::max(1, config_.key_frame_interval /
                          std::max(1, config_.max_frame_rate)));
      AMediaFormat_setInt32(f.get(), "max-bframes", 0);
      if (type_ == VideoCodecType::H264) {
        AMediaFormat_setInt32(f.get(), "profile", 1);  // Baseline, no B frames.
        AMediaFormat_setInt32(f.get(), "level", info.avc_level);
      }
      AMediaFormat_setInt32(f.get(), "stride", w);
      AMediaFormat_setInt32(f.get(), "slice-height", h);
    } else {
      AMediaFormat_setInt32(f.get(), "max-input-size", 8 * 1024 * 1024);
      if (info.low_latency) AMediaFormat_setInt32(f.get(), "low-latency", 1);
      if (s->surface_output) {
        s->placeholder = std::make_unique<AndroidSurface>();
        if (!s->placeholder->window) continue;
      } else
        AMediaFormat_setInt32(f.get(), "color-format", info.color);
    }
    auto* window = s->placeholder ? s->placeholder->window : nullptr;
    if (AMediaCodec_configure(
            s->codec, f.get(), window, nullptr,
            encoder_ ? AMEDIACODEC_CONFIGURE_FLAG_ENCODE : 0) != AMEDIA_OK ||
        AMediaCodec_start(s->codec) != AMEDIA_OK)
      continue;
    s->started = true;
    if (encoder_) {
      using GetInputFormat = AMediaFormat* (*)(AMediaCodec*);
      static const auto get_input_format = reinterpret_cast<GetInputFormat>(
          dlsym(RTLD_DEFAULT, "AMediaCodec_getInputFormat"));
      if (get_input_format) {
        Format input(get_input_format(s->codec), AMediaFormat_delete);
        if (input) {
          s->stride = Field(input.get(), "stride", w);
          s->slice_height = Field(input.get(), "slice-height", h);
          if (!s->slice_height) s->slice_height = h;
          s->color = Field(input.get(), "color-format", info.color);
        }
      }
      if ((s->color != 19 && s->color != 21) || s->stride < w ||
          s->slice_height < h || s->stride > 16384 || s->slice_height > 16384 ||
          (s->stride & 1) || (s->slice_height & 1))
        continue;
    }
    state_ = std::move(s);
    LOG_INFO(
        "Using MediaCodec [{}] {}x{}{}", info.name, w, h,
        native_output_ && !encoder_ ? " Surface output" : " ByteBuffer output");
    return true;
  }
  return false;
}

void AndroidMediaCodec::DrainLoop() {
  for (;;) {
    std::shared_ptr<AndroidCodecState> state;
    uint64_t sequence;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stopping_) return;
      state = state_;
      sequence = input_sequence_;
    }
    bool output = false;
    if (state) {
      for (int i = 0; i < 32; ++i) {
        auto callback = DrainOne(state, clock_);
        if (!callback) break;
        output = true;
        callback();
      }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_) return;
    // Input arrival wakes this immediately. Drain even without another input
    // so the last frame is delivered when a remote desktop becomes idle.
    bool pending = false;
    if (state_) {
      std::lock_guard<std::mutex> codec_lock(state_->mutex);
      pending = !state_->failed && !state_->pending.empty();
    }
    auto changed = [&] { return stopping_ || sequence != input_sequence_; };
    if (pending)
      wake_.wait_for(lock, std::chrono::milliseconds(output ? 1 : 3), changed);
    else
      wake_.wait(lock, changed);
  }
}

AndroidMediaCodec::Result AndroidMediaCodec::Encode(
    const RawFrame& frame, std::function<int(const EncodedFrame&)> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!encoder_ || !IsValidSize(frame.Width(), frame.Height()))
    return Result::InvalidInput;
  if ((!state_ || width_ != int(frame.Width()) ||
       height_ != int(frame.Height())) &&
      !Configure(frame.Width(), frame.Height())) {
    return Result::ConfigureFailed;
  }
  bool success = false;
  {
    std::lock_guard<std::mutex> codec_lock(state_->mutex);
    auto& s = *state_;
    const size_t size = size_t(width_) * height_ * 3 / 2;
    if (!frame.Buffer() || frame.Size() < size) return Result::InvalidInput;
    if (!s.failed) {
      const ssize_t index = AMediaCodec_dequeueInputBuffer(s.codec, 10000);
      if (index >= 0) {
        size_t capacity = 0;
        auto* input = AMediaCodec_getInputBuffer(s.codec, index, &capacity);
        const size_t padded = size_t(s.stride) * s.slice_height * 3 / 2;
        if (input && capacity >= padded) {
          const size_t luma = size_t(s.stride) * s.slice_height;
          std::memset(input, 16, luma);
          std::memset(input + luma, 128, padded - luma);
          if (s.color == 21)
            libyuv::NV12Copy(frame.Buffer(), width_,
                             frame.Buffer() + size_t(width_) * height_, width_,
                             input, s.stride, input + luma, s.stride, width_,
                             height_);
          else
            libyuv::NV12ToI420(frame.Buffer(), width_,
                               frame.Buffer() + size_t(width_) * height_,
                               width_, input, s.stride, input + luma,
                               s.stride / 2, input + luma * 5 / 4, s.stride / 2,
                               width_, height_);
          const int64_t pts = next_pts_ =
              std::max(next_pts_ + 1, clock_->CurrentTimeUs());
          success = AMediaCodec_queueInputBuffer(s.codec, index, 0, padded, pts,
                                                 0) == AMEDIA_OK;
          if (success)
            s.pending.emplace(pts, Pending{frame.CapturedTimestamp(),
                                           0,
                                           clock_->CurrentTimeUs(),
                                           callback,
                                           {}});
        }
      }
    }
  }
  if (success) {
    ++input_sequence_;
    wake_.notify_one();
    return Result::Ok;
  }
  return Result::CodecError;
}
AndroidMediaCodec::Result AndroidMediaCodec::Decode(
    const ReceivedFrame& frame,
    std::function<void(const DecodedFrame*)> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (encoder_ || !frame.Buffer() || !frame.Size() ||
      frame.Size() > 8 * 1024 * 1024)
    return Result::InvalidInput;
  int w = width_, h = height_;
  if (type_ == VideoCodecType::H264) {
    parser_.ParseBitstream(frame.Buffer(), frame.Size());
    parser_.GetResolution(&w, &h);
  } else {
    Dav1dSequenceHeader header{};
    if (dav1d_parse_sequence_header(&header, frame.Buffer(), frame.Size()) ==
        0) {
      w = header.max_width;
      h = header.max_height;
    }
  }
  if (!IsValidSize(w, h)) return Result::NeedKeyFrame;
  if ((!state_ || width_ != w || height_ != h) && !Configure(w, h)) {
    return Result::ConfigureFailed;
  }
  bool success = false;
  {
    std::lock_guard<std::mutex> codec_lock(state_->mutex);
    auto& s = *state_;
    if (!s.failed) {
      const ssize_t index = AMediaCodec_dequeueInputBuffer(s.codec, 10000);
      if (index >= 0) {
        size_t capacity = 0;
        auto* input = AMediaCodec_getInputBuffer(s.codec, index, &capacity);
        if (input && capacity >= frame.Size()) {
          std::memcpy(input, frame.Buffer(), frame.Size());
          const int64_t pts = next_pts_ =
              std::max(next_pts_ + 1, clock_->CurrentTimeUs());
          success = AMediaCodec_queueInputBuffer(
                        s.codec, index, 0, frame.Size(), pts, 0) == AMEDIA_OK;
          if (success)
            s.pending.emplace(pts, Pending{frame.CapturedTimestamp(),
                                           frame.ReceivedTimestamp(),
                                           clock_->CurrentTimeUs(),
                                           {},
                                           callback});
        }
      }
    }
  }
  if (success) {
    ++input_sequence_;
    wake_.notify_one();
    return Result::Ok;
  }
  return Result::CodecError;
}
int AndroidMediaCodec::ForceIdr() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!encoder_ || !state_) return -1;
  std::lock_guard<std::mutex> codec_lock(state_->mutex);
  auto f = NewFormat();
  AMediaFormat_setInt32(f.get(), "request-sync", 0);
  return AMediaCodec_setParameters(state_->codec, f.get()) == AMEDIA_OK ? 0
                                                                        : -1;
}
int AndroidMediaCodec::SetTargetBitrate(int bitrate) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.init_bitrate =
      ClampEncoderTargetBitrate(bitrate, config_.max_bitrate);
  if (!encoder_ || !state_) return -1;
  std::lock_guard<std::mutex> codec_lock(state_->mutex);
  auto f = NewFormat();
  AMediaFormat_setInt32(f.get(), "video-bitrate", config_.init_bitrate);
  return AMediaCodec_setParameters(state_->codec, f.get()) == AMEDIA_OK ? 0
                                                                        : -1;
}
int AndroidMediaCodec::GetResolution(int* w, int* h) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!w || !h) return -1;
  *w = width_;
  *h = height_;
  return 0;
}
std::string AndroidMediaCodec::GetName() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ ? "MediaCodec/" + state_->name : "MediaCodec";
}
}  // namespace minirtc
