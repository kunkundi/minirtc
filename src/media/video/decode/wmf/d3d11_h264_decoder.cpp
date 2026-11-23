#include "d3d11_h264_decoder.h"

#include <codecapi.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wmcodecdsp.h>

#include <cstring>
#include <limits>
#include <map>

#include "native_nv12_frame.h"
#include "openh264/openh264_decoder.h"
#include "wmf/wmf_utils.h"

namespace minirtc {
namespace {
using wmf::ComPtr;

HRESULT CreateVideoDevice(ComPtr<ID3D11Device>& device,
                          ComPtr<ID3D11DeviceContext>& context) {
  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) return hr;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(index, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(hr)) return hr;
    DXGI_ADAPTER_DESC1 adapter_desc{};
    if (FAILED(adapter->GetDesc1(&adapter_desc)) ||
        (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
      continue;
    ComPtr<ID3D11Device> candidate;
    ComPtr<ID3D11DeviceContext> candidate_context;
    hr = D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &candidate, nullptr, &candidate_context);
    if (FAILED(hr)) continue;
    ComPtr<ID3D11VideoDevice> video;
    if (FAILED(candidate.As(&video))) continue;
    BOOL supported = FALSE;
    hr = video->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
                                        DXGI_FORMAT_NV12, &supported);
    if (FAILED(hr) || !supported) continue;
    D3D11_VIDEO_DECODER_DESC desc{};
    desc.Guid = D3D11_DECODER_PROFILE_H264_VLD_NOFGT;
    desc.SampleWidth = 1920;
    desc.SampleHeight = 1080;
    desc.OutputFormat = DXGI_FORMAT_NV12;
    UINT configs = 0;
    if (FAILED(video->GetVideoDecoderConfigCount(&desc, &configs)) || !configs)
      continue;
    ComPtr<ID3D10Multithread> multithread;
    if (FAILED(candidate.As(&multithread))) continue;
    multithread->SetMultithreadProtected(TRUE);
    device = candidate;
    context = candidate_context;
    return S_OK;
  }
  return MF_E_UNSUPPORTED_D3D_TYPE;
}

struct FrameOutput {
  DecodedFrame frame;
  // Retains pooled pixels until the caller has finished its callback.
  NativeVideoFrameRef native;
};
}  // namespace

class D3D11H264Decoder::Impl {
 public:
  Impl(std::shared_ptr<SystemClock> clock, bool native_output)
      : clock_(std::move(clock)),
        native_output_(native_output),
        pool_(NativeNv12FramePool::Create()) {}
  ~Impl() {
    worker_.Call([this] {
      Reset();
      fallback_.reset();
    });
  }

  void Reset() {
    wmf::ShutdownTransform(decoder_);
    staging_.Reset();
    manager_.Reset();
    context_.Reset();
    device_.Reset();
    timestamps_.clear();
    width_ = height_ = crop_x_ = crop_y_ = 0;
  }

