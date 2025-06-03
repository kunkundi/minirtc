#include "openh264_decoder.h"

#include <cstring>

#include "libyuv.h"
#include "log.h"

// #define SAVE_DECODED_NV12_STREAM
// #define SAVE_RECEIVED_H264_STREAM

OpenH264Decoder::OpenH264Decoder(std::shared_ptr<SystemClock> clock)
    : clock_(clock) {}
OpenH264Decoder::~OpenH264Decoder() {
  if (openh264_decoder_) {
    openh264_decoder_->Uninitialize();
    WelsDestroyDecoder(openh264_decoder_);
  }

  if (nv12_frame_) {
    delete[] nv12_frame_;
  }

  if (decoded_frame_) {
    delete decoded_frame_;
  }

  if (yuv420p_frame_) {
    delete[] yuv420p_frame_;
  }

#ifdef SAVE_DECODED_NV12_STREAM
  if (file_nv12_) {
    fflush(file_nv12_);
    file_nv12_ = nullptr;
  }
#endif

#ifdef SAVE_RECEIVED_H264_STREAM
  if (file_h264_) {
    fflush(file_h264_);
    file_h264_ = nullptr;
  }
#endif
}

int OpenH264Decoder::Init() {
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

  frame_width_ = 1280;
  frame_height_ = 720;

  if (WelsCreateDecoder(&openh264_decoder_) != 0) {
    LOG_ERROR("Failed to create OpenH264 decoder");
    return -1;
  }

  long ret = -1;
  int trace_level = WELS_LOG_QUIET;
  ret = openh264_decoder_->SetOption(DECODER_OPTION_TRACE_LEVEL, &trace_level);
  if (ret != cmResultSuccess) {
    LOG_ERROR("Failed to set decoder trace level");
    return -1;
  }

  // 1 thread for decoding, do not use more threads
  int decode_thread_count = 1;
  ret = openh264_decoder_->SetOption(DECODER_OPTION_NUM_OF_THREADS,
                                     &decode_thread_count);
  if (ret != cmResultSuccess) {
    LOG_ERROR("Decoder SetOption NUM_OF_THREADS failed, ret {}", ret);
    return -1;
  }

  SDecodingParam sDecParam;
  memset(&sDecParam, 0, sizeof(SDecodingParam));
  sDecParam.uiTargetDqLayer = UCHAR_MAX;
  sDecParam.eEcActiveIdc = ERROR_CON_SLICE_MV_COPY_CROSS_IDR_FREEZE_RES_CHANGE;
  sDecParam.sVideoProperty.size = sizeof(sDecParam.sVideoProperty);
  sDecParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;

  ret = openh264_decoder_->Initialize(&sDecParam);
  if (ret != cmResultSuccess) {
    LOG_ERROR("Failed to initialize OpenH264 decoder, ret {}", ret);
    return -1;
  }

  if (!decoded_frame_) {
    decoded_frame_ = new DecodedFrame(frame_width_ * frame_height_ * 3 / 2,
                                      frame_width_, frame_height_);
  }

  return 0;
}

int OpenH264Decoder::Decode(
    std::unique_ptr<ReceivedFrame> received_frame,
    std::function<void(const DecodedFrame *)> on_receive_decoded_frame) {
  if (!openh264_decoder_) {
    return -1;
  }

  const uint8_t *data = received_frame->Buffer();
  size_t size = received_frame->Size();

  if (data == nullptr) {
    LOG_WARN("data is nullptr!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  }

#ifdef SAVE_RECEIVED_H264_STREAM
  fwrite((unsigned char *)data, 1, size, file_h264_);
#endif

  if (size > 4 && (*(data + 4) & 0x1f) == 0x07) {
    // Key frame received
  }

  SBufferInfo sDstBufInfo;
  memset(&sDstBufInfo, 0, sizeof(SBufferInfo));

  DECODING_STATE ret = openh264_decoder_->DecodeFrameNoDelay(
      data, (int)size, yuv420p_planes_, &sDstBufInfo);
  if (ret != 0) {
    LOG_ERROR("Failed to decode frame, error code: {}", (int)ret);
    return -1;
  }

  frame_width_ = sDstBufInfo.UsrData.sSystemBuffer.iWidth;
  frame_height_ = sDstBufInfo.UsrData.sSystemBuffer.iHeight;
  yuv420p_frame_size_ = frame_width_ * frame_height_ * 3 / 2;
  nv12_frame_size_ = frame_width_ * frame_height_ * 3 / 2;

  if (!yuv420p_frame_) {
    yuv420p_frame_capacity_ = yuv420p_frame_size_;
    yuv420p_frame_ = new unsigned char[yuv420p_frame_capacity_];
  }

  if (yuv420p_frame_capacity_ < yuv420p_frame_size_) {
    yuv420p_frame_capacity_ = yuv420p_frame_size_;
    delete[] yuv420p_frame_;
    yuv420p_frame_ = new unsigned char[yuv420p_frame_capacity_];
  }

  if (!nv12_frame_) {
    nv12_frame_capacity_ = yuv420p_frame_size_;
    nv12_frame_ = new unsigned char[nv12_frame_capacity_];
  }

  if (nv12_frame_capacity_ < yuv420p_frame_size_) {
    nv12_frame_capacity_ = yuv420p_frame_size_;
    delete[] nv12_frame_;
    nv12_frame_ = new unsigned char[nv12_frame_capacity_];
  }

  if (sDstBufInfo.iBufferStatus == 1) {
    if (on_receive_decoded_frame) {
      int stride_y = sDstBufInfo.UsrData.sSystemBuffer.iStride[0];
      int stride_u = sDstBufInfo.UsrData.sSystemBuffer.iStride[1];
      int stride_v = sDstBufInfo.UsrData.sSystemBuffer.iStride[1];

      libyuv::I420Copy(
          yuv420p_planes_[0], stride_y, yuv420p_planes_[1], stride_u,
          yuv420p_planes_[2], stride_v, yuv420p_frame_, frame_width_,
          yuv420p_frame_ + frame_width_ * frame_height_, frame_width_ / 2,
          yuv420p_frame_ + frame_width_ * frame_height_ * 5 / 4,
          frame_width_ / 2, frame_width_, frame_height_);

      libyuv::I420ToNV12(yuv420p_frame_, frame_width_,
                         yuv420p_frame_ + frame_width_ * frame_height_,
                         frame_width_ / 2,
                         yuv420p_frame_ + frame_width_ * frame_height_ * 5 / 4,
                         frame_width_ / 2, nv12_frame_, frame_width_,
                         nv12_frame_ + frame_width_ * frame_height_,
                         frame_width_, frame_width_, frame_height_);

      decoded_frame_->UpdateBuffer(nv12_frame_, nv12_frame_capacity_);
      decoded_frame_->SetWidth(received_frame->Width());
      decoded_frame_->SetHeight(received_frame->Height());
      decoded_frame_->SetDecodedWidth(frame_width_);
      decoded_frame_->SetDecodedHeight(frame_height_);
      decoded_frame_->SetReceivedTimestamp(received_frame->ReceivedTimestamp());
      decoded_frame_->SetCapturedTimestamp(received_frame->CapturedTimestamp());
      decoded_frame_->SetDecodedTimestamp(clock_->CurrentTime());

#ifdef SAVE_DECODED_NV12_STREAM
      fwrite((unsigned char *)decoded_frame_->Buffer(), 1,
             decoded_frame_->Size(), file_nv12_);
#endif
      on_receive_decoded_frame(decoded_frame_);
    }
  }

  return 0;
}