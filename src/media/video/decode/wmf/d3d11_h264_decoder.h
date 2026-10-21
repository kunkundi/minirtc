/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-22
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _D3D11_H264_DECODER_H_
#define _D3D11_H264_DECODER_H_

#include "media_codec.h"

namespace minirtc {

// Microsoft H.264 MFT with an IMFDXGIDeviceManager. Decoded D3D11 surfaces are
// copied to NV12 for the existing renderer; no vendor decoder SDK is used.
class D3D11H264Decoder final : public MediaCodec {
 public:
  explicit D3D11H264Decoder(std::shared_ptr<SystemClock> clock,
                            bool native_video_output = false);
  ~D3D11H264Decoder() override;
  static bool IsSupported();
  int Init() override;
  int Decode(std::unique_ptr<ReceivedFrame> frame,
             std::function<void(const DecodedFrame*)> callback) override;
  std::string GetDecoderName() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace minirtc
#endif
