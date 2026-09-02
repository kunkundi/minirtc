#include "openh264_decoder.h"

#include <cstring>

#include "libyuv.h"
#include "log.h"
#if defined(_WIN32)
#include "windows_native_video_frame.h"
#endif

// #define SAVE_DECODED_NV12_STREAM
// #define SAVE_RECEIVED_H264_STREAM

namespace minirtc {

OpenH264Decoder::OpenH264Decoder(std::shared_ptr<SystemClock> clock,
                                 bool native_video_output)
    : clock_(clock), native_video_output_(native_video_output) {}
OpenH264Decoder::~OpenH264Decoder() {
  if (openh264_decoder_) {
    openh264_decoder_->Uninitialize();
    WelsDestroyDecoder(openh264_decoder_);
  }

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

  if (!decoded_frame_ && !native_video_output_) {
    decoded_frame_ = new DecodedFrame(frame_width_ * frame_height_ * 3 / 2,
                                      frame_width_, frame_height_);
  }

#if defined(_WIN32)
  if (native_video_output_) {
    LOG_INFO("OpenH264 Windows native CPU NV12 output enabled");
  }
#endif

  return 0;
}

int OpenH264Decoder::Decode(
    std::unique_ptr<ReceivedFrame> received_frame,
    std::function<void(const DecodedFrame*)> on_receive_decoded_frame) {
  if (!openh264_decoder_) {
    return -1;
  }

  const uint8_t* data = received_frame->Buffer();
  size_t size = received_frame->Size();

  if (data == nullptr) {
    return -1;
  }

#ifdef SAVE_RECEIVED_H264_STREAM
  if (file_h264_) {
    fwrite((unsigned char*)data, 1, size, file_h264_);
  }
#endif

  if (size > 4 && (*(data + 4) & 0x1f) == 0x07) {
    // Key frame received
  }

  SBufferInfo sDstBufInfo;
  memset(&sDstBufInfo, 0, sizeof(SBufferInfo));

  DECODING_STATE decode_state = openh264_decoder_->DecodeFrameNoDelay(
      data, (int)size, yuv420p_planes_, &sDstBufInfo);
  if (decode_state != 0) {
    LOG_ERROR("Failed to decode frame, error code: {}", (int)decode_state);
    return -1;
  }

  if (sDstBufInfo.iBufferStatus == 1) {
    frame_width_ = sDstBufInfo.UsrData.sSystemBuffer.iWidth;
    frame_height_ = sDstBufInfo.UsrData.sSystemBuffer.iHeight;
    frame_size_ = (size_t)frame_width_ * (size_t)frame_height_ * 3 / 2;
    if (!native_video_output_) {
      yuv420p_frame_.resize(frame_size_);
      nv12_frame_.resize(frame_size_);
    }

    if (on_receive_decoded_frame) {
      int stride_y = sDstBufInfo.UsrData.sSystemBuffer.iStride[0];
      int stride_u = sDstBufInfo.UsrData.sSystemBuffer.iStride[1];
      int stride_v = stride_u;

#if defined(_WIN32)
      if (native_video_output_) {
        auto* native_frame =
            WindowsNativeNv12Frame::Create(frame_width_, frame_height_);
        if (!native_frame) {
          LOG_ERROR("Failed to allocate Windows native OpenH264 frame");
          return -1;
        }
        const int conversion_result = libyuv::I420ToNV12(
            yuv420p_planes_[0], stride_y, yuv420p_planes_[1], stride_u,
            yuv420p_planes_[2], stride_v, native_frame->YPlane(),
            frame_width_, native_frame->UvPlane(), frame_width_, frame_width_,
            frame_height_);
        if (conversion_result != 0) {
          native_frame->Release();
          LOG_ERROR("OpenH264 I420 to native NV12 conversion failed, ret {}",
                    conversion_result);
          return -1;
        }

        DecodedFrame native_decoded_frame;
        native_decoded_frame.SetSize(native_frame->Size());
        native_decoded_frame.SetWidth(frame_width_);
        native_decoded_frame.SetHeight(frame_height_);
        native_decoded_frame.SetDecodedWidth(frame_width_);
        native_decoded_frame.SetDecodedHeight(frame_height_);
        native_decoded_frame.SetReceivedTimestamp(
            received_frame->ReceivedTimestamp());
        native_decoded_frame.SetCapturedTimestamp(
            received_frame->CapturedTimestamp());
        native_decoded_frame.SetDecodedTimestamp(clock_->CurrentTime());
        native_decoded_frame.SetNativeHandle(native_frame->Handle());
        native_decoded_frame.SetNativeHandleType(
            XVideoFrameNativeHandleWindowsNv12);
#ifdef SAVE_DECODED_NV12_STREAM
        if (file_nv12_) {
          fwrite(native_frame->Data(), 1, native_frame->Size(), file_nv12_);
        }
#endif
        on_receive_decoded_frame(&native_decoded_frame);
        native_frame->Release();
        return 0;
      }
#endif

      libyuv::I420Copy(
          yuv420p_planes_[0], stride_y, yuv420p_planes_[1], stride_u,
          yuv420p_planes_[2], stride_v, yuv420p_frame_.data(), frame_width_,
          yuv420p_frame_.data() + frame_width_ * frame_height_,
          frame_width_ / 2,
          yuv420p_frame_.data() + frame_width_ * frame_height_ * 5 / 4,
          frame_width_ / 2, frame_width_, frame_height_);

      libyuv::I420ToNV12(
          yuv420p_frame_.data(), frame_width_,
          yuv420p_frame_.data() + frame_width_ * frame_height_,
          frame_width_ / 2,
          yuv420p_frame_.data() + frame_width_ * frame_height_ * 5 / 4,
          frame_width_ / 2, nv12_frame_.data(), frame_width_,
          nv12_frame_.data() + frame_width_ * frame_height_, frame_width_,
          frame_width_, frame_height_);

      decoded_frame_->UpdateBuffer(nv12_frame_.data(), frame_size_);
      decoded_frame_->SetWidth(frame_width_);
      decoded_frame_->SetHeight(frame_height_);
      decoded_frame_->SetDecodedWidth(frame_width_);
      decoded_frame_->SetDecodedHeight(frame_height_);
      decoded_frame_->SetReceivedTimestamp(received_frame->ReceivedTimestamp());
      decoded_frame_->SetCapturedTimestamp(received_frame->CapturedTimestamp());
      decoded_frame_->SetDecodedTimestamp(clock_->CurrentTime());

#ifdef SAVE_DECODED_NV12_STREAM
      if (file_nv12_) {
        fwrite((unsigned char*)decoded_frame_->Buffer(), 1,
               decoded_frame_->Size(), file_nv12_);
      }
#endif
      on_receive_decoded_frame(decoded_frame_);
    }
  }

  return 0;
}
}  // namespace minirtc
