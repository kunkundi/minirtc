#include "wmf_video_encoder.h"

#include <codecapi.h>
#include <strmif.h>
#include <wrl/implements.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>

#include "encoder_input_timing.h"
#include "h264_bitstream_parser.h"
#include "openh264/openh264_encoder.h"
#include "wmf_encoder_burst_control.h"
#include "wmf/wmf_utils.h"

namespace minirtc {
namespace {
using wmf::ComPtr;

// One reader per transform. Encode waits for one notification at a time;
// the MF callback never references the encoder or its current frame.
class MftEventReader final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFAsyncCallback> {
 public:
  explicit MftEventReader(IMFMediaEventGenerator* source) : source_(source) {}

  HRESULT Next(std::chrono::steady_clock::time_point deadline,
               ComPtr<IMFMediaEvent>& event) {
    HRESULT hr = source_->BeginGetEvent(this, nullptr);
    if (FAILED(hr)) return hr;
    std::unique_lock lock(mutex_);
    if (!ready_.wait_until(lock, deadline, [this] { return completed_; })) {
      // Reset will shut down this transform. Break the reference back to it
      // even if a stalled driver never delivers the pending notification.
      source_.Reset();
      return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    event = std::move(event_);
    completed_ = false;
    return status_;
  }

  STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }

  STDMETHODIMP Invoke(IMFAsyncResult* result) override {
    ComPtr<IMFMediaEventGenerator> source;
    {
      std::lock_guard lock(mutex_);
      source = source_;
    }
    if (!source) return S_OK;
    ComPtr<IMFMediaEvent> event;
    const HRESULT hr = source->EndGetEvent(result, &event);
    {
      std::lock_guard lock(mutex_);
      event_ = std::move(event);
      status_ = hr;
      completed_ = true;
    }
    ready_.notify_one();
    return S_OK;
  }

