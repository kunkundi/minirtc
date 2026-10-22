#include "wmf_h264_encoder.h"

#include <codecapi.h>
#include <strmif.h>

#include <chrono>
#include <limits>

#include "h264_bitstream_parser.h"
#include "openh264/openh264_encoder.h"
#include "wmf/wmf_utils.h"

namespace minirtc {
namespace {
using wmf::ComPtr;

HRESULT SetUint(ICodecAPI* api, const GUID& key, ULONG value) {
  VARIANT variant{};
  variant.vt = VT_UI4;
  variant.ulVal = value;
  return api->SetValue(&key, &variant);
}

HRESULT EnumerateEncoders(IMFActivate*** activates, UINT32* count) {
  MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
  return MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                   MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input,
                   &output, activates, count);
}

bool ValidConfig(const MediaCodecConfig& config) {
  size_t size = 0;
  return config.init_width > 0 && config.init_height > 0 &&
         config.init_width <= 8192 && config.init_height <= 8192 &&
         GetNv12FrameSize(config.init_width, config.init_height, &size) &&
         config.init_bitrate > 0 && config.max_bitrate > 0 &&
         config.max_frame_rate > 0 && config.max_frame_rate <= 240 &&
         config.key_frame_interval > 0;
}
}  // namespace

class WmfH264Encoder::Impl {
 public:
  explicit Impl(std::shared_ptr<SystemClock> clock)
      : clock_(std::move(clock)) {}
  ~Impl() {
    worker_.Call([this] {
      Reset();
      fallback_.reset();
    });
  }

  void Reset() {
    events_.Reset();
    api_.Reset();
    wmf::ShutdownTransform(encoder_);
    input_credit_ = 0;
    sps_.clear();
    pps_.clear();
    parser_.Reset();
    sequence_ = 0;
    force_idr_ = true;
  }

