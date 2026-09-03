#include "nvidia_video_decoder.h"

#include <atomic>
#include <new>
#include <utility>

#include "log.h"
#include "minirtc.h"

// #define SAVE_DECODED_NV12_STREAM
// #define SAVE_RECEIVED_H264_STREAM

#include "bitstream_parser.h"

namespace minirtc {

struct NvidiaVideoDecoderState {
  ~NvidiaVideoDecoderState() {
    decoder.reset();
    if (cuda_context) {
      cuCtxDestroy_ld(cuda_context);
      cuda_context = nullptr;
    }
  }

  CUcontext cuda_context = nullptr;
  std::unique_ptr<NvDecoder> decoder;
};

namespace {

class NvidiaNativeNv12Frame final {
 public:
  static NvidiaNativeNv12Frame* Create(
      std::shared_ptr<NvidiaVideoDecoderState> state, uint8_t* frame,
      uint32_t width, uint32_t height, uint32_t pitch) {
    if (!state || !state->decoder || !frame || width == 0 || height == 0 ||
        pitch < width) {
      return nullptr;
    }
    return new (std::nothrow) NvidiaNativeNv12Frame(
        std::move(state), frame, width, height, pitch);
  }

  XNativeVideoFrame* Descriptor() { return &descriptor_; }

  void AddRef() { references_.fetch_add(1, std::memory_order_relaxed); }

  void Release() {
    if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }

 private:
  NvidiaNativeNv12Frame(std::shared_ptr<NvidiaVideoDecoderState> state,
                         uint8_t* frame, uint32_t width, uint32_t height,
                         uint32_t pitch)
      : state_(std::move(state)), frame_(frame) {
    descriptor_.struct_size = sizeof(descriptor_);
    descriptor_.type = XNativeVideoFrameCudaNv12;
    descriptor_.width = width;
    descriptor_.height = height;
    descriptor_.payload.cuda_nv12 = {
        reinterpret_cast<uint64_t>(frame_),
        reinterpret_cast<uint64_t>(frame_) +
            static_cast<uint64_t>(pitch) * height,
        pitch, pitch, state_->cuda_context};
    descriptor_.owner = this;
    descriptor_.retain = &RetainOwner;
    descriptor_.release = &ReleaseOwner;
    descriptor_.copy_to_nv12 = &CopyToNv12;
  }

  ~NvidiaNativeNv12Frame() {
    if (state_ && state_->decoder && frame_) {
      state_->decoder->UnlockFrame(&frame_);
    }
  }

  static void RetainOwner(void* owner) {
    static_cast<NvidiaNativeNv12Frame*>(owner)->AddRef();
  }

  static void ReleaseOwner(void* owner) {
    static_cast<NvidiaNativeNv12Frame*>(owner)->Release();
  }

  static int CopyToNv12(void* owner, uint8_t* destination,
                        size_t destination_size) {
    auto* native = static_cast<NvidiaNativeNv12Frame*>(owner);
    if (!native || !destination ||
        destination_size < static_cast<size_t>(native->descriptor_.width) *
                               native->descriptor_.height * 3U / 2U ||
        !native->state_ ||
        !native->state_->cuda_context || !native->frame_) {
      return -1;
    }

    if (cuCtxPushCurrent_ld(native->state_->cuda_context) != CUDA_SUCCESS) {
      return -1;
    }

    CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice =
        static_cast<CUdeviceptr>(
            native->descriptor_.payload.cuda_nv12.y_device_pointer);
    copy.srcPitch = native->descriptor_.payload.cuda_nv12.y_stride;
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = destination;
    copy.dstPitch = native->descriptor_.width;
    copy.WidthInBytes = native->descriptor_.width;
    copy.Height = native->descriptor_.height;
    CUresult result = cuMemcpy2D_ld(&copy);
    if (result == CUDA_SUCCESS) {
      copy.srcDevice =
          static_cast<CUdeviceptr>(
              native->descriptor_.payload.cuda_nv12.uv_device_pointer);
      copy.srcPitch = native->descriptor_.payload.cuda_nv12.uv_stride;
      copy.dstHost = destination +
                     static_cast<size_t>(native->descriptor_.width) *
                         native->descriptor_.height;
      copy.Height = native->descriptor_.height / 2U;
      result = cuMemcpy2D_ld(&copy);
    }
    CUcontext popped_context = nullptr;
    const CUresult pop_result = cuCtxPopCurrent_ld(&popped_context);
    return result == CUDA_SUCCESS && pop_result == CUDA_SUCCESS ? 0 : -1;
  }

