/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _NV_ENCODER_D3D11_H_
#define _NV_ENCODER_D3D11_H_

#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <vector>

#include "NvEncoder.h"
#include "raw_frame.h"

namespace minirtc {

class NvEncoderD3D11 final : public NvEncoder {
 public:
  static std::unique_ptr<NvEncoderD3D11> Create(uint32_t width,
                                                uint32_t height);
  bool Prepare(const RawFrame& frame);
  void Finish();
  bool Fits(uint32_t width, uint32_t height) const {
    return width <= GetMaxEncodeWidth() && height <= GetMaxEncodeHeight();
  }

 private:
  NvEncoderD3D11(ID3D11Device* device, uint32_t width, uint32_t height);
  void AllocateInputBuffers(int32_t count) override;
  void ReleaseInputBuffers() override;

  struct Input {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source, texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
    NV_ENC_REGISTERED_PTR registered = nullptr;
  };
  void Clear(Input& input);
  Input* OpenShared(const MiniRtcNativeVideoFrame& frame);
  bool Upload(const RawFrame& frame);
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGIKeyedMutex> acquired_;
  std::array<Input, 3> shared_;
  size_t next_ = 0;
  Input upload_;
  std::vector<uint8_t> scratch_;
  bool shared_logged_ = false;
};

}  // namespace minirtc
#endif
#endif