  HRESULT SetOutputType() {
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> type;
      HRESULT hr = decoder_->GetOutputAvailableType(0, i, &type);
      if (FAILED(hr)) return hr;
      GUID subtype{};
      if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
          subtype != MFVideoFormat_NV12)
        continue;
      hr = decoder_->SetOutputType(0, type.Get(), 0);
      if (FAILED(hr)) continue;
      hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width_, &height_);
      if (FAILED(hr)) return hr;
      crop_x_ = crop_y_ = 0;
      MFVideoArea area{};
      if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                  reinterpret_cast<UINT8*>(&area), sizeof(area),
                                  nullptr)) ||
          SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE,
                                  reinterpret_cast<UINT8*>(&area), sizeof(area),
                                  nullptr))) {
        if (area.OffsetX.value < 0 || area.OffsetY.value < 0 ||
            area.OffsetX.fract || area.OffsetY.fract || area.Area.cx <= 0 ||
            area.Area.cy <= 0)
          return MF_E_INVALIDMEDIATYPE;
        crop_x_ = area.OffsetX.value;
        crop_y_ = area.OffsetY.value;
        width_ = area.Area.cx;
        height_ = area.Area.cy;
      }
      size_t size = 0;
      if (width_ > 8192 || height_ > 8192 || (crop_x_ & 1) || (crop_y_ & 1) ||
          !GetNv12FrameSize(width_, height_, &size))
        return MF_E_INVALIDMEDIATYPE;
      staging_.Reset();
      return S_OK;
    }
  }

  HRESULT Initialize() {
    Reset();
    if (FAILED(worker_.Status())) return worker_.Status();
    HRESULT hr = CreateVideoDevice(device_, context_);
    if (FAILED(hr)) return hr;
    UINT token = 0;
    hr = MFCreateDXGIDeviceManager(&token, &manager_);
    if (FAILED(hr)) return hr;
    hr = manager_->ResetDevice(device_.Get(), token);
    if (FAILED(hr)) return hr;
    hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr,
                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&decoder_));
    if (FAILED(hr)) return hr;
    ComPtr<IMFAttributes> attributes;
    hr = decoder_->GetAttributes(&attributes);
    if (FAILED(hr)) return hr;
    if (!MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE))
      return MF_E_UNSUPPORTED_D3D_TYPE;
    attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
    hr = decoder_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  reinterpret_cast<ULONG_PTR>(manager_.Get()));
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaType> input;
    hr = MFCreateMediaType(&input);
    if (FAILED(hr)) return hr;
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    // The SPS determines dimensions. The initial output type is a placeholder;
    // ProcessOutput signals a stream change when the real format is known.
    hr = decoder_->SetInputType(0, input.Get(), 0);
    if (FAILED(hr)) return hr;
    hr = SetOutputType();
    if (FAILED(hr)) return hr;
    hr = decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) return hr;
    return decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  }

  HRESULT CopyFrame(IMFSample* sample, FrameOutput& output) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->GetBufferByIndex(0, &buffer);
    if (FAILED(hr)) return hr;
    ComPtr<IMFDXGIBuffer> dxgi;
    hr = buffer.As(&dxgi);
    // Do not report hardware decoding when the MFT silently returns CPU data.
    if (FAILED(hr)) return MF_E_UNSUPPORTED_D3D_TYPE;
    ComPtr<ID3D11Texture2D> texture;
    hr = dxgi->GetResource(IID_PPV_ARGS(&texture));
    if (FAILED(hr)) return hr;
    UINT subresource = 0;
    hr = dxgi->GetSubresourceIndex(&subresource);
    if (FAILED(hr)) return hr;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_NV12 || width_ > desc.Width ||
        height_ > desc.Height || crop_x_ > desc.Width - width_ ||
        crop_y_ > desc.Height - height_)
      return MF_E_INVALIDMEDIATYPE;
    D3D11_TEXTURE2D_DESC staged{};
    if (staging_) staging_->GetDesc(&staged);
    if (!staging_ || staged.Width != desc.Width ||
        staged.Height != desc.Height) {
      staging_.Reset();
      staged = desc;
      staged.MipLevels = staged.ArraySize = 1;
      staged.Usage = D3D11_USAGE_STAGING;
      staged.BindFlags = staged.MiscFlags = 0;
      staged.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      hr = device_->CreateTexture2D(&staged, nullptr, &staging_);
      if (FAILED(hr)) return hr;
    }
    context_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, texture.Get(),
                                    subresource, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;
    if (mapped.RowPitch < crop_x_ + width_) {
      context_->Unmap(staging_.Get(), 0);
      return MF_E_BUFFERTOOSMALL;
    }
    size_t size = 0;
    GetNv12FrameSize(width_, height_, &size);
    uint8_t* destination = nullptr;
    if (native_output_) {
      auto* native = pool_->Acquire(width_, height_);
      if (!native) {
        context_->Unmap(staging_.Get(), 0);
        return E_OUTOFMEMORY;
      }
      output.native = NativeVideoFrameRef(native->Descriptor());
      destination = native->YPlane();
      native->Release();
      output.frame.SetSize(size);
    } else {
      output.frame = DecodedFrame(size, width_, height_);
      destination = output.frame.MutableBuffer();
    }
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    const size_t pitch = mapped.RowPitch;
    for (UINT y = 0; y < height_; ++y)
      std::memcpy(destination + static_cast<size_t>(y) * width_,
                  source + (crop_y_ + y) * pitch + crop_x_, width_);
    const size_t pixels = static_cast<size_t>(width_) * height_;
    for (UINT y = 0; y < height_ / 2; ++y)
      std::memcpy(destination + pixels + static_cast<size_t>(y) * width_,
                  source + (desc.Height + crop_y_ / 2 + y) * pitch + crop_x_,
                  width_);
    context_->Unmap(staging_.Get(), 0);
    LONGLONG time = 0;
    hr = sample->GetSampleTime(&time);
    if (FAILED(hr)) return hr;
    const auto metadata = timestamps_.find(time);
    if (metadata == timestamps_.end()) return MF_E_INVALID_TIMESTAMP;
    output.frame.SetWidth(width_);
    output.frame.SetHeight(height_);
    output.frame.SetDecodedWidth(width_);
    output.frame.SetDecodedHeight(height_);
    output.frame.SetCapturedTimestamp(metadata->second.first);
    output.frame.SetReceivedTimestamp(metadata->second.second);
    output.frame.SetDecodedTimestamp(clock_->CurrentTime());
    timestamps_.erase(metadata);
    return S_OK;
  }

  HRESULT ReadFrames(std::vector<FrameOutput>& output) {
    // Bound retries for malformed streams and repeated driver stream changes.
    for (unsigned i = 0; i < 64; ++i) {
      ComPtr<IMFSample> sample;
      const HRESULT hr = wmf::ReadOutput(decoder_.Get(), 0, sample);
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        const HRESULT changed = SetOutputType();
        if (FAILED(changed)) return changed;
        continue;
      }
      if (FAILED(hr)) return hr;
      if (!sample) continue;
      FrameOutput frame;
      const HRESULT copied = CopyFrame(sample.Get(), frame);
      if (FAILED(copied)) return copied;
      output.push_back(std::move(frame));
    }
    return E_UNEXPECTED;
  }

  HRESULT DecodeHardware(const ReceivedFrame& frame,
                         std::vector<FrameOutput>& output) {
    if (!decoder_) return MF_E_NOT_INITIALIZED;
    ComPtr<IMFSample> sample;
    const LONGLONG time = next_time_;
    next_time_ += 166667;
    HRESULT hr =
        wmf::CreateSample(frame.Buffer(), frame.Size(), time, 166667, sample);
    if (FAILED(hr)) return hr;
    hr = decoder_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
      hr = ReadFrames(output);
      if (FAILED(hr)) return hr;
      // The rejected input was not consumed. Retry that exact sample.
      hr = decoder_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return hr;
    timestamps_[time] = {frame.CapturedTimestamp(), frame.ReceivedTimestamp()};
    if (timestamps_.size() > 64) return MF_E_NOTACCEPTING;
    return ReadFrames(output);
  }

  int StartFallback() {
    Reset();
    fallback_ = std::make_unique<OpenH264Decoder>(clock_, native_output_);
    if (fallback_->Init() == 0) return 0;
    fallback_.reset();
    return -1;
  }

  wmf::CodecThread worker_;
  std::shared_ptr<SystemClock> clock_;
  bool native_output_;
  bool initialized_ = false;
  std::shared_ptr<NativeNv12FramePool> pool_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IMFDXGIDeviceManager> manager_;
  ComPtr<IMFTransform> decoder_;
  ComPtr<ID3D11Texture2D> staging_;
  UINT width_ = 0, height_ = 0, crop_x_ = 0, crop_y_ = 0;
  LONGLONG next_time_ = 0;
  std::map<LONGLONG, std::pair<int64_t, int64_t>> timestamps_;
  std::unique_ptr<OpenH264Decoder> fallback_;
};