  HRESULT Configure(IMFActivate* activation) {
    HRESULT hr = activation->ActivateObject(IID_PPV_ARGS(&encoder_));
    if (FAILED(hr)) return hr;
    ComPtr<IMFAttributes> attributes;
    hr = encoder_->GetAttributes(&attributes);
    if (FAILED(hr)) return hr;
    async_ =
        MFGetAttributeUINT32(attributes.Get(), MF_TRANSFORM_ASYNC, FALSE) != 0;
    if (async_) {
      hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      if (FAILED(hr)) return hr;
      hr = encoder_.As(&events_);
      if (FAILED(hr)) return hr;
    }
    DWORD inputs = 0, outputs = 0;
    hr = encoder_->GetStreamCount(&inputs, &outputs);
    if (FAILED(hr) || inputs != 1 || outputs != 1) return E_UNEXPECTED;
    input_id_ = output_id_ = 0;
    hr = encoder_->GetStreamIDs(1, &input_id_, 1, &output_id_);
    if (FAILED(hr) && hr != E_NOTIMPL) return hr;
    hr = encoder_.As(&api_);
    if (FAILED(hr)) return hr;
    hr = SetUint(api_.Get(), CODECAPI_AVEncCommonRateControlMode,
                 eAVEncCommonRateControlMode_CBR);
    if (FAILED(hr)) return hr;
    VARIANT low_latency{};
    low_latency.vt = VT_BOOL;
    low_latency.boolVal = VARIANT_TRUE;
    api_->SetValue(&CODECAPI_AVLowLatencyMode, &low_latency);
    SetUint(api_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    SetUint(api_.Get(), CODECAPI_AVEncMPVGOPSize, config_.key_frame_interval);
    SetUint(api_.Get(), CODECAPI_AVEncCommonQualityVsSpeed,
            config_.video_degradation_preference ==
                    VideoDegradationPreference::MaintainFrameRate
                ? 0
                : 50);

    ComPtr<IMFMediaType> output, input;
    hr = MFCreateMediaType(&output);
    if (FAILED(hr)) return hr;
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, config_.init_bitrate);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // Baseline forbids B pictures even on drivers that ignore the optional
    // B-picture property. The desktop pipeline requires one-frame latency.
    output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, config_.init_width,
                       config_.init_height);
    MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, config_.max_frame_rate,
                        1);
    MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = encoder_->SetOutputType(output_id_, output.Get(), 0);
    if (FAILED(hr)) return hr;
    hr = MFCreateMediaType(&input);
    if (FAILED(hr)) return hr;
    output->CopyAllItems(input.Get());
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input->DeleteItem(MF_MT_MPEG2_PROFILE);
    input->DeleteItem(MF_MT_AVG_BITRATE);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, config_.init_width);
    hr = encoder_->SetInputType(input_id_, input.Get(), 0);
    if (FAILED(hr)) return hr;
    hr = SetUint(api_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                 config_.init_bitrate);
    if (FAILED(hr)) return hr;
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return hr;
    return encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  }

  HRESULT Initialize() {
    Reset();
    if (FAILED(worker_.Status())) return worker_.Status();
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    HRESULT hr = EnumerateEncoders(&activations, &count);
    if (FAILED(hr)) return hr;
    hr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; ++i) {
      if (!encoder_) {
        hr = Configure(activations[i]);
        if (FAILED(hr)) {
          LOG_DEBUG("Hardware H.264 MFT initialization failed: 0x{:08x}",
                    static_cast<uint32_t>(hr));
          Reset();
          activations[i]->ShutdownObject();
        } else {
          WCHAR name[256]{};
          activations[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256,
                                    nullptr);
          char utf8[1024]{};
          WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr,
                              nullptr);
          LOG_INFO("Media Foundation hardware H.264 encoder: {}", utf8);
          // The transform's lifetime is now owned by this codec.
          activations[i]->DetachObject();
        }
      }
      activations[i]->Release();
    }
    CoTaskMemFree(activations);
    reconfigure_ = false;
    return encoder_ ? S_OK : hr;
  }

  void CacheHeaders(const uint8_t* data, size_t size) {
    for (const auto& nalu : wmf::AnnexBNalus(data, size)) {
      if (nalu.type != 7 && nalu.type != 8) continue;
      auto& header = nalu.type == 7 ? sps_ : pps_;
      header = {0, 0, 0, 1};
      header.insert(header.end(), data + nalu.offset,
                    data + nalu.offset + nalu.size);
    }
  }

  HRESULT ReadFrame(IMFSample* sample, const RawFrame& raw,
                    std::vector<EncodedFrame>& frames) {
    std::vector<uint8_t> bytes;
    HRESULT hr = wmf::ReadBytes(sample, bytes);
    if (FAILED(hr)) return hr;
    if (wmf::AnnexBNalus(bytes.data(), bytes.size()).empty())
      return MF_E_INVALID_STREAM_DATA;
    const bool key = wmf::HasNalu(bytes.data(), bytes.size(), 5);
    CacheHeaders(bytes.data(), bytes.size());
    if (key && (sps_.empty() || pps_.empty())) {
      ComPtr<IMFMediaType> type;
      UINT32 size = 0;
      if (SUCCEEDED(encoder_->GetOutputCurrentType(output_id_, &type)) &&
          SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size))) {
        std::vector<uint8_t> header(size);
        if (SUCCEEDED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(),
                                    size, nullptr)))
          CacheHeaders(header.data(), header.size());
      }
    }
    if (force_idr_ && !key) return MF_E_INVALID_STREAM_DATA;
    if (key) {
      if (sps_.empty() || pps_.empty()) return MF_E_INVALID_STREAM_DATA;
      std::vector<uint8_t> headers;
      if (!wmf::HasNalu(bytes.data(), bytes.size(), 7) ||
          !wmf::HasNalu(bytes.data(), bytes.size(), 8)) {
        // Always prepend a complete ordered pair, including when the MFT
        // emitted only one of the two parameter sets in this sample.
        headers.insert(headers.end(), sps_.begin(), sps_.end());
        headers.insert(headers.end(), pps_.begin(), pps_.end());
      }
      bytes.insert(bytes.begin(), headers.begin(), headers.end());
    }
    EncodedFrame frame(bytes.data(), bytes.size(), raw.Width(), raw.Height());
    frame.SetEncodedWidth(raw.Width());
    frame.SetEncodedHeight(raw.Height());
    frame.SetFrameType(key ? kVideoFrameKey : kVideoFrameDelta);
    frame.SetCapturedTimestamp(raw.CapturedTimestamp());
    frame.SetEncodedTimestamp(clock_->CurrentTime());
    parser_.ParseBitstream(bytes.data(), bytes.size());
    if (const auto qp = parser_.GetLastSliceQp())
      frame.SetQp(*qp, 0, 51, false);
    frames.push_back(std::move(frame));
    force_idr_ = false;
    return S_OK;
  }

  HRESULT EncodeHardware(const RawFrame& raw,
                         std::vector<EncodedFrame>& frames) {
    if (raw.Width() != static_cast<uint32_t>(config_.init_width) ||
        raw.Height() != static_cast<uint32_t>(config_.init_height) ||
        reconfigure_) {
      config_.init_width = static_cast<int>(raw.Width());
      config_.init_height = static_cast<int>(raw.Height());
      HRESULT hr = Initialize();
      if (FAILED(hr)) return hr;
    }
    if (!encoder_) return MF_E_NOT_INITIALIZED;
    if (sequence_ % config_.key_frame_interval == 0) force_idr_ = true;
    if (force_idr_) {
      HRESULT hr = SetUint(api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
      if (FAILED(hr)) return hr;
    }
    ComPtr<IMFSample> input;
    const LONGLONG duration = 10'000'000LL / config_.max_frame_rate;
    size_t input_size = 0;
    GetNv12FrameSize(raw.Width(), raw.Height(), &input_size);
    HRESULT hr = wmf::CreateSample(raw.Buffer(), input_size, next_time_,
                                   duration, input);
    if (FAILED(hr)) return hr;
    next_time_ += duration;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool submitted = false;
    // Low-latency, no-B-frame mode must produce this frame before accepting the
    // next one. A bounded wait also handles driver/device loss without hanging
    // the media thread. No callbacks or input references escape Encode().
    while (std::chrono::steady_clock::now() < deadline) {
      bool have_output = !async_;
      bool received_event = false;
      if (async_) {
        ComPtr<IMFMediaEvent> event;
        hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr != MF_E_NO_EVENTS_AVAILABLE) {
          if (FAILED(hr)) return hr;
          received_event = true;
          HRESULT status = S_OK;
          hr = event->GetStatus(&status);
          if (FAILED(hr)) return hr;
          if (FAILED(status)) return status;
          MediaEventType type = MEUnknown;
          hr = event->GetType(&type);
          if (FAILED(hr)) return hr;
          if (type == METransformNeedInput) ++input_credit_;
          if (type == METransformHaveOutput) have_output = true;
        }
      }
      if (!submitted && (!async_ || input_credit_)) {
        hr = encoder_->ProcessInput(input_id_, input.Get(), 0);
        if (SUCCEEDED(hr)) {
          submitted = true;
          if (async_) --input_credit_;
        } else if (hr != MF_E_NOTACCEPTING)
          return hr;
      }
      if (have_output) {
        ComPtr<IMFSample> output;
        hr = wmf::ReadOutput(encoder_.Get(), output_id_, output);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
          ComPtr<IMFMediaType> type;
          hr = encoder_->GetOutputAvailableType(output_id_, 0, &type);
          if (FAILED(hr)) return hr;
          hr = encoder_->SetOutputType(output_id_, type.Get(), 0);
          if (FAILED(hr)) return hr;
          // An asynchronous MFT consumes the HaveOutput notification even
          // when it reports a format change. Wait for the next notification;
          // another ProcessOutput here fails with E_UNEXPECTED on Intel MFTs.
          if (async_) continue;
          hr = wmf::ReadOutput(encoder_.Get(), output_id_, output);
        }
        if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) return hr;
        if (SUCCEEDED(hr) && output) {
          hr = ReadFrame(output.Get(), raw, frames);
          if (FAILED(hr)) return hr;
          ++sequence_;
          return S_OK;
        }
      }
      // Drain queued notifications without adding a timer delay per event.
      // Only poll with a delay when idle; keep the bounded wait above.
      if (!received_event)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
  }

  wmf::CodecThread worker_;
  std::shared_ptr<SystemClock> clock_;
  MediaCodecConfig config_;
  ComPtr<IMFTransform> encoder_;
  ComPtr<IMFMediaEventGenerator> events_;
  ComPtr<ICodecAPI> api_;
  DWORD input_id_ = 0, output_id_ = 0;
  unsigned input_credit_ = 0;
  bool async_ = false, force_idr_ = true, reconfigure_ = false;
  bool initialized_ = false;
  uint64_t sequence_ = 0;
  LONGLONG next_time_ = 0;
  std::vector<uint8_t> sps_, pps_;
  H264BitstreamParser parser_;
  std::unique_ptr<OpenH264Encoder> fallback_;
};

