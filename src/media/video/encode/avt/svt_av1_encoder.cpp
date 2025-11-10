#include "svt_av1_encoder.h"

#include <cstring>

#include "libyuv.h"
#include "log.h"

// #define SAVE_RECEIVED_NV12_STREAM
// #define SAVE_ENCODED_AV1_STREAM

#define INPUT_SIZE_240p_TH 0x28500    // 0.165 Million
#define INPUT_SIZE_360p_TH 0x4CE00    // 0.315 Million
#define INPUT_SIZE_480p_TH 0xA1400    // 0.661 Million
#define INPUT_SIZE_720p_TH 0x16DA00   // 1.5 Million
#define INPUT_SIZE_1080p_TH 0x535200  // 5.46 Million
#define INPUT_SIZE_4K_TH 0x140A000    // 21 Million
#define INPUT_SIZE_8K_TH 0X5028000    // 84 Million
#define EB_OUTPUTSTREAMBUFFERSIZE_MACRO(ResolutionSize) \
  ((ResolutionSize) < (INPUT_SIZE_720p_TH) ? 0x1E8480 : 0x2DC6C0)

namespace minirtc {

static void Nv12ToI420(unsigned char* Src_data, int src_width, int src_height,
                       unsigned char* Dst_data) {
  // NV12
  int NV12_Y_Size = src_width * src_height;

  // YUV420
  int I420_Y_Size = src_width * src_height;
  int I420_U_Size = (src_width >> 1) * (src_height >> 1);
  int I420_V_Size = I420_U_Size;

  // src: buffer address of Y channel and UV channel
  unsigned char* Y_data_Src = Src_data;
  unsigned char* UV_data_Src = Src_data + NV12_Y_Size;
  int src_stride_y = src_width;
  int src_stride_uv = src_width;

  // dst: buffer address of Y channel、U channel and V channel
  unsigned char* Y_data_Dst = Dst_data;
  unsigned char* U_data_Dst = Dst_data + I420_Y_Size;
  unsigned char* V_data_Dst = Dst_data + I420_Y_Size + I420_V_Size;
  int Dst_Stride_Y = src_width;
  int Dst_Stride_U = src_width >> 1;
  int Dst_Stride_V = Dst_Stride_U;

  libyuv::NV12ToI420(
      (const uint8_t*)Y_data_Src, src_stride_y, (const uint8_t*)UV_data_Src,
      src_stride_uv, (uint8_t*)Y_data_Dst, Dst_Stride_Y, (uint8_t*)U_data_Dst,
      Dst_Stride_U, (uint8_t*)V_data_Dst, Dst_Stride_V, src_width, src_height);
}

SvtAv1Encoder::SvtAv1Encoder(std::shared_ptr<SystemClock> clock)
    : clock_(clock) {}

SvtAv1Encoder::~SvtAv1Encoder() {
#ifdef SAVE_RECEIVED_NV12_STREAM
  if (file_nv12_) {
    fflush(file_nv12_);
    fclose(file_nv12_);
    file_nv12_ = nullptr;
  }
#endif

#ifdef SAVE_ENCODED_AV1_STREAM
  if (file_av1_) {
    fflush(file_av1_);
    fclose(file_av1_);
    file_av1_ = nullptr;
  }
#endif
  Release();
}

void SvtAv1Encoder::Release() {
  if (svt_av1_encoder_) {
    EbBufferHeaderType stream_header_buffer;
    memset(&stream_header_buffer, 0, sizeof(stream_header_buffer));
    stream_header_buffer.pic_type = EB_AV1_INVALID_PICTURE;
    stream_header_buffer.flags = EB_BUFFERFLAG_EOS;
    svt_av1_enc_send_picture(svt_av1_encoder_, &stream_header_buffer);
    svt_av1_enc_deinit(svt_av1_encoder_);
    svt_av1_enc_deinit_handle(svt_av1_encoder_);
    svt_av1_encoder_ = nullptr;
  }

  if (stream_header_buffer_) {
    if (stream_header_buffer_->p_buffer) {
      free(stream_header_buffer_->p_buffer);
      stream_header_buffer_->p_buffer = nullptr;
    }
    free(stream_header_buffer_);
    stream_header_buffer_ = nullptr;
  }

  delete[] yuv420p_frame_;
  yuv420p_frame_ = nullptr;
}

int SvtAv1Encoder::Init(const MediaCodecConfig& config) {
#ifdef SAVE_RECEIVED_NV12_STREAM
  nv12_file_name_ = "received_nv12_stream_" +
                    std::to_string(reinterpret_cast<uintptr_t>(this)) + ".yuv";
  file_nv12_ = fopen(nv12_file_name_.c_str(), "w+b");
  if (!file_nv12_) {
    LOG_ERROR("Fail to open {}", nv12_file_name_.c_str());
  }
#endif

#ifdef SAVE_ENCODED_AV1_STREAM
  av1_file_name_ = "encoded_h264_stream_" +
                   std::to_string(reinterpret_cast<uintptr_t>(this)) + ".ivf";
  file_av1_ = fopen(av1_file_name_.c_str(), "w+b");
  if (!file_av1_) {
    LOG_ERROR("Fail to open {}", av1_file_name_.c_str());
  }
#endif
  return Reconfigure(frame_width_, frame_height_);
}

int SvtAv1Encoder::Reconfigure(uint32_t frame_width, uint32_t frame_height) {
  EbErrorType ret;

  if (svt_av1_encoder_) {
    EbBufferHeaderType stream_header_buffer;
    memset(&stream_header_buffer, 0, sizeof(stream_header_buffer));
    stream_header_buffer.pic_type = EB_AV1_INVALID_PICTURE;
    stream_header_buffer.flags = EB_BUFFERFLAG_EOS;
    svt_av1_enc_send_picture(svt_av1_encoder_, &stream_header_buffer);
    svt_av1_enc_deinit(svt_av1_encoder_);
    svt_av1_enc_deinit_handle(svt_av1_encoder_);
    svt_av1_encoder_ = nullptr;
  }

  if (!svt_av1_encoder_) {
    ret = svt_av1_enc_init_handle(&svt_av1_encoder_, &enc_config_);
    if (ret != EB_ErrorNone) {
      LOG_ERROR("Fail to init svt_av1_encoder_");
      return -1;
    }
  }

  if (stream_header_buffer_) {
    if (stream_header_buffer_->p_buffer) {
      delete[] stream_header_buffer_->p_buffer;
      stream_header_buffer_->p_buffer = nullptr;
    }
    delete stream_header_buffer_;
    stream_header_buffer_ = nullptr;
  }

  // Set default config
  frame_width_ = frame_width;
  frame_height_ = frame_height;
  enc_config_.source_width = frame_width_;
  enc_config_.source_height = frame_height_;
  enc_config_.encoder_color_format = EB_YUV420;
  enc_config_.encoder_bit_depth = 8;
  enc_config_.frame_rate_numerator = max_fps_;
  enc_config_.frame_rate_denominator = 1;
  enc_config_.enc_mode = 10;
  enc_config_.rate_control_mode = SVT_AV1_RC_MODE_CBR;
  enc_config_.pred_structure = SVT_AV1_PRED_LOW_DELAY_B;
  enc_config_.target_bit_rate = max_bitrate_;
  enc_config_.max_qp_allowed = 60;
  enc_config_.min_qp_allowed = 10;
  enc_config_.intra_period_length = I_FRAME_INTERVAL;
  // enc_config_.intra_refresh_type = SVT_AV1_KF_REFRESH;
  enc_config_.level = 52;
  // enc_config_.qp = 63;
  // enc_config_.screen_content_mode = 1;
  // enc_config_.sframe_dist = I_FRAME_INTERVAL;

  svt_av1_enc_set_parameter(svt_av1_encoder_, &enc_config_);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("svt_av1_enc_set_parameter failed");
    return -1;
  }

