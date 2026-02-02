/*
 * @Author: DI JUNKUN
 * @Date: 2026-02-02
 */

#ifndef _WMF_H264_SOFTWARE_DECODER_H_
#define _WMF_H264_SOFTWARE_DECODER_H_

#if defined(_WIN32) || defined(_WIN64)

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media_codec.h"

namespace minirtc {

// Windows Media Foundation software H.264 decoder.
//
// Notes:
// - This decoder is intended as a software fallback (no DXVA/D3D manager).
// - Input is expected to be Annex-B H.264 bytestream (00 00 00 01 / 00 00 01).
// - Output is NV12 to match the rest of the pipeline.
class WmfH264SoftwareDecoder : public MediaCodec {
 public:
  explicit WmfH264SoftwareDecoder(std::shared_ptr<SystemClock> clock);
  ~WmfH264SoftwareDecoder() override;

  int Init() override;

  int Decode(std::unique_ptr<ReceivedFrame> received_frame,
             std::function<void(const DecodedFrame*)> on_receive_decoded_frame)
      override;

  std::string GetDecoderName() const override { return "WMF(H264/SW)"; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace minirtc

#endif  // _WIN32 || _WIN64

#endif
