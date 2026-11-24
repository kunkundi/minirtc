#ifdef _WIN32
#include "nv_encoder_d3d11.h"

#include <dxgi1_2.h>

#include "log.h"

namespace minirtc {
using Microsoft::WRL::ComPtr;

std::unique_ptr<NvEncoderD3D11> NvEncoderD3D11::Create(uint32_t width,
                                                       uint32_t height) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return {};
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0;
       factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) == S_OK;
       ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) || desc.VendorId != 0x10de ||
        (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
      continue;
    ComPtr<ID3D11Device> device;
    if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                    nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    nullptr, 0, D3D11_SDK_VERSION, &device,
                                    nullptr, nullptr)))
      return std::unique_ptr<NvEncoderD3D11>(
          new NvEncoderD3D11(device.Get(), width, height));
  }
  return {};
}

NvEncoderD3D11::NvEncoderD3D11(ID3D11Device* device, uint32_t width,
                               uint32_t height)
    : NvEncoder(NV_ENC_DEVICE_TYPE_DIRECTX, device, width, height,
                NV_ENC_BUFFER_FORMAT_NV12, 0, false),
      device_(device) {
  device_->GetImmediateContext(&context_);
}

void NvEncoderD3D11::AllocateInputBuffers(int32_t count) {
  // With no lookahead, B-frames or extra output delay, EncodeFrame waits for
  // the bitstream and unmaps this input before its owner releases the mutex.
  if (count != 1)
    NVENC_THROW_ERROR("Direct texture input requires synchronous output",
                      NV_ENC_ERR_UNSUPPORTED_PARAM);
  m_vRegisteredResources.resize(1);
}

void NvEncoderD3D11::Clear(Input& input) {
  if (input.registered)
    m_nvenc.nvEncUnregisterResource(m_hEncoder, input.registered);
  input = {};
}

void NvEncoderD3D11::ReleaseInputBuffers() {
  // The registration in the active slot is borrowed from one of our entries.
  m_vRegisteredResources.clear();
  UnregisterInputResources();
  Finish();
  for (auto& input : shared_) Clear(input);
  Clear(upload_);
}

NvEncoderD3D11::Input* NvEncoderD3D11::OpenShared(
    const MiniRtcNativeVideoFrame& frame) {
  auto* source =
      static_cast<ID3D11Texture2D*>(frame.payload.d3d11_nv12.texture);
  for (auto& input : shared_) {
    if (input.source.Get() == source)
      return input.registered ? &input : nullptr;
  }
  auto& input = shared_[next_];
  next_ = (next_ + 1) % shared_.size();
  Clear(input);
  input.source = source;
  if (FAILED(device_->OpenSharedResource(frame.payload.d3d11_nv12.shared_handle,
                                         IID_PPV_ARGS(&input.texture))) ||
      FAILED(input.texture.As(&input.mutex)))
    return nullptr;
  D3D11_TEXTURE2D_DESC desc{};
  input.texture->GetDesc(&desc);
  if (desc.Format != DXGI_FORMAT_NV12 || desc.Width != frame.width ||
      desc.Height != frame.height || desc.ArraySize != 1 || desc.MipLevels != 1)
    return nullptr;
  try {
    input.registered = RegisterResource(
        input.texture.Get(), NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX, desc.Width,
        desc.Height, 0, NV_ENC_BUFFER_FORMAT_NV12);
  } catch (const NVENCException& e) {
    LOG_WARN("NVENC shared texture registration failed: {}", e.what());
    return nullptr;
  }
  return &input;
}

bool NvEncoderD3D11::Prepare(const RawFrame& frame) {
  const auto* native = frame.NativeFrame();
  if (native && native->type == MiniRtcNativeVideoFrameD3D11Nv12) {
    auto* input = OpenShared(*native);
    if (input) {
      const HRESULT acquired = input->mutex->AcquireSync(0, 100);
      if (acquired != S_OK) {
        if (FAILED(device_->GetDeviceRemovedReason()))
          NVENC_THROW_ERROR("NVENC input device was removed", NV_ENC_ERR_DEVICE_NOT_EXIST);
        return false;
      }
      acquired_ = input->mutex;
      m_vRegisteredResources[0] = input->registered;
      if (!shared_logged_) {
        LOG_INFO("NVENC consuming shared DXGI NV12 textures directly ({}x{})",
                 frame.Width(), frame.Height());
        shared_logged_ = true;
      }
      return true;
    }
  }
  if (shared_logged_) {
    LOG_INFO("NVENC switching to CPU NV12 input ({}x{})", frame.Width(),
             frame.Height());
    shared_logged_ = false;
  }
  return Upload(frame);
}

bool NvEncoderD3D11::Upload(const RawFrame& frame) {
  const auto w = frame.Width(), h = frame.Height();
  D3D11_TEXTURE2D_DESC desc{};
  if (upload_.texture) upload_.texture->GetDesc(&desc);
  if (!upload_.registered || w != desc.Width || h != desc.Height) {
    Clear(upload_);
    desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.Format = DXGI_FORMAT_NV12;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &upload_.texture)))
      NVENC_THROW_ERROR("NVENC upload texture allocation failed", NV_ENC_ERR_OUT_OF_MEMORY);
    upload_.registered = RegisterResource(upload_.texture.Get(),
                                          NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX, w,
                                          h, 0, NV_ENC_BUFFER_FORMAT_NV12);
  }
  const uint8_t* pixels = frame.Buffer();
  const auto* native = frame.NativeFrame();
  if (native) {
    const auto& cpu = native->payload.cpu_nv12;
    if (native->type == MiniRtcNativeVideoFrameCpuNv12 && cpu.y_stride == w &&
        cpu.uv_stride == w && cpu.uv_plane == cpu.y_plane + size_t(w) * h) {
      pixels = cpu.y_plane;
    } else {
      scratch_.resize(size_t(w) * h * 3 / 2);
      if (native->copy_to_nv12(native->owner, scratch_.data(),
                               scratch_.size()) != 0)
        return false;
      pixels = scratch_.data();
    }
  }
  if (!pixels) return false;
  context_->UpdateSubresource(upload_.texture.Get(), 0, nullptr, pixels, w, 0);
  m_vRegisteredResources[0] = upload_.registered;
  return true;
}

void NvEncoderD3D11::Finish() {
  if (acquired_) {
    acquired_->ReleaseSync(0);
    acquired_.Reset();
  }
}
}  // namespace minirtc
#endif
