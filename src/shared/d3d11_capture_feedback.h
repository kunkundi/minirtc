/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-28
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _D3D11_CAPTURE_FEEDBACK_H_
#define _D3D11_CAPTURE_FEEDBACK_H_

#include <d3d11.h>
#include <wrl/client.h>

namespace minirtc {
// Optional, in-process feedback attached to the capture device. A producer
// can stop converting to NV12 after failed probes without penalizing viewers
// that are still consuming textures. No change to the native frame ABI.
struct __declspec(uuid("ab0cbbce-ae95-4553-940b-3dc390e24fd8"))
D3D11CaptureFeedback : IUnknown {
  virtual void STDMETHODCALLTYPE EncoderStarted() = 0;
  virtual void STDMETHODCALLTYPE EncoderStopped() = 0;
  virtual void STDMETHODCALLTYPE RequestCpuFrames() = 0;

  static Microsoft::WRL::ComPtr<D3D11CaptureFeedback> Find(ID3D11Device* device) {
    Microsoft::WRL::ComPtr<IUnknown> object;
    UINT size = sizeof(IUnknown*);
    Microsoft::WRL::ComPtr<D3D11CaptureFeedback> feedback;
    if (SUCCEEDED(device->GetPrivateData(__uuidof(D3D11CaptureFeedback), &size,
                                         object.GetAddressOf())) && object)
      object.As(&feedback);
    return feedback;
  }
};
}  // namespace minirtc
#endif