  std::atomic<uint32_t> references_{1};
  std::shared_ptr<NvidiaVideoDecoderState> state_;
  uint8_t* frame_ = nullptr;
  XNativeVideoFrame descriptor_{};
};

}  // namespace

NvidiaVideoDecoder::NvidiaVideoDecoder(std::shared_ptr<SystemClock> clock,
                                       bool native_video_output)
    : clock_(std::move(clock)),
      native_video_output_(native_video_output) {}
NvidiaVideoDecoder::~NvidiaVideoDecoder() {
#ifdef SAVE_DECODED_NV12_STREAM
  if (file_nv12_) {
    fflush(file_nv12_);
    fclose(file_nv12_);
    file_nv12_ = nullptr;
  }
#endif

#ifdef SAVE_RECEIVED_H264_STREAM
  if (file_h264_) {
    fflush(file_h264_);
    fclose(file_h264_);
    file_h264_ = nullptr;
  }
#endif

  state_.reset();

  if (decoded_frame_) {
    delete decoded_frame_;
  }
}

int NvidiaVideoDecoder::Init() {
  CudaInitializer::Init();
  int nGpu = 0;
  int iGpu = 0;

  ck(cuDeviceGetCount_ld(&nGpu));
  if (nGpu < 1) {
    return -1;
  }

  cuDeviceGet_ld(&cuda_device_, iGpu);

  state_ = std::make_shared<NvidiaVideoDecoderState>();
  cuCtxCreate_ld(&state_->cuda_context, 0, cuda_device_);
  if (!state_->cuda_context) {
    state_.reset();
    return -1;
  }

  state_->decoder = std::make_unique<NvDecoder>(
      state_->cuda_context, native_video_output_, cudaVideoCodec_H264, true,
      false, nullptr, nullptr, 4096, 4096, 1000, false);

  if (!decoded_frame_ && !native_video_output_) {
    decoded_frame_ = new DecodedFrame(frame_width_ * frame_height_ * 3 / 2,
                                      frame_width_, frame_height_);
  }
  if (native_video_output_) {
    LOG_INFO("NVIDIA Windows native CUDA NV12 output enabled");
  }

#ifdef SAVE_DECODED_NV12_STREAM
  nv12_file_name_ = "decoded_nv12_stream_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yuv";
  file_nv12_ = fopen(nv12_file_name_.c_str(), "w+b");
  if (!file_nv12_) {
    LOG_WARN("Fail to open {}", nv12_file_name_.c_str());
  }
#endif

#ifdef SAVE_RECEIVED_H264_STREAM
  h264_file_name_ = "received_h264_stream_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".h264";
  file_h264_ = fopen(h264_file_name_.c_str(), "w+b");
  if (!file_h264_) {
    LOG_WARN("Fail to open {}", h264_file_name_.c_str());
  }
#endif

  return 0;
}

int NvidiaVideoDecoder::Decode(
    std::unique_ptr<ReceivedFrame> received_frame,
    std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
  if (!state_ || !state_->decoder) {
    return -1;
  }

  const uint8_t* data = received_frame->Buffer();
  size_t size = received_frame->Size();

#ifdef SAVE_RECEIVED_H264_STREAM
  fwrite((unsigned char*)data, 1, size, file_h264_);
#endif

  int num_frame_returned = state_->decoder->Decode(data, (int)size);
  for (size_t i = 0; i < num_frame_returned; ++i) {
    cudaVideoSurfaceFormat format = state_->decoder->GetOutputFormat();
    if (format == cudaVideoSurfaceFormat_NV12) {
      uint8_t* decoded_frame_buffer = nullptr;
      decoded_frame_buffer = native_video_output_
                                 ? state_->decoder->GetLockedFrame()
                                 : state_->decoder->GetFrame();
      frame_width_ = state_->decoder->GetWidth();
      frame_height_ = state_->decoder->GetHeight();
      if (decoded_frame_buffer) {
        if (on_receive_decoded_frame) {
          if (native_video_output_) {
            auto* native_frame = NvidiaNativeNv12Frame::Create(
                state_, decoded_frame_buffer, frame_width_, frame_height_,
                state_->decoder->GetDeviceFramePitch());
            if (!native_frame) {
              state_->decoder->UnlockFrame(&decoded_frame_buffer);
              LOG_ERROR("Failed to wrap native NVIDIA NV12 frame");
              return -1;
            }

            DecodedFrame native_decoded_frame;
            native_decoded_frame.SetSize(
                static_cast<size_t>(frame_width_) * frame_height_ * 3U / 2U);
            native_decoded_frame.SetWidth(frame_width_);
            native_decoded_frame.SetHeight(frame_height_);
            native_decoded_frame.SetDecodedWidth(frame_width_);
            native_decoded_frame.SetDecodedHeight(frame_height_);
            native_decoded_frame.SetReceivedTimestamp(
                received_frame->ReceivedTimestamp());
            native_decoded_frame.SetCapturedTimestamp(
                received_frame->CapturedTimestamp());
            native_decoded_frame.SetDecodedTimestamp(clock_->CurrentTime());
            native_decoded_frame.SetNativeFrame(native_frame->Descriptor());
            on_receive_decoded_frame(&native_decoded_frame);
            native_frame->Release();
            continue;
          }

          decoded_frame_->UpdateBuffer(decoded_frame_buffer,
                                       frame_width_ * frame_height_ * 3 / 2);
          decoded_frame_->SetWidth(frame_width_);
          decoded_frame_->SetHeight(frame_height_);
          decoded_frame_->SetDecodedWidth(frame_width_);
          decoded_frame_->SetDecodedHeight(frame_height_);
          decoded_frame_->SetReceivedTimestamp(
              received_frame->ReceivedTimestamp());
          decoded_frame_->SetCapturedTimestamp(
              received_frame->CapturedTimestamp());
          decoded_frame_->SetDecodedTimestamp(clock_->CurrentTime());
#ifdef SAVE_DECODED_NV12_STREAM
          fwrite((unsigned char*)decoded_frame_->Buffer(), 1,
                 decoded_frame_->Size(), file_nv12_);
#endif
          on_receive_decoded_frame(decoded_frame_);
        } else if (native_video_output_) {
          state_->decoder->UnlockFrame(&decoded_frame_buffer);
        }
      }
    }
  }

  return 0;
}
}  // namespace minirtc
