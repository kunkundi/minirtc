/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-25
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _D3D11_VIDEO_DECODER_H_
#define _D3D11_VIDEO_DECODER_H_

#include "media_codec.h"

namespace minirtc {

// H.264/HEVC MFT with an IMFDXGIDeviceManager. Decoded D3D11 surfaces are
// copied to NV12 for the existing renderer; no vendor decoder SDK is used.
class D3D11VideoDecoder final : public MediaCodec {
 public:
  D3D11VideoDecoder(std::shared_ptr<SystemClock> clock,
                    bool native_video_output, VideoCodecType codec);
  ~D3D11VideoDecoder() override;
  static bool IsSupported(VideoCodecType codec);
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