D3D11H264Decoder::D3D11H264Decoder(std::shared_ptr<SystemClock> clock,
                                   bool native_video_output)
    : impl_(std::make_unique<Impl>(std::move(clock), native_video_output)) {}
D3D11H264Decoder::~D3D11H264Decoder() = default;

bool D3D11H264Decoder::IsSupported() {
  Impl probe(std::make_shared<SystemClock>(), false);
  return probe.worker_.Call([&] { return SUCCEEDED(probe.Initialize()); });
}

int D3D11H264Decoder::Init() {
  return impl_->worker_.Call([&] {
    impl_->fallback_.reset();
    const HRESULT hr = impl_->Initialize();
    if (SUCCEEDED(hr)) {
      impl_->initialized_ = true;
      return 0;
    }
    LOG_WARN("D3D11VA initialization failed (0x{:08x}); using OpenH264",
             static_cast<uint32_t>(hr));
    impl_->initialized_ = impl_->StartFallback() == 0;
    return impl_->initialized_ ? 0 : -1;
  });
}

int D3D11H264Decoder::Decode(
    std::unique_ptr<ReceivedFrame> frame,
    std::function<void(const DecodedFrame*)> callback) {
  if (!frame || !frame->Buffer() || !frame->Size() ||
      frame->Size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    return -1;
  std::vector<FrameOutput> output;
  const int result = impl_->worker_.Call([&] {
    if (!impl_->initialized_) return -1;
    if (!impl_->fallback_) {
      const HRESULT hr = impl_->DecodeHardware(*frame, output);
      if (SUCCEEDED(hr)) return 0;
      LOG_WARN("D3D11VA decoding failed (0x{:08x}); using OpenH264",
               static_cast<uint32_t>(hr));
      if (impl_->StartFallback() != 0) return -1;
      // A fresh decoder cannot recover reference frames. Request a key frame
      // through the existing negative Decode() result when switching on a P
      // frame.
      if (!wmf::HasNalu(frame->Buffer(), frame->Size(), 5)) return -1;
    }
    return impl_->fallback_->Decode(
        std::move(frame), [&](const DecodedFrame* decoded) {
          FrameOutput copy;
          copy.frame = *decoded;
          copy.native = NativeVideoFrameRef(decoded->NativeFrame());
          output.push_back(std::move(copy));
        });
  });
  for (auto& decoded : output) {
    decoded.frame.SetNativeFrame(decoded.native.Get());
    if (callback) callback(&decoded.frame);
  }
  return result;
}

std::string D3D11H264Decoder::GetDecoderName() const {
  return impl_->worker_.Call([&]() -> std::string {
    return impl_->fallback_ ? impl_->fallback_->GetDecoderName()
                            : "D3D11VA (H.264)";
  });
}
}  // namespace minirtc
