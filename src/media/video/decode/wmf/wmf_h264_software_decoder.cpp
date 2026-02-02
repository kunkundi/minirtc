/*
 * @Author: DI JUNKUN
 * @Date: 2026-02-02
 */

#if defined(_WIN32) || defined(_WIN64)

#include "wmf_h264_software_decoder.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include "log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")

namespace minirtc {

namespace {

using Microsoft::WRL::ComPtr;

static std::atomic<int> g_mf_ref_count{0};

struct MfGuard {
  MfGuard() = default;

  static int EnsureStartup() {
    int prev = g_mf_ref_count.fetch_add(1);
    if (prev == 0) {
      HRESULT hr = MFStartup(MF_VERSION);
      if (FAILED(hr)) {
        g_mf_ref_count.fetch_sub(1);
        LOG_ERROR("MFStartup failed, hr=0x{:08x}", (uint32_t)hr);
        return -1;
      }
    }
    return 0;
  }

  static void Shutdown() {
    int prev = g_mf_ref_count.fetch_sub(1);
    if (prev == 1) {
      MFShutdown();
    }
  }
};

static bool IsAnnexBStartCode3(const uint8_t* p) {
  return p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01;
}

static bool IsAnnexBStartCode4(const uint8_t* p) {
  return p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x00 && p[3] == 0x01;
}

struct NaluView {
  const uint8_t* data = nullptr;  // points at NAL header byte
  size_t size = 0;               // includes NAL header
};

static std::vector<NaluView> SplitAnnexBNalus(const uint8_t* data, size_t size) {
  std::vector<NaluView> nalus;
  if (!data || size < 4) {
    return nalus;
  }

  auto find_start = [&](size_t from) -> size_t {
    for (size_t i = from; i + 3 < size; ++i) {
      if (IsAnnexBStartCode3(data + i) || (i + 4 <= size && IsAnnexBStartCode4(data + i))) {
        return i;
      }
    }
    return size;
  };

  size_t pos = find_start(0);
  while (pos < size) {
    size_t sc_len = 0;
    if (pos + 3 < size && IsAnnexBStartCode3(data + pos)) {
      sc_len = 3;
    } else if (pos + 4 <= size && IsAnnexBStartCode4(data + pos)) {
      sc_len = 4;
    } else {
      break;
    }

    size_t nalu_start = pos + sc_len;
    size_t next = find_start(nalu_start);

    if (nalu_start < size) {
      size_t nalu_end = next;
      if (nalu_end > size) {
        nalu_end = size;
      }
      if (nalu_end > nalu_start) {
        nalus.push_back(NaluView{data + nalu_start, nalu_end - nalu_start});
      }
    }

    pos = next;
  }

  return nalus;
}

static bool ExtractSpsPpsFromAnnexB(const uint8_t* data, size_t size,
                                   std::vector<uint8_t>& sps,
                                   std::vector<uint8_t>& pps) {
  sps.clear();
  pps.clear();

  auto nalus = SplitAnnexBNalus(data, size);
  for (const auto& nalu : nalus) {
    if (!nalu.data || nalu.size < 1) {
      continue;
    }
    uint8_t nal_unit_type = nalu.data[0] & 0x1F;
    if (nal_unit_type == 7 && sps.empty()) {
      sps.assign(nalu.data, nalu.data + nalu.size);
    } else if (nal_unit_type == 8 && pps.empty()) {
      pps.assign(nalu.data, nalu.data + nalu.size);
    }
    if (!sps.empty() && !pps.empty()) {
      return true;
    }
  }
  return false;
}

static std::vector<uint8_t> BuildAvcC(const std::vector<uint8_t>& sps,
                                      const std::vector<uint8_t>& pps) {
  // AVCDecoderConfigurationRecord (ISO/IEC 14496-15)
  // We use 4-byte lengths (lengthSizeMinusOne = 3).
  std::vector<uint8_t> avcc;
  if (sps.size() < 4 || pps.empty()) {
    return avcc;
  }

  avcc.reserve(7 + 2 + sps.size() + 1 + 2 + pps.size());

  avcc.push_back(0x01);                 // configurationVersion
  avcc.push_back(sps[1]);              // AVCProfileIndication
  avcc.push_back(sps[2]);              // profile_compatibility
  avcc.push_back(sps[3]);              // AVCLevelIndication
  avcc.push_back(0xFC | 0x03);         // 6 bits reserved + lengthSizeMinusOne
  avcc.push_back(0xE0 | 0x01);         // 3 bits reserved + numOfSPS

  uint16_t sps_len = (uint16_t)sps.size();
  avcc.push_back((uint8_t)((sps_len >> 8) & 0xFF));
  avcc.push_back((uint8_t)(sps_len & 0xFF));
  avcc.insert(avcc.end(), sps.begin(), sps.end());

  avcc.push_back(0x01);  // numOfPPS
  uint16_t pps_len = (uint16_t)pps.size();
  avcc.push_back((uint8_t)((pps_len >> 8) & 0xFF));
  avcc.push_back((uint8_t)(pps_len & 0xFF));
  avcc.insert(avcc.end(), pps.begin(), pps.end());

  return avcc;
}

static bool IsHardwareMft(IMFActivate* activate) {
  if (!activate) {
    return false;
  }

  UINT32 has_attr = 0;
  if (SUCCEEDED(activate->GetUINT32(MFT_ENUM_HARDWARE_URL_Attribute, &has_attr))) {
    (void)has_attr;
    return true;
  }

  WCHAR* value = nullptr;
  UINT32 value_len = 0;
  HRESULT hr = activate->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute, &value, &value_len);
  if (SUCCEEDED(hr) && value) {
    CoTaskMemFree(value);
    return true;
  }

