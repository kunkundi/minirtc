/*
 * @Author: DI JUNKUN
 * @Date: 2026-02-02
 */

#ifndef _WMF_H264_SOFTWARE_DECODER_H_
#define _WMF_H264_SOFTWARE_DECODER_H_

#if defined(_WIN32) || defined(_WIN64)

#include <Windows.h>
#include <mfidl.h>
#include <wrl/client.h>

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
  WmfH264SoftwareDecoder(std::shared_ptr<SystemClock> clock);
  virtual ~WmfH264SoftwareDecoder() override;

  // Non-copyable, non-movable
  WmfH264SoftwareDecoder(const WmfH264SoftwareDecoder&) = delete;
  WmfH264SoftwareDecoder& operator=(const WmfH264SoftwareDecoder&) = delete;
  WmfH264SoftwareDecoder(WmfH264SoftwareDecoder&&) = delete;
  WmfH264SoftwareDecoder& operator=(WmfH264SoftwareDecoder&&) = delete;

  int Init() override;

  int Decode(std::unique_ptr<ReceivedFrame> received_frame,
             std::function<void(const DecodedFrame*)> on_receive_decoded_frame)
      override;

  std::string GetDecoderName() const override { return "WMF(H264/SW)"; }

 private:
  int CreateDecoder();
  int ConfigureTypes(uint32_t hint_w, uint32_t hint_h);
  void DrainOutput(ReceivedFrame* received_frame,
                   const std::function<void(const DecodedFrame*)>& cb);

 private:
  std::mutex mtx_;
  std::shared_ptr<SystemClock> clock_;

  Microsoft::WRL::ComPtr<IMFTransform> decoder_;

  bool configured_ = false;
  bool started_ = false;

  std::vector<uint8_t> avcc_;
  std::vector<uint8_t> nv12_frame_;
  std::unique_ptr<DecodedFrame> decoded_frame_;

  bool mf_started_ = false;
  bool com_inited_ = false;
};

}  // namespace minirtc

#endif  // _WIN32 || _WIN64

#endif
