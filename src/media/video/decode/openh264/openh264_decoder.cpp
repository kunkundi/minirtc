#include "openh264_decoder.h"

#include <cstring>

#include "libyuv.h"
#include "log.h"

// #define SAVE_DECODED_NV12_STREAM
// #define SAVE_RECEIVED_H264_STREAM

void CopyYuvWithStride(uint8_t *src_y, uint8_t *src_u, uint8_t *src_v,
                       int width, int height, int stride_y, int stride_u,
                       int stride_v, uint8_t *yuv420p_frame) {
  int actual_stride_y = width;
  int actual_stride_u = width / 2;
  int actual_stride_v = width / 2;

  for (int row = 0; row < height; row++) {
    memcpy(yuv420p_frame, src_y, actual_stride_y);
    src_y += stride_y;
    yuv420p_frame += actual_stride_y;
  }

  for (int row = 0; row < height / 2; row++) {
    memcpy(yuv420p_frame, src_u, actual_stride_u);
    src_u += stride_u;
    yuv420p_frame += actual_stride_u;
  }

  for (int row = 0; row < height / 2; row++) {
    memcpy(yuv420p_frame, src_v, actual_stride_v);
    src_v += stride_v;
    yuv420p_frame += actual_stride_v;
  }
}

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

  SDecodingParam sDecParam;

  memset(&sDecParam, 0, sizeof(SDecodingParam));
  sDecParam.uiTargetDqLayer = UCHAR_MAX;
  sDecParam.eEcActiveIdc = ERROR_CON_SLICE_COPY;
  sDecParam.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;

  openh264_decoder_->Initialize(&sDecParam);

  int trace_level = WELS_LOG_QUIET;
  openh264_decoder_->SetOption(DECODER_OPTION_TRACE_LEVEL, &trace_level);

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

  int ret = openh264_decoder_->DecodeFrameNoDelay(
      data, (int)size, yuv420p_planes_, &sDstBufInfo);
  if (ret != 0) {
    LOG_ERROR("Failed to decode frame, error code: {}", ret);
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
      CopyYuvWithStride(
          yuv420p_planes_[0], yuv420p_planes_[1], yuv420p_planes_[2],
          sDstBufInfo.UsrData.sSystemBuffer.iWidth,
          sDstBufInfo.UsrData.sSystemBuffer.iHeight,
          sDstBufInfo.UsrData.sSystemBuffer.iStride[0],
          sDstBufInfo.UsrData.sSystemBuffer.iStride[1],
          sDstBufInfo.UsrData.sSystemBuffer.iStride[2], yuv420p_frame_);

      libyuv::I420ToNV12(
          (const uint8_t *)yuv420p_frame_, frame_width_,
          (const uint8_t *)yuv420p_frame_ + frame_width_ * frame_height_,
          frame_width_ / 2,
          (const uint8_t *)yuv420p_frame_ +
              frame_width_ * frame_height_ * 5 / 4,
          frame_width_ / 2, (uint8_t *)nv12_frame_, frame_width_,
          (uint8_t *)nv12_frame_ + frame_width_ * frame_height_, frame_width_,
          frame_width_, frame_height_);

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