 private:
  ComPtr<IMFMediaEventGenerator> source_;
  ComPtr<IMFMediaEvent> event_;
  std::mutex mutex_;
  std::condition_variable ready_;
  bool completed_ = false;
  HRESULT status_ = S_OK;
};

HRESULT SetUint(ICodecAPI* api, const GUID& key, ULONG value) {
  VARIANT variant{};
  variant.vt = VT_UI4;
  variant.ulVal = value;
  return api->SetValue(&key, &variant);
}

HRESULT EnumerateEncoders(bool hevc, IMFActivate*** activates, UINT32* count) {
  MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output{MFMediaType_Video,
                                hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
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

class WmfVideoEncoder::Impl {
 public:
  Impl(std::shared_ptr<SystemClock> clock, VideoCodecType codec)
      : clock_(std::move(clock)), hevc_(codec == VideoCodecType::H265) {}
  ~Impl() {
    worker_.Call([this] {
      Reset();
      fallback_.reset();
    });
  }

  void Reset() {
    event_reader_.Reset();
    api_.Reset();
    wmf::ShutdownTransform(encoder_);
    input_credit_ = 0;
    for (auto& header : parameter_sets_) header.clear();
    parser_.Reset();
    sequence_ = 0;
    burst_control_.Reset(buffer_duration_ms_);
    last_buffer_log_ms_ = -1;
    last_buffer_observation_bitrate_ = 0;
    last_buffer_request_bitrate_ = 0;
    buffer_initial_bitrate_ = config_.init_bitrate;
    buffer_initialization_write_attempted_ = false;
    buffer_control_requested_ = false;
    force_idr_ = true;
    sample_clock_.Reset();
  }

  void ConfigureBufferControl(const char* phase, bool verify_current) {
    // Capability and writability are configuration-dependent. An optional
    // property rejected before media types are negotiated must not poison the
    // live encoder. These three initialization phases are the only retries;
    // frame processing retains the bounded dynamic failure cache.
    const bool requested =
        config_.video_content_type == MiniRtcVideoContentType::ScreenContent &&
        buffer_duration_ms_ > 0;
    const HRESULT support_hr = requested && api_
        ? api_->IsSupported(&CODECAPI_AVEncCommonBufferSize) : E_NOTIMPL;
    const bool supported = support_hr == S_OK;
    HRESULT range_hr = E_NOTIMPL;
    VARTYPE minimum_type = VT_EMPTY, maximum_type = VT_EMPTY,
            step_type = VT_EMPTY;
    std::optional<WmfEncoderBurstControl::Range> range;
    if (supported) {
      VARIANT minimum{}, maximum{}, step{};
      range_hr = api_->GetParameterRange(
          &CODECAPI_AVEncCommonBufferSize, &minimum, &maximum, &step);
      minimum_type = minimum.vt;
      maximum_type = maximum.vt;
      step_type = step.vt;
      if (range_hr == S_OK && minimum.vt == VT_UI4 && maximum.vt == VT_UI4 &&
          (step.vt == VT_UI4 || step.vt == VT_EMPTY) &&
          minimum.ulVal <= maximum.ulVal &&
          maximum.ulVal > 0)
        range = WmfEncoderBurstControl::Range{
            minimum.ulVal, maximum.ulVal,
            step.vt == VT_EMPTY ? 1u : step.ulVal};
      VariantClear(&minimum);
      VariantClear(&maximum);
      VariantClear(&step);
    }
    burst_control_.Configure(supported, range);
    LOG_DEBUG("WMF encoder burst control: phase={} buffer_ms={} "
              "initial_buffer_ms={} recovery_buffer_ms={} "
              "target_bps={} requested={} supported={} "
              "support_status=0x{:08x} range_known={} range_min={} "
              "range_max={} range_step={} range_status=0x{:08x} "
              "range_types={}/{}/{}",
              phase, buffer_duration_ms_, initial_buffer_duration_ms_,
              std::max(buffer_duration_ms_,
                       WmfEncoderBurstControl::kRecoveryBufferMs),
              config_.init_bitrate, requested, supported,
              static_cast<uint32_t>(support_hr),
              range.has_value(), range ? range->minimum : 0,
              range ? range->maximum : 0, range ? range->step : 0,
              static_cast<uint32_t>(range_hr), minimum_type, maximum_type,
              step_type);
    const auto request = burst_control_.InitialRequest(
        config_.init_bitrate, initial_buffer_duration_ms_);
    if (!request) return;
    if (verify_current) {
      VARIANT readback{};
      const HRESULT read_hr =
          api_->GetValue(&CODECAPI_AVEncCommonBufferSize, &readback);
      const auto actual = read_hr == S_OK && readback.vt == VT_UI4
          ? std::optional<uint32_t>(readback.ulVal) : std::nullopt;
      const VARTYPE read_type = readback.vt;
      VariantClear(&readback);
      const bool confirmed =
          burst_control_.VerifyInitialization(*request, actual);
      LOG_DEBUG("WMF encoder buffer verification: phase={} requested_bytes={} "
                "readback_bytes={} readback_known={} confirmed={} "
                "readback_status=0x{:08x} readback_type={}",
                phase, request->bytes, actual.value_or(0), actual.has_value(),
                confirmed, static_cast<uint32_t>(read_hr), read_type);
      if (confirmed) {
        // Preserve a valid early setting even when a static MFT stops allowing
        // writes after type negotiation; do not overwrite it just to verify it.
        LogBufferState(phase, config_.init_bitrate);
        return;
      }
    }
    UpdateBufferSize(config_.init_bitrate, true, phase);
  }

  void LogBufferState(const char* phase, int bitrate) {
    const auto actual = burst_control_.observed_bytes();
    // This is a property observation, not proof of bitstream HRD compliance or
    // a per-frame cap. Fixed bytes have a different duration after BWE changes.
    LOG_INFO("WMF encoder buffer state: encoder={} phase={} policy={} "
              "dynamic_capability={} writes_enabled={} "
              "initial_confirmed={} initial_bytes={} initial_bps={} "
              "last_requested_bytes={} last_request_bps={} "
              "readback_known={} readback_bytes={} target_bps={} "
              "readback_equivalent_ms={:.3f} recovery_only={} "
              "verification_scope=property",
              static_cast<void*>(encoder_.Get()), phase,
              burst_control_.policy_name(), burst_control_.dynamic_state_name(),
              burst_control_.writes_enabled(), burst_control_.initial_confirmed(),
              burst_control_.initial_bytes().value_or(0), buffer_initial_bitrate_,
              burst_control_.requested_bytes().value_or(0),
              last_buffer_request_bitrate_, actual.has_value(),
              actual.value_or(0), bitrate,
              actual && bitrate > 0 ? double(*actual) * 8000 / bitrate : 0.0,
              burst_control_.tighter_failed());
    last_buffer_log_ms_ = clock_->CurrentTimeMs();
    last_buffer_observation_bitrate_ = bitrate;
  }

  void ObserveBufferSize(int bitrate, bool keyframe) {
    const int64_t now_ms = clock_->CurrentTimeMs();
    // BWE can update many times per second. Bound read-only property queries
    // and state logs even when bitrate changes; encoded IDRs are still checked.
    const int interval_ms = bitrate == last_buffer_observation_bitrate_
                                ? 5000 : 1000;
    if (!keyframe && last_buffer_log_ms_ >= 0 &&
        now_ms - last_buffer_log_ms_ < interval_ms)
      return;
    // Read-only monitoring continues after writes stop, including after an
    // encoded IDR. It detects late/unknown readback without retrying SetValue.
    VARIANT readback{};
    const HRESULT hr = api_->GetValue(&CODECAPI_AVEncCommonBufferSize, &readback);
    burst_control_.ObserveRuntime(hr == S_OK && readback.vt == VT_UI4
        ? std::optional<uint32_t>(readback.ulVal) : std::nullopt);
    LOG_DEBUG("WMF encoder buffer observation: status=0x{:08x} type={}",
              static_cast<uint32_t>(hr), readback.vt);
    VariantClear(&readback);
    LogBufferState("runtime_observation", bitrate);
  }

  void UpdateBufferSize(int bitrate, bool keyframe,
                        const char* phase = "dynamic") {
    if (!api_ || !buffer_control_requested_) return;
    const bool initializing = std::strcmp(phase, "dynamic") != 0;
    if (initializing && !burst_control_.writes_enabled()) return;
    if (!initializing && sequence_ == 0) {
      // The negotiated initial allowance includes the first IDR, including an
      // explicit initialization experiment. Probe runtime changes after output.
      ObserveBufferSize(bitrate, false);
      return;
    }
    // At most one tighter attempt and one legacy fallback. Driver capability
    // and failures are cached for this MFT; optional writes never reconfigure
    // it or prevent a pending keyframe from being encoded.
    for (int attempt = 0; attempt < (initializing ? 1 : 2); ++attempt) {
      const auto request = initializing
          ? burst_control_.InitialRequest(bitrate, initial_buffer_duration_ms_)
          : burst_control_.Next(bitrate, keyframe);
      if (!request) {
        if (!initializing) ObserveBufferSize(bitrate, false);
        return;
      }
      const HRESULT writable =
          api_->IsModifiable(&CODECAPI_AVEncCommonBufferSize);
      if (initializing && writable != S_FALSE)
        buffer_initialization_write_attempted_ = true;
      const HRESULT hr = writable == S_FALSE
          ? E_ACCESSDENIED
          : SetUint(api_.Get(), CODECAPI_AVEncCommonBufferSize, request->bytes);
      VARIANT readback{};
      const HRESULT read_hr =
          api_->GetValue(&CODECAPI_AVEncCommonBufferSize, &readback);
      const auto actual = read_hr == S_OK && readback.vt == VT_UI4
          ? std::optional<uint32_t>(readback.ulVal) : std::nullopt;
      const VARTYPE read_type = readback.vt;
      VariantClear(&readback);
      // ICodecAPI::SetValue can return S_FALSE for a read-only property;
      // that is not an accepted write even though SUCCEEDED(S_FALSE) is true.
      bool retry = false;
      if (initializing)
        burst_control_.CompleteInitialization(*request, hr == S_OK, actual);
      else
        retry = burst_control_.Complete(*request, hr == S_OK, actual);
      last_buffer_request_bitrate_ = bitrate;
      const bool readback_mismatch = actual &&
          !WmfEncoderBurstControl::ReadbackSatisfies(*request, *actual);
      // Log each bounded write, including successful bitrate-driven changes.
      // Read-only steady-state observations remain rate limited.
      LOG_DEBUG("WMF encoder buffer: phase={} target_bps={} requested_bytes={} "
                  "readback_bytes={} "
                  "readback_known={} readback_mismatch={} buffer_ms={} "
                  "keyframe={} write_accepted={} "
                  "status=0x{:08x} modifiable_status=0x{:08x} "
                  "readback_status=0x{:08x} readback_type={} "
                  "legacy_fallback={} request_confirmed={}",
                  phase, bitrate, request->bytes, actual.value_or(0),
                  actual.has_value(),
                  readback_mismatch,
                  request->duration_ms, keyframe, hr == S_OK,
                  static_cast<uint32_t>(hr), static_cast<uint32_t>(writable),
                  static_cast<uint32_t>(read_hr), read_type,
                  burst_control_.tighter_failed(),
                  hr == S_OK && actual && !readback_mismatch);
      LogBufferState(phase, bitrate);
      if (!retry) return;
    }
  }

  HRESULT Configure(IMFActivate* activation, bool configure_buffer = true,
                    [[maybe_unused]] bool* applied_realtime = nullptr) {
    buffer_control_requested_ = configure_buffer && buffer_duration_ms_ > 0 &&
        config_.video_content_type == MiniRtcVideoContentType::ScreenContent;
    if (!buffer_control_requested_) burst_control_.Reset(0);
    HRESULT hr = activation->ActivateObject(IID_PPV_ARGS(&encoder_));
    if (FAILED(hr)) return hr;
    ComPtr<IMFAttributes> attributes;
    hr = encoder_->GetAttributes(&attributes);
    if (FAILED(hr)) return hr;
    if (MFGetAttributeUINT32(attributes.Get(), MF_TRANSFORM_ASYNC, FALSE)) {
      hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      if (FAILED(hr)) return hr;
      ComPtr<IMFMediaEventGenerator> events;
      hr = encoder_.As(&events);
      if (FAILED(hr)) return hr;
      event_reader_ = Microsoft::WRL::Make<MftEventReader>(events.Get());
      if (!event_reader_) return E_OUTOFMEMORY;
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
#ifdef CODECAPI_AVEncVideoEnableSpatialAdaptiveQuantization
    if (applied_realtime && config_.UseDesktopRealtimeRateControl() &&
        api_->IsSupported(&CODECAPI_AVEncVideoEnableSpatialAdaptiveQuantization) == S_OK) {
      VARIANT aq{};
      aq.vt = VT_BOOL;
      aq.boolVal = VARIANT_FALSE;
      *applied_realtime = SUCCEEDED(api_->SetValue(
          &CODECAPI_AVEncVideoEnableSpatialAdaptiveQuantization, &aq));
    }
#endif
    VARIANT low_latency{};
    low_latency.vt = VT_BOOL;
    low_latency.boolVal = VARIANT_TRUE;
    api_->SetValue(&CODECAPI_AVLowLatencyMode, &low_latency);
    SetUint(api_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    SetUint(api_.Get(), CODECAPI_AVEncMPVGOPSize, config_.key_frame_interval);
    HRESULT speed_hr =
        SetUint(api_.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 0);

    // Offer the optional HRD setting before media-type negotiation, when an
    // MFT can still accept static options. Recheck after stream start rather
    // than assuming
    // SetValue(S_OK) or an early readback proves the live HRD configuration.
    if (configure_buffer) ConfigureBufferControl("before_types", false);

    ComPtr<IMFMediaType> output, input;
    hr = MFCreateMediaType(&output);
    if (FAILED(hr)) return hr;
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE,
                    hevc_ ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, config_.init_bitrate);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    // AVC Baseline also forbids B pictures if the driver ignores the
    // B-picture property. HEVC Main is the negotiated 8-bit 4:2:0 profile.
    const UINT32 profile = hevc_ ? UINT32(eAVEncH265VProfile_Main_420_8)
                                 : UINT32(eAVEncH264VProfile_Base);
    output->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
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
    if (speed_hr != S_OK)
      speed_hr = SetUint(api_.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 0);
    hr = SetUint(api_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                 config_.init_bitrate);
    if (FAILED(hr)) return hr;
    if (configure_buffer) ConfigureBufferControl("types_ready", true);
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return hr;
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return hr;
    VARIANT speed{};
    const HRESULT read_hr = api_->GetValue(&CODECAPI_AVEncCommonQualityVsSpeed,
                                           &speed);
    LOG_INFO("WMF encoder speed: requested=0 readback={} set_status=0x{:08x}",
             read_hr == S_OK && speed.vt == VT_UI4 ? int64_t(speed.ulVal) : -1,
             static_cast<uint32_t>(speed_hr));
    VariantClear(&speed);
    if (configure_buffer) ConfigureBufferControl("streaming", true);
    if (!buffer_control_requested_)
      LogBufferState("without_optional_buffer", config_.init_bitrate);
    return S_OK;
  }

  HRESULT Initialize() {
    Reset();
    if (FAILED(worker_.Status())) return worker_.Status();
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    HRESULT hr = EnumerateEncoders(hevc_, &activations, &count);
    if (FAILED(hr)) return hr;
    hr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; ++i) {
      if (!encoder_) {
        bool applied_realtime = false;
        hr = Configure(activations[i], true, &applied_realtime);
        if (FAILED(hr) && applied_realtime) {
          // Some MFTs accept a property but reject the combination at start.
          Reset();
          hr = activations[i]->ShutdownObject();
          if (SUCCEEDED(hr)) hr = Configure(activations[i]);
        }
        if (FAILED(hr) && buffer_initialization_write_attempted_) {
          // Even an accepted optional property can make subsequent media-type
          // negotiation or stream start fail. Reactivate this same MFT once
          // with no HRD writes before falling back to another encoder. This
          // path runs only during initialization, never for dynamic updates.
          LOG_DEBUG("WMF encoder retry without optional buffer control: "
                    "initialization_status=0x{:08x}",
                    static_cast<uint32_t>(hr));
          Reset();
          const HRESULT shutdown_hr = activations[i]->ShutdownObject();
          hr = FAILED(shutdown_hr) ? shutdown_hr
                                   : Configure(activations[i], false);
        }
        if (FAILED(hr)) {
          LOG_DEBUG("Hardware {} MFT initialization failed: 0x{:08x}",
                    hevc_ ? "H.265" : "H.264", static_cast<uint32_t>(hr));
          Reset();
          activations[i]->ShutdownObject();
        } else {
          WCHAR name[256]{};
          activations[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256,
                                    nullptr);
          char utf8[1024]{};
          WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr,
                              nullptr);
          LOG_INFO("Media Foundation hardware {} encoder: {}",
                   hevc_ ? "H.265" : "H.264", utf8);
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

  unsigned CacheHeaders(const uint8_t* data,
                        const std::vector<wmf::Nalu>& nalus,
                        unsigned already_present = 0) {
    // HEVC uses consecutive VPS/SPS/PPS types 32..34, AVC uses SPS/PPS 7..8.
    unsigned present = 0;
    for (const auto& nalu : nalus) {
      const int index = nalu.type - (hevc_ ? 32 : 7);
      if (index < 0 || index >= (hevc_ ? 3 : 2)) continue;
      if (already_present & (1u << index)) continue;
      present |= 1u << index;
      auto& header = parameter_sets_[index];
      header = {0, 0, 0, 1};
      header.insert(header.end(), data + nalu.offset,
                    data + nalu.offset + nalu.size);
    }
    return present;
  }

  HRESULT ReadFrame(IMFSample* sample, const RawFrame& raw,
                    std::vector<EncodedFrame>& frames) {
    std::vector<uint8_t> bytes;
    HRESULT hr = wmf::ReadBytes(sample, bytes);
    if (FAILED(hr)) return hr;
    const auto nalus = wmf::AnnexBNalus(bytes.data(), bytes.size(), hevc_);
    if (nalus.empty()) return MF_E_INVALID_STREAM_DATA;
    const bool key =
        std::any_of(nalus.begin(), nalus.end(),
                    [&](const auto& nalu) { return nalu.IsKeySlice(hevc_); });
    const unsigned present = CacheHeaders(bytes.data(), nalus);
    if (hevc_ && std::none_of(nalus.begin(), nalus.end(),
                              [](const auto& nalu) { return nalu.type <= 31; }))
      return S_FALSE;  // Parameter sets can precede the coded picture.
    if (force_idr_ && !key) return MF_E_INVALID_STREAM_DATA;
    const int count = hevc_ ? 3 : 2;
    if (key && present != (1u << count) - 1) {
      ComPtr<IMFMediaType> type;
      UINT32 size = 0;
      if (std::any_of(parameter_sets_.begin(), parameter_sets_.begin() + count,
                      [](const auto& header) { return header.empty(); }) &&
          SUCCEEDED(encoder_->GetOutputCurrentType(output_id_, &type)) &&
          SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size))) {
        std::vector<uint8_t> header(size);
        if (SUCCEEDED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(),
                                    size, nullptr)))
          CacheHeaders(header.data(),
                       wmf::AnnexBNalus(header.data(), size, hevc_), present);
      }
      std::vector<uint8_t> headers;
      for (int i = 0; i < count; ++i) {
        const auto& header = parameter_sets_[i];
        if (header.empty()) return MF_E_INVALID_STREAM_DATA;
        headers.insert(headers.end(), header.begin(), header.end());
      }
      bytes.insert(bytes.begin(), headers.begin(), headers.end());
    }
    EncodedFrame frame(bytes.data(), bytes.size(), raw.Width(), raw.Height());
    frame.SetEncodedWidth(raw.Width());
    frame.SetEncodedHeight(raw.Height());
    frame.SetFrameType(key ? kVideoFrameKey : kVideoFrameDelta);
    frame.SetCapturedTimestamp(raw.CapturedTimestamp());
    frame.SetEncodedTimestamp(clock_->CurrentTime());
    if (!hevc_) {
      parser_.ParseBitstream(bytes.data(), bytes.size());
      if (const auto qp = parser_.GetLastSliceQp())
        frame.SetQp(*qp, 0, 51, false);
    }
    frames.push_back(std::move(frame));
    if (buffer_control_requested_)
      ObserveBufferSize(config_.init_bitrate, key);
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
    UpdateBufferSize(config_.init_bitrate, force_idr_);
    if (force_idr_) {
      HRESULT hr = SetUint(api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
      if (FAILED(hr)) return hr;
    }
    ComPtr<IMFSample> input;
    const auto timing = sample_clock_.Next(
        raw.CapturedTimestamp(), clock_->CurrentTimeUs(), input_frame_rate_);
    size_t input_size = 0;
    GetNv12FrameSize(raw.Width(), raw.Height(), &input_size);
    HRESULT hr = wmf::CreateSample(raw.Buffer(), input_size, timing.time_100ns,
                                   timing.duration_100ns, input);
    if (FAILED(hr)) return hr;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool submitted = false;
    // Low-latency, no-B-frame mode must produce this frame before accepting the
    // next one. A bounded wait also handles driver/device loss without hanging
    // the media thread.
    while (std::chrono::steady_clock::now() < deadline) {
      if (!submitted && (!event_reader_ || input_credit_)) {
        hr = encoder_->ProcessInput(input_id_, input.Get(), 0);
        if (SUCCEEDED(hr)) {
          submitted = true;
          if (event_reader_) --input_credit_;
        } else if (hr != MF_E_NOTACCEPTING)
          return hr;
      }
      if (event_reader_) {
        ComPtr<IMFMediaEvent> event;
        hr = event_reader_->Next(deadline, event);
        if (FAILED(hr)) return hr;
        HRESULT status = S_OK;
        hr = event->GetStatus(&status);
        if (FAILED(hr)) return hr;
        if (FAILED(status)) return status;
        MediaEventType type = MEUnknown;
        hr = event->GetType(&type);
        if (FAILED(hr)) return hr;
        if (type == METransformNeedInput) ++input_credit_;
        if (type != METransformHaveOutput) continue;
      }
      ComPtr<IMFSample> output;
      hr = wmf::ReadOutput(encoder_.Get(), output_id_, output);
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        const GUID& codec = hevc_ ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        for (DWORD i = 0;; ++i) {
          ComPtr<IMFMediaType> type;
          hr = encoder_->GetOutputAvailableType(output_id_, i, &type);
          if (FAILED(hr)) return hr;
          GUID subtype{};
          if (SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
              subtype == codec &&
              SUCCEEDED(encoder_->SetOutputType(output_id_, type.Get(), 0)))
            break;
        }
        // An asynchronous MFT consumes the HaveOutput notification even
        // when it reports a format change. Wait for the next notification;
        // another ProcessOutput here fails with E_UNEXPECTED on Intel MFTs.
        if (event_reader_) continue;
        hr = wmf::ReadOutput(encoder_.Get(), output_id_, output);
      }
      if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) return hr;
      if (SUCCEEDED(hr) && output) {
        hr = ReadFrame(output.Get(), raw, frames);
        if (FAILED(hr)) return hr;
        if (hr == S_FALSE) continue;
        ++sequence_;
        return S_OK;
      }
      if (!event_reader_)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
  }

  wmf::CodecThread worker_;
  std::shared_ptr<SystemClock> clock_;
  const bool hevc_;
  MediaCodecConfig config_;
  ComPtr<IMFTransform> encoder_;
  ComPtr<MftEventReader> event_reader_;
  ComPtr<ICodecAPI> api_;
  DWORD input_id_ = 0, output_id_ = 0;
  unsigned input_credit_ = 0;
  const int buffer_duration_ms_ = WmfEncoderBurstControl::BufferMsFromEnvironment();
  const int initial_buffer_duration_ms_ =
      WmfEncoderBurstControl::InitialBufferMsFromEnvironment(buffer_duration_ms_);
  WmfEncoderBurstControl burst_control_;
  bool buffer_initialization_write_attempted_ = false;
  bool buffer_control_requested_ = false;
  int64_t last_buffer_log_ms_ = -1;
  int last_buffer_observation_bitrate_ = 0;
  int last_buffer_request_bitrate_ = 0;
  int buffer_initial_bitrate_ = 0;
  bool force_idr_ = true, reconfigure_ = false;
  bool initialized_ = false;
  uint64_t sequence_ = 0;
  int input_frame_rate_ = 60;
  EncoderSampleClock sample_clock_;
  std::array<std::vector<uint8_t>, 3> parameter_sets_;
  H264BitstreamParser parser_;
  std::unique_ptr<OpenH264Encoder> fallback_;
};

WmfVideoEncoder::WmfVideoEncoder(std::shared_ptr<SystemClock> clock,
                                 VideoCodecType codec)
    : impl_(std::make_unique<Impl>(std::move(clock), codec)) {}
WmfVideoEncoder::~WmfVideoEncoder() = default;

bool WmfVideoEncoder::IsSupported(VideoCodecType codec) {
  if (codec == VideoCodecType::H265) {
    // A registered encoder may reject NV12 / Main or fail to start. Probe
    // with the same initial configuration used by native sessions.
    WmfVideoEncoder probe(std::make_shared<SystemClock>(), codec);
    return probe.Init(MediaCodecConfig{}) == 0;
  }
  if (codec != VideoCodecType::H264) return false;
  wmf::CodecThread worker;
  if (FAILED(worker.Status())) return false;
  return worker.Call([] {
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT hr = EnumerateEncoders(false, &activations, &count);
    for (UINT32 i = 0; i < count; ++i) activations[i]->Release();
    CoTaskMemFree(activations);
    return SUCCEEDED(hr) && count > 0;
  });
}

int WmfVideoEncoder::Init(const MediaCodecConfig& config) {
  if (!ValidConfig(config)) return -1;
  return impl_->worker_.Call([&] {
    impl_->fallback_.reset();
    impl_->config_ = config;
    impl_->input_frame_rate_ = config.max_frame_rate;
    impl_->config_.init_bitrate =
        ClampEncoderTargetBitrate(config.init_bitrate, config.max_bitrate);
    impl_->initialized_ = SUCCEEDED(impl_->Initialize());
    return impl_->initialized_ ? 0 : -1;
  });
}

int WmfVideoEncoder::Encode(const RawFrame& raw,
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
      if (impl_->hevc_) {
        LOG_WARN("Media Foundation HEVC encoding failed (0x{:08x})",
                 static_cast<uint32_t>(hr));
        impl_->Reset();
        impl_->reconfigure_ = true;
        return -1;
      }
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
      impl_->fallback_->SetTargetFrameRate(impl_->input_frame_rate_);
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

int WmfVideoEncoder::ForceIdr() {
  return impl_->worker_.Call([&] {
    impl_->force_idr_ = true;
    return impl_->fallback_ ? impl_->fallback_->ForceIdr() : 0;
  });
}

int WmfVideoEncoder::SetTargetBitrate(int bitrate) {
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
    impl_->UpdateBufferSize(bitrate, impl_->force_idr_);
    return 0;
  });
}

int WmfVideoEncoder::SetTargetFrameRate(int frame_rate) {
  if (frame_rate <= 0 || frame_rate > 240) return -1;
  return impl_->worker_.Call([&] {
    if (impl_->fallback_) {
      const int result = impl_->fallback_->SetTargetFrameRate(frame_rate);
      if (result == 0) {
        impl_->config_.max_frame_rate = frame_rate;
        impl_->input_frame_rate_ = frame_rate;
      }
      return result;
    }
    if (!impl_->encoder_) return -1;
    if (impl_->config_.max_frame_rate != frame_rate) impl_->reconfigure_ = true;
    impl_->config_.max_frame_rate = frame_rate;
    impl_->input_frame_rate_ = frame_rate;
    return 0;
  });
}

int WmfVideoEncoder::SetInputFrameRate(int frame_rate) {
  if (frame_rate <= 0 || frame_rate > 240) return -1;
  return impl_->worker_.Call([&] {
    if (!impl_->initialized_) return -1;
    const int fps = std::min(frame_rate, impl_->config_.max_frame_rate);
    if (fps == impl_->input_frame_rate_) return 0;
    if (impl_->fallback_ && impl_->fallback_->SetTargetFrameRate(fps) != 0)
      return -1;
    // Keep the negotiated MFT type/capture ceiling stable. Cadence changes are
    // represented by real sample PTS and duration, not streaming type changes
    // that reset the hardware encoder and force an IDR every second.
    impl_->input_frame_rate_ = fps;
    return 0;
  });
}

int WmfVideoEncoder::GetResolution(int* width, int* height) const {
  if (!width || !height) return -1;
  return impl_->worker_.Call([&] {
    if (impl_->fallback_) return impl_->fallback_->GetResolution(width, height);
    if (!impl_->encoder_) return -1;
    *width = impl_->config_.init_width;
    *height = impl_->config_.init_height;
    return 0;
  });
}

std::string WmfVideoEncoder::GetEncoderName() const {
  return impl_->worker_.Call([&]() -> std::string {
    if (impl_->fallback_) return impl_->fallback_->GetEncoderName();
    return impl_->hevc_ ? "Media Foundation (H.265, HW)"
                        : "Media Foundation (H.264, HW)";
  });
}
}  // namespace minirtc
