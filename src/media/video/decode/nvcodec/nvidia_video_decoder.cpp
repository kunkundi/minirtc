#include "nvidia_video_decoder.h"

#include "log.h"

// #define SAVE_DECODED_NV12_STREAM
// #define SAVE_RECEIVED_H264_STREAM

NvidiaVideoDecoder::NvidiaVideoDecoder(std::shared_ptr<SystemClock> clock)
    : clock_(clock) {}
NvidiaVideoDecoder::~NvidiaVideoDecoder() {
  if (decoded_frame_) {
    delete decoded_frame_;
  }

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

  if (cuda_context_) {
    cuCtxDestroy(cuda_context_);
    cuda_context_ = nullptr;
  }
}

int NvidiaVideoDecoder::Init() {
  ck(cuInit(0));
  int nGpu = 0;
  int iGpu = 0;

  ck(cuDeviceGetCount(&nGpu));
  if (nGpu < 1) {
    return -1;
  }

  cuDeviceGet(&cuda_device_, iGpu);

  cuCtxCreate(&cuda_context_, 0, cuda_device_);
  if (!cuda_context_) {
    return -1;
  }

  decoder = new NvDecoder(cuda_context_, false, cudaVideoCodec_H264, true,
                          false, nullptr, nullptr, 4096, 2160, 1000, false);

  if (!decoded_frame_) {
    decoded_frame_ = new DecodedFrame(frame_width_ * frame_height_ * 3 / 2,
                                      frame_width_, frame_height_);
  }

#ifdef SAVE_DECODED_NV12_STREAM
  file_nv12_ = fopen("decoded_nv12_stream.yuv", "w+b");
  if (!file_nv12_) {
    LOG_WARN("Fail to open decoded_nv12_stream.yuv");
  }
#endif

#ifdef SAVE_RECEIVED_H264_STREAM
  file_h264_ = fopen("received_h264_stream.h264", "w+b");
  if (!file_h264_) {
    LOG_WARN("Fail to open received_h264_stream.h264");
  }
#endif

  return 0;
}

int NvidiaVideoDecoder::Decode(
    std::unique_ptr<ReceivedFrame> received_frame,
    std::function<void(const DecodedFrame *)> on_receive_decoded_frame) {
  if (!decoder) {
    return -1;
  }

  const uint8_t *data = received_frame->Buffer();
  size_t size = received_frame->Size();

#ifdef SAVE_RECEIVED_H264_STREAM
  fwrite((unsigned char *)data, 1, size, file_h264_);
#endif

  if ((*(data + 4) & 0x1f) == 0x07) {
    LOG_INFO("Receive key frame");
  }

  int num_frame_returned = decoder->Decode(data, (int)size);
  for (size_t i = 0; i < num_frame_returned; ++i) {
    cudaVideoSurfaceFormat format = decoder->GetOutputFormat();
    if (format == cudaVideoSurfaceFormat_NV12) {
      uint8_t *decoded_frame_buffer = nullptr;
      decoded_frame_buffer = decoder->GetFrame();
      if (decoded_frame_buffer) {
        if (on_receive_decoded_frame) {
          decoded_frame_->UpdateBuffer(
              decoded_frame_buffer,
              decoder->GetWidth() * decoder->GetHeight() * 3 / 2);
          decoded_frame_->SetWidth(received_frame->Width());
          decoded_frame_->SetHeight(received_frame->Height());
          decoded_frame_->SetDecodedWidth(decoder->GetWidth());
          decoded_frame_->SetDecodedHeight(decoder->GetHeight());
          decoded_frame_->SetReceivedTimestamp(
              received_frame->ReceivedTimestamp());
          decoded_frame_->SetCapturedTimestamp(
              received_frame->CapturedTimestamp());
          decoded_frame_->SetDecodedTimestamp(clock_->CurrentTime());

#ifdef SAVE_DECODED_NV12_STREAM
          fwrite((unsigned char *)decoded_frame_->Buffer(), 1,
                 decoded_frame_->Size(), file_nv12_);
#endif
          on_receive_decoded_frame(decoded_frame_);
        }
      }
    }
  }

  return 0;
}