  return false;
}

static std::string HrToString(HRESULT hr) {
  char buf[32];
  snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)hr);
  return std::string(buf);
}

static bool GetFrameSizeFromMediaType(IMFMediaType* mt, uint32_t& w, uint32_t& h) {
  if (!mt) {
    return false;
  }
  UINT32 width = 0, height = 0;
  HRESULT hr = MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &width, &height);
  if (FAILED(hr) || width == 0 || height == 0) {
    return false;
  }
  w = width;
  h = height;
  return true;
}

}  // namespace

struct WmfH264SoftwareDecoder::Impl {
  explicit Impl(std::shared_ptr<SystemClock> clock) : clock_(clock) {}

  ~Impl() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (decoder_) {
      decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    decoder_.Reset();
    MfGuard::Shutdown();
  }

  int Init() {
    std::lock_guard<std::mutex> lock(mtx_);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
      LOG_ERROR("CoInitializeEx failed, hr={}", HrToString(hr));
      return -1;
    }

    if (MfGuard::EnsureStartup() != 0) {
      return -1;
    }

    return CreateDecoderLocked();
  }

  int Decode(std::unique_ptr<ReceivedFrame> received_frame,
             std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
    if (!received_frame) {
      return -1;
    }

    const uint8_t* data = received_frame->Buffer();
    size_t size = received_frame->Size();
    if (!data || size == 0) {
      return -1;
    }

    std::lock_guard<std::mutex> lock(mtx_);

    if (!decoder_) {
      return -1;
    }

    // Lazily configure sequence header when SPS/PPS become available.
    if (!configured_) {
      std::vector<uint8_t> sps;
      std::vector<uint8_t> pps;
      if (ExtractSpsPpsFromAnnexB(data, size, sps, pps)) {
        avcc_ = BuildAvcC(sps, pps);
        if (!avcc_.empty()) {
          if (ConfigureTypesLocked(received_frame->Width(), received_frame->Height()) != 0) {
            LOG_WARN("WMF ConfigureTypes failed, falling back to OpenH264 may be needed");
            return -1;
          }
        }
      }
    }

    if (!configured_) {
      // Wait for keyframe that carries SPS/PPS.
      return 0;
    }

    if (!started_) {
      decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
      decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
      started_ = true;
    }

    ComPtr<IMFSample> in_sample;
    ComPtr<IMFMediaBuffer> in_buf;

    HRESULT hr = MFCreateSample(&in_sample);
    if (FAILED(hr)) {
      LOG_ERROR("MFCreateSample failed, hr={}", HrToString(hr));
      return -1;
    }

    hr = MFCreateMemoryBuffer((DWORD)size, &in_buf);
    if (FAILED(hr)) {
      LOG_ERROR("MFCreateMemoryBuffer failed, hr={}", HrToString(hr));
      return -1;
    }

    BYTE* dst = nullptr;
    DWORD max_len = 0;
    DWORD cur_len = 0;
    hr = in_buf->Lock(&dst, &max_len, &cur_len);
    if (FAILED(hr) || !dst || max_len < size) {
      LOG_ERROR("IMFMediaBuffer::Lock failed, hr={}", HrToString(hr));
      return -1;
    }

    memcpy(dst, data, size);
    in_buf->Unlock();
    in_buf->SetCurrentLength((DWORD)size);
    in_sample->AddBuffer(in_buf.Get());

    // Timestamps are optional for raw decoding.
    // MF uses 100-nanosecond units.
    LONGLONG sample_time = (LONGLONG)received_frame->CapturedTimestamp() * 10;
    in_sample->SetSampleTime(sample_time);

    hr = decoder_->ProcessInput(0, in_sample.Get(), 0);
    if (FAILED(hr)) {
      if (hr == MF_E_NOTACCEPTING) {
        // Need to drain output.
      } else {
        LOG_WARN("ProcessInput failed, hr={}", HrToString(hr));
        return -1;
      }
    }

    DrainOutputLocked(received_frame.get(), on_receive_decoded_frame);
    return 0;
  }

 private:
  int CreateDecoderLocked() {
    configured_ = false;
    started_ = false;
    decoder_.Reset();

    MFT_REGISTER_TYPE_INFO input_info{};
    input_info.guidMajorType = MFMediaType_Video;
    input_info.guidSubtype = MFVideoFormat_H264;

    MFT_REGISTER_TYPE_INFO output_info{};
    output_info.guidMajorType = MFMediaType_Video;
    output_info.guidSubtype = MFVideoFormat_NV12;

    IMFActivate** activates = nullptr;
    UINT32 activate_count = 0;

    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT;

    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &input_info,
                           &output_info, &activates, &activate_count);
    if (FAILED(hr) || activate_count == 0) {
      LOG_ERROR("MFTEnumEx failed/no decoder, hr={} count={}", HrToString(hr), (uint32_t)activate_count);
      return -1;
    }

    ComPtr<IMFTransform> chosen;
    for (UINT32 i = 0; i < activate_count; ++i) {
      IMFActivate* act = activates[i];
      if (!act) {
        continue;
      }

      if (!chosen && !IsHardwareMft(act)) {
        ComPtr<IMFTransform> t;
        hr = act->ActivateObject(IID_PPV_ARGS(&t));
        if (SUCCEEDED(hr) && t) {
          chosen = t;
        }
      }

      act->Release();
      activates[i] = nullptr;
    }

    CoTaskMemFree(activates);

    if (!chosen) {
      LOG_ERROR("No software H264 MFT found");
      return -1;
    }

    decoder_ = chosen;

    // Best-effort: tell MFT we won't use D3D.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(decoder_->GetAttributes(&attrs)) && attrs) {
      attrs->SetUINT32(MF_SA_D3D11_AWARE, 0);
      attrs->SetUINT32(MF_SA_D3D_AWARE, 0);
    }

    return 0;
  }

  int ConfigureTypesLocked(uint32_t hint_w, uint32_t hint_h) {
    if (!decoder_) {
      return -1;
    }

    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);

    ComPtr<IMFMediaType> in_type;
    HRESULT hr = MFCreateMediaType(&in_type);
    if (FAILED(hr)) {
      return -1;
    }

    in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    if (!avcc_.empty()) {
      hr = in_type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, avcc_.data(), (UINT32)avcc_.size());
      if (FAILED(hr)) {
        LOG_WARN("Set MF_MT_MPEG_SEQUENCE_HEADER failed, hr={}", HrToString(hr));
      }
    }

    uint32_t w = hint_w ? hint_w : MINIRTC_INIT_WIDTH;
    uint32_t h = hint_h ? hint_h : MINIRTC_INIT_HEIGHT;
    MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, w, h);

    hr = decoder_->SetInputType(0, in_type.Get(), 0);
    if (FAILED(hr)) {
      LOG_ERROR("SetInputType failed, hr={}", HrToString(hr));
      return -1;
    }

    // Select NV12 output.
    ComPtr<IMFMediaType> out_type;
    for (DWORD i = 0;; ++i) {
      ComPtr<IMFMediaType> t;
      hr = decoder_->GetOutputAvailableType(0, i, &t);
      if (hr == MF_E_NO_MORE_TYPES) {
        break;
      }
      if (FAILED(hr) || !t) {
        continue;
      }

      GUID subtype{};
      if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        continue;
      }
      if (subtype == MFVideoFormat_NV12) {
        out_type = t;
        break;
      }
    }

    if (!out_type) {
      LOG_ERROR("WMF decoder has no NV12 output type");
      return -1;
    }

    hr = decoder_->SetOutputType(0, out_type.Get(), 0);
    if (FAILED(hr)) {
      LOG_ERROR("SetOutputType failed, hr={}", HrToString(hr));
      return -1;
    }

    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    started_ = true;

    configured_ = true;
    return 0;
  }

  void DrainOutputLocked(ReceivedFrame* received_frame,
                         const std::function<void(const DecodedFrame*)>& cb) {
    if (!decoder_ || !configured_) {
      return;
    }

    MFT_OUTPUT_STREAM_INFO stream_info{};
    HRESULT hr = decoder_->GetOutputStreamInfo(0, &stream_info);
    if (FAILED(hr)) {
      return;
    }

    while (true) {
      MFT_OUTPUT_DATA_BUFFER out{};
      DWORD status = 0;

      ComPtr<IMFSample> out_sample;
      ComPtr<IMFMediaBuffer> out_buf;

      if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        hr = MFCreateSample(&out_sample);
        if (FAILED(hr)) {
          return;
        }

        DWORD buf_size = stream_info.cbSize;
        if (buf_size == 0) {
          buf_size = 1920 * 1080 * 3 / 2;
        }

        hr = MFCreateMemoryBuffer(buf_size, &out_buf);
        if (FAILED(hr)) {
          return;
        }
        out_sample->AddBuffer(out_buf.Get());
        out.pSample = out_sample.Get();
      }

      out.dwStreamID = 0;

      hr = decoder_->ProcessOutput(0, 1, &out, &status);
      if (out.pEvents) {
        out.pEvents->Release();
        out.pEvents = nullptr;
      }

      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        break;
      }

      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // Refresh output type.
        ComPtr<IMFMediaType> new_type;
        if (SUCCEEDED(decoder_->GetOutputAvailableType(0, 0, &new_type)) && new_type) {
          decoder_->SetOutputType(0, new_type.Get(), 0);
        }
        continue;
      }

      if (FAILED(hr)) {
        break;
      }

      if (!out.pSample) {
        break;
      }

      // Extract decoded buffer.
      ComPtr<IMFMediaBuffer> buf;
      if (FAILED(out.pSample->ConvertToContiguousBuffer(&buf)) || !buf) {
        break;
      }

      BYTE* p = nullptr;
      DWORD max_len = 0;
      DWORD cur_len = 0;
      if (FAILED(buf->Lock(&p, &max_len, &cur_len)) || !p || cur_len == 0) {
        break;
      }

      uint32_t decoded_w = 0;
      uint32_t decoded_h = 0;
      {
        ComPtr<IMFMediaType> cur_type;
        if (SUCCEEDED(decoder_->GetOutputCurrentType(0, &cur_type)) && cur_type) {
          GetFrameSizeFromMediaType(cur_type.Get(), decoded_w, decoded_h);
        }
      }

      if (decoded_w == 0 || decoded_h == 0) {
        decoded_w = received_frame->Width() ? received_frame->Width() : MINIRTC_INIT_WIDTH;
        decoded_h = received_frame->Height() ? received_frame->Height() : MINIRTC_INIT_HEIGHT;
      }

      size_t expected = (size_t)decoded_w * (size_t)decoded_h * 3 / 2;
      size_t copy_len = std::min<size_t>(cur_len, expected);

      if (nv12_frame_.size() < copy_len) {
        nv12_frame_.resize(copy_len);
      }
      memcpy(nv12_frame_.data(), p, copy_len);
      buf->Unlock();

      if (!decoded_frame_) {
        decoded_frame_ = std::make_unique<DecodedFrame>(nv12_frame_.data(), copy_len, decoded_w, decoded_h);
      }

      decoded_frame_->UpdateBuffer(nv12_frame_.data(), nv12_frame_.size());
      decoded_frame_->SetWidth(received_frame->Width());
      decoded_frame_->SetHeight(received_frame->Height());
      decoded_frame_->SetDecodedWidth(decoded_w);
      decoded_frame_->SetDecodedHeight(decoded_h);
      decoded_frame_->SetReceivedTimestamp(received_frame->ReceivedTimestamp());
      decoded_frame_->SetCapturedTimestamp(received_frame->CapturedTimestamp());
      decoded_frame_->SetDecodedTimestamp(clock_->CurrentTime());

      if (cb) {
        cb(decoded_frame_.get());
      }
    }
  }

 private:
  std::mutex mtx_;
  std::shared_ptr<SystemClock> clock_;

  ComPtr<IMFTransform> decoder_;

  bool configured_ = false;
  bool started_ = false;

  std::vector<uint8_t> avcc_;
  std::vector<uint8_t> nv12_frame_;
  std::unique_ptr<DecodedFrame> decoded_frame_;
};

WmfH264SoftwareDecoder::WmfH264SoftwareDecoder(std::shared_ptr<SystemClock> clock)
    : impl_(std::make_unique<Impl>(clock)) {}

WmfH264SoftwareDecoder::~WmfH264SoftwareDecoder() = default;

int WmfH264SoftwareDecoder::Init() {
  return impl_->Init();
}

int WmfH264SoftwareDecoder::Decode(
    std::unique_ptr<ReceivedFrame> received_frame,
    std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
  return impl_->Decode(std::move(received_frame), on_receive_decoded_frame);
}

}  // namespace minirtc

#endif  // _WIN32 || _WIN64