WmfH264Encoder::WmfH264Encoder(std::shared_ptr<SystemClock> clock)
    : impl_(std::make_unique<Impl>(std::move(clock))) {}
WmfH264Encoder::~WmfH264Encoder() = default;

bool WmfH264Encoder::IsSupported() {
  wmf::CodecThread worker;
  if (FAILED(worker.Status())) return false;
  return worker.Call([] {
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT hr = EnumerateEncoders(&activations, &count);
    for (UINT32 i = 0; i < count; ++i) activations[i]->Release();
    CoTaskMemFree(activations);
    return SUCCEEDED(hr) && count > 0;
  });
}

int WmfH264Encoder::Init(const MediaCodecConfig& config) {
  if (!ValidConfig(config)) return -1;
  return impl_->worker_.Call([&] {
    impl_->fallback_.reset();
    impl_->config_ = config;
    impl_->config_.init_bitrate =
        ClampEncoderTargetBitrate(config.init_bitrate, config.max_bitrate);
    impl_->initialized_ = SUCCEEDED(impl_->Initialize());
    return impl_->initialized_ ? 0 : -1;
  });
}

int WmfH264Encoder::Encode(const RawFrame& raw,
                           std::function<int(const EncodedFrame&)> callback) {
  size_t size = 0;
  if (!raw.Buffer() || raw.Width() > 8192 || raw.Height() > 8192 ||
      !GetNv12FrameSize(raw.Width(), raw.Height(), &size) || raw.Size() < size)
    return -1;
  std::vector<EncodedFrame> frames;
  const int result = impl_->worker_.Call([&] {
    if (!impl_->initialized_) return -1;
    if (!impl_->fallback_) {
      const HRESULT hr = impl_->EncodeHardware(raw, frames);
      if (SUCCEEDED(hr)) return 0;
      LOG_WARN(
          "Media Foundation hardware encoding failed (0x{:08x}); using "
          "OpenH264",
          static_cast<uint32_t>(hr));
      impl_->Reset();
      impl_->fallback_ = std::make_unique<OpenH264Encoder>(impl_->clock_);
      if (impl_->fallback_->Init(impl_->config_) != 0) {
        impl_->fallback_.reset();
        return -1;
      }
    }
    return impl_->fallback_->Encode(raw, [&](const EncodedFrame& frame) {
      frames.push_back(frame);
      return 0;
    });
  });
  for (const auto& frame : frames)
    if (callback) callback(frame);
  return result;
}

