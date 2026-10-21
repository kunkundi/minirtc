#include "wmf_utils.h"

#include <cstring>
#include <limits>

namespace minirtc::wmf {

CodecThread::CodecThread() {
  std::promise<HRESULT> started;
  auto result = started.get_future();
  thread_ = std::thread([this, &started] {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const HRESULT mf = SUCCEEDED(com) ? MFStartup(MF_VERSION) : com;
    started.set_value(mf);
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty() && stopping_) break;
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      task();
    }
    if (SUCCEEDED(mf)) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
  });
  status_ = result.get();
}

CodecThread::~CodecThread() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  ready_.notify_one();
  thread_.join();
}

HRESULT CreateSample(const uint8_t* data, size_t size, LONGLONG time,
                     LONGLONG duration, ComPtr<IMFSample>& sample) {
  sample.Reset();
  if (!data || !size || size > std::numeric_limits<DWORD>::max())
    return E_INVALIDARG;
  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
  if (FAILED(hr)) return hr;
  BYTE* destination = nullptr;
  hr = buffer->Lock(&destination, nullptr, nullptr);
  if (FAILED(hr)) return hr;
  std::memcpy(destination, data, size);
  buffer->Unlock();
  hr = buffer->SetCurrentLength(static_cast<DWORD>(size));
  if (FAILED(hr)) return hr;
  hr = MFCreateSample(&sample);
  if (FAILED(hr)) return hr;
  hr = sample->AddBuffer(buffer.Get());
  if (FAILED(hr)) return hr;
  hr = sample->SetSampleTime(time);
  if (FAILED(hr)) return hr;
  return sample->SetSampleDuration(duration);
}

HRESULT ReadOutput(IMFTransform* transform, DWORD stream_id,
                   ComPtr<IMFSample>& sample) {
  sample.Reset();
  MFT_OUTPUT_STREAM_INFO info{};
  HRESULT hr = transform->GetOutputStreamInfo(stream_id, &info);
  if (FAILED(hr)) return hr;
  ComPtr<IMFSample> supplied;
  if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateSample(&supplied);
    if (FAILED(hr)) return hr;
    hr = MFCreateAlignedMemoryBuffer(
        info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, &buffer);
    if (FAILED(hr)) return hr;
    hr = supplied->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;
  }
  MFT_OUTPUT_DATA_BUFFER output{};
  output.dwStreamID = stream_id;
  output.pSample = supplied.Get();
  DWORD status = 0;
  hr = transform->ProcessOutput(0, 1, &output, &status);
  if (output.pEvents) output.pEvents->Release();
  // A sample supplied by the caller is not AddRef'd by ProcessOutput.
  if (output.pSample == supplied.Get())
    sample = supplied;
  else
    sample.Attach(output.pSample);
  return hr;
}

HRESULT ReadBytes(IMFSample* sample, std::vector<uint8_t>& bytes) {
  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(hr)) return hr;
  BYTE* data = nullptr;
  DWORD size = 0;
  hr = buffer->Lock(&data, nullptr, &size);
  if (FAILED(hr)) return hr;
  bytes.assign(data, data + size);
  buffer->Unlock();
  return S_OK;
}

void ShutdownTransform(ComPtr<IMFTransform>& transform) {
  if (!transform) return;
  transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  ComPtr<IMFShutdown> shutdown;
  if (SUCCEEDED(transform.As(&shutdown))) shutdown->Shutdown();
  transform.Reset();
}

std::vector<Nalu> AnnexBNalus(const uint8_t* data, size_t size) {
  std::vector<Nalu> result;
  if (!data) return result;
  size_t payload = size;
  for (size_t i = 0; i + 2 < size;) {
    size_t prefix = 0;
    if (data[i] == 0 && data[i + 1] == 0) {
      if (data[i + 2] == 1)
        prefix = 3;
      else if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1)
        prefix = 4;
    }
    if (!prefix) {
      ++i;
      continue;
    }
    if (payload < i)
      result.push_back(
          {payload, i - payload, static_cast<uint8_t>(data[payload] & 0x1f)});
    payload = i + prefix;
    i = payload;
  }
  if (payload < size)
    result.push_back(
        {payload, size - payload, static_cast<uint8_t>(data[payload] & 0x1f)});
  return result;
}

bool HasNalu(const uint8_t* data, size_t size, uint8_t type) {
  for (const auto& nalu : AnnexBNalus(data, size))
    if (nalu.type == type) return true;
  return false;
}
}  // namespace minirtc::wmf
