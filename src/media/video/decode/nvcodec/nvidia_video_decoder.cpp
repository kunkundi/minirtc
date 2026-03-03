#include "nvidia_video_decoder.h"

#include "log.h"

// #define SAVE_DECODED_NV12_STREAM
// #define SAVE_RECEIVED_H264_STREAM

#include "bitstream_parser.h"

namespace minirtc {

NvidiaVideoDecoder::NvidiaVideoDecoder(std::shared_ptr<SystemClock> clock)
    : clock_(clock) {}
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

  if (decoder_) {
    delete decoder_;
    decoder_ = nullptr;
  }

  if (cuda_context_) {
    cuCtxDestroy_ld(cuda_context_);
    cuda_context_ = nullptr;
  }

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

  cuCtxCreate_ld(&cuda_context_, 0, cuda_device_);
  if (!cuda_context_) {
    return -1;
  }

  decoder_ = new NvDecoder(cuda_context_, false, cudaVideoCodec_H264, true,
                           false, nullptr, nullptr, 4096, 4096, 1000, false);

  if (!decoded_frame_) {
    decoded_frame_ = new DecodedFrame(frame_width_ * frame_height_ * 3 / 2,
                                      frame_width_, frame_height_);
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
  if (!decoder_) {
    return -1;
  }

  const uint8_t* data = received_frame->Buffer();
  size_t size = received_frame->Size();

#ifdef SAVE_RECEIVED_H264_STREAM
  fwrite((unsigned char*)data, 1, size, file_h264_);
#endif

  int num_frame_returned = decoder_->Decode(data, (int)size);
  for (size_t i = 0; i < num_frame_returned; ++i) {
    cudaVideoSurfaceFormat format = decoder_->GetOutputFormat();
    if (format == cudaVideoSurfaceFormat_NV12) {
      uint8_t* decoded_frame_buffer = nullptr;
      decoded_frame_buffer = decoder_->GetFrame();
      frame_width_ = decoder_->GetWidth();
      frame_height_ = decoder_->GetHeight();
      if (decoded_frame_buffer) {
        if (on_receive_decoded_frame) {
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
        }
      }
      decoder_->UnlockFrame(&decoded_frame_buffer);
    }
  }

  return 0;
}
}  // namespace minirtc