int WmfH264Encoder::ForceIdr() {
  return impl_->worker_.Call([&] {
    impl_->force_idr_ = true;
    return impl_->fallback_ ? impl_->fallback_->ForceIdr() : 0;
  });
}

int WmfH264Encoder::SetTargetBitrate(int bitrate) {
  if (bitrate <= 0) return -1;
  return impl_->worker_.Call([&] {
    bitrate = ClampEncoderTargetBitrate(bitrate, impl_->config_.max_bitrate);
    if (impl_->fallback_) {
      const int result = impl_->fallback_->SetTargetBitrate(bitrate);
      if (result == 0) impl_->config_.init_bitrate = bitrate;
      return result;
    }
    if (!impl_->api_) return -1;
    if (FAILED(SetUint(impl_->api_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                       bitrate)))
      impl_->reconfigure_ = true;
    impl_->config_.init_bitrate = bitrate;
    return 0;
  });
}

int WmfH264Encoder::SetTargetFrameRate(int frame_rate) {
  if (frame_rate <= 0 || frame_rate > 240) return -1;
  return impl_->worker_.Call([&] {
    if (impl_->fallback_) {
      const int result = impl_->fallback_->SetTargetFrameRate(frame_rate);
      if (result == 0) impl_->config_.max_frame_rate = frame_rate;
      return result;
    }
    if (!impl_->encoder_) return -1;
    if (impl_->config_.max_frame_rate != frame_rate) impl_->reconfigure_ = true;
    impl_->config_.max_frame_rate = frame_rate;
    return 0;
  });
}

int WmfH264Encoder::GetResolution(int* width, int* height) const {
  if (!width || !height) return -1;
  return impl_->worker_.Call([&] {
    if (impl_->fallback_) return impl_->fallback_->GetResolution(width, height);
    if (!impl_->encoder_) return -1;
    *width = impl_->config_.init_width;
    *height = impl_->config_.init_height;
    return 0;
  });
}

std::string WmfH264Encoder::GetEncoderName() const {
  return impl_->worker_.Call([&]() -> std::string {
    return impl_->fallback_ ? impl_->fallback_->GetEncoderName()
                            : "MediaFoundation(H264/HW)";
  });
}
}  // namespace minirtc