  ret = svt_av1_enc_init(svt_av1_encoder_);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("svt_av1_enc_init failed");
    return -1;
  }

  stream_header_buffer_ = new EbBufferHeaderType;
  if (!stream_header_buffer_) {
    LOG_ERROR("Failed to allocate stream header buffer");
    return -1;
  }
  stream_header_buffer_->size = sizeof(EbBufferHeaderType);
  stream_header_buffer_->p_buffer =
      new uint8_t[EB_OUTPUTSTREAMBUFFERSIZE_MACRO(frame_width * frame_height)];
  if (!stream_header_buffer_->p_buffer) {
    LOG_ERROR("Failed to allocate input picture buffer");
    delete stream_header_buffer_;
    stream_header_buffer_ = nullptr;
    return -1;
  }

  yuv420p_frame_capacity_ = frame_width_ * frame_height_ * 3 / 2;
  yuv420p_frame_ = new uint8_t[yuv420p_frame_capacity_];

  return 0;
}

int SvtAv1Encoder::Encode(
    const RawFrame& raw_frame,
    std::function<int(const EncodedFrame& encoded_frame)> on_encoded_image) {
  if (!svt_av1_encoder_) {
    LOG_ERROR("Invalid openh264 encoder");
    return -1;
  }

#ifdef SAVE_RECEIVED_NV12_STREAM
  fwrite(raw_frame.Buffer(), 1, raw_frame.Size(), file_nv12_);
#endif

  if (!yuv420p_frame_) {
    yuv420p_frame_capacity_ = raw_frame.Size();
    yuv420p_frame_ = new unsigned char[yuv420p_frame_capacity_];
  }

  if (yuv420p_frame_capacity_ < raw_frame.Size()) {
    yuv420p_frame_capacity_ = raw_frame.Size();
    delete[] yuv420p_frame_;
    yuv420p_frame_ = new unsigned char[yuv420p_frame_capacity_];
  }

  if (raw_frame.Width() != frame_width_ ||
      raw_frame.Height() != frame_height_) {
    ResetEncodeResolution(raw_frame.Width(), raw_frame.Height());
  }

  Nv12ToI420((unsigned char*)raw_frame.Buffer(), raw_frame.Width(),
             raw_frame.Height(), yuv420p_frame_);

  EbSvtIOFormat* input_picture_buffer =
      (EbSvtIOFormat*)stream_header_buffer_->p_buffer;

  const int y_stride = frame_width_;
  const int uv_stride = frame_width_ / 2;
  const int y_plane_size = y_stride * frame_height_;
  const int uv_plane_size = uv_stride * (frame_height_ / 2);

  uint8_t* y_dst = yuv420p_frame_;
  uint8_t* u_dst = y_dst + y_plane_size;
  uint8_t* v_dst = u_dst + uv_plane_size;

  input_picture_buffer->luma = y_dst;
  input_picture_buffer->cb = u_dst;
  input_picture_buffer->cr = v_dst;
  input_picture_buffer->y_stride = frame_width_;
  input_picture_buffer->cb_stride = frame_width_ / 2;
  input_picture_buffer->cr_stride = frame_width_ / 2;
  stream_header_buffer_->n_filled_len = frame_width_ * frame_height_ * 3 / 2;
  stream_header_buffer_->flags = 0;
  stream_header_buffer_->p_app_private = nullptr;
  stream_header_buffer_->pts = raw_frame.CapturedTimestamp();
  stream_header_buffer_->metadata = nullptr;

  VideoFrameType frame_type;
  if (0 == seq_++ % key_frame_interval_ || force_idr_) {
    stream_header_buffer_->pic_type = EB_AV1_KEY_PICTURE;
    force_idr_ = false;
  } else {
    stream_header_buffer_->pic_type = EB_AV1_INVALID_PICTURE;
    // stream_header_buffer_->qp = 10;
  }

  EbErrorType ret;
  ret = svt_av1_enc_send_picture(svt_av1_encoder_, stream_header_buffer_);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("Failed to send picture");
    return -1;
  }

  EbBufferHeaderType* output_packet = NULL;

  EbErrorType packet_ret =
      svt_av1_enc_get_packet(svt_av1_encoder_, &output_packet, 0);
  if (packet_ret == EB_NoErrorEmptyQueue) {
    LOG_INFO("No packet available");
    return 0;
  }
  if (packet_ret != EB_ErrorNone) {
    LOG_ERROR("Failed to get packet");
    return -1;
  }

  if (output_packet && output_packet->n_filled_len > 0) {
    if (on_encoded_image) {
      EncodedFrame encoded_frame(output_packet->p_buffer,
                                 output_packet->n_filled_len, frame_width_,
                                 frame_height_);
      encoded_frame.SetFrameType((output_packet->pic_type == EB_AV1_KEY_PICTURE)
                                     ? kVideoFrameKey
                                     : kVideoFrameDelta);
      encoded_frame.SetEncodedWidth(frame_width_);
      encoded_frame.SetEncodedHeight(frame_height_);
      encoded_frame.SetCapturedTimestamp(raw_frame.CapturedTimestamp());
      encoded_frame.SetEncodedTimestamp(clock_->CurrentTime());
      on_encoded_image(encoded_frame);
#ifdef SAVE_ENCODED_AV1_STREAM
      fwrite(encoded_frame.Buffer(), 1, encoded_frame.Size(), file_av1_);
#endif
    }
  }

  if (output_packet) {
    svt_av1_enc_release_out_buffer(&output_packet);
  }
  output_packet = nullptr;

  return 0;
}

int SvtAv1Encoder::ForceIdr() {
  force_idr_ = true;
  return 0;
}

int SvtAv1Encoder::SetTargetBitrate(int bitrate) {
  enc_config_.target_bit_rate = bitrate;
  svt_av1_enc_set_parameter(svt_av1_encoder_, &enc_config_);
  return 0;
}

int SvtAv1Encoder::GetResolution(int* width, int* height) const {
  if (width) *width = frame_width_;
  if (height) *height = frame_height_;
  return 0;
}

std::string SvtAv1Encoder::GetEncoderName() const { return "SvtAv1Encoder"; }

int SvtAv1Encoder::ResetEncodeResolution(unsigned int width,
                                         unsigned int height) {
  LOG_INFO("Reset encode resolution from [{}x{}] to [{}x{}]]", frame_width_,
           frame_height_, width, height);

  return Reconfigure(width, height);
}
}  // namespace minirtc