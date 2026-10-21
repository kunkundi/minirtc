/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-22
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _WMF_UTILS_H_
#define _WMF_UTILS_H_

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace minirtc::wmf {
using Microsoft::WRL::ComPtr;

// All COM objects are created, used and released in the same MTA. Callbacks
// remain on the caller's thread, outside this worker and its internal locks.
class CodecThread {
 public:
  CodecThread();
  ~CodecThread();
  CodecThread(const CodecThread&) = delete;
  CodecThread& operator=(const CodecThread&) = delete;

  HRESULT Status() const { return status_; }

  template <typename F>
  auto Call(F&& function) -> std::invoke_result_t<F> {
    using Result = std::invoke_result_t<F>;
    auto task = std::make_shared<std::packaged_task<Result()>>(
        std::forward<F>(function));
    auto result = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push([task] { (*task)(); });
    }
    ready_.notify_one();
    return result.get();
  }

 private:
  HRESULT status_ = E_FAIL;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::queue<std::function<void()>> tasks_;
  bool stopping_ = false;
  std::thread thread_;
};

HRESULT CreateSample(const uint8_t* data, size_t size, LONGLONG time,
                     LONGLONG duration, ComPtr<IMFSample>& sample);
HRESULT ReadOutput(IMFTransform* transform, DWORD stream_id,
                   ComPtr<IMFSample>& sample);
HRESULT ReadBytes(IMFSample* sample, std::vector<uint8_t>& bytes);
void ShutdownTransform(ComPtr<IMFTransform>& transform);

struct Nalu {
  size_t offset;
  size_t size;
  uint8_t type;
};
std::vector<Nalu> AnnexBNalus(const uint8_t* data, size_t size);
bool HasNalu(const uint8_t* data, size_t size, uint8_t type);

}  // namespace minirtc::wmf
#endif
