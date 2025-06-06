#include "svt_av1_encoder.h"

#include <cstring>

#include "libyuv.h"
#include "log.h"

// #define SAVE_RECEIVED_NV12_STREAM
// #define SAVE_ENCODED_H264_STREAM

static void Nv12ToI420(unsigned char *Src_data, int src_width, int src_height,
                       unsigned char *Dst_data) {
  // NV12
  int NV12_Y_Size = src_width * src_height;

  // YUV420
  int I420_Y_Size = src_width * src_height;
  int I420_U_Size = (src_width >> 1) * (src_height >> 1);
  int I420_V_Size = I420_U_Size;

  // src: buffer address of Y channel and UV channel
  unsigned char *Y_data_Src = Src_data;
  unsigned char *UV_data_Src = Src_data + NV12_Y_Size;
  int src_stride_y = src_width;
  int src_stride_uv = src_width;

  // dst: buffer address of Y channel、U channel and V channel
  unsigned char *Y_data_Dst = Dst_data;
  unsigned char *U_data_Dst = Dst_data + I420_Y_Size;
  unsigned char *V_data_Dst = Dst_data + I420_Y_Size + I420_V_Size;
  int Dst_Stride_Y = src_width;
  int Dst_Stride_U = src_width >> 1;
  int Dst_Stride_V = Dst_Stride_U;

  libyuv::NV12ToI420(
      (const uint8_t *)Y_data_Src, src_stride_y, (const uint8_t *)UV_data_Src,
      src_stride_uv, (uint8_t *)Y_data_Dst, Dst_Stride_Y, (uint8_t *)U_data_Dst,
      Dst_Stride_U, (uint8_t *)V_data_Dst, Dst_Stride_V, src_width, src_height);
}

SvtAv1Encoder::SvtAv1Encoder(std::shared_ptr<SystemClock> clock)
    : clock_(clock) {}

SvtAv1Encoder::~SvtAv1Encoder() {
  if (svt_av1_encoder_) {
    svt_av1_enc_deinit(svt_av1_encoder_);
    svt_av1_enc_deinit_handle(svt_av1_encoder_);
    svt_av1_encoder_ = nullptr;
  }

  if (stream_header_buffer_) {
    svt_av1_enc_stream_header_release(stream_header_buffer_);
    stream_header_buffer_ = nullptr;
  }

  delete[] yuv420p_frame_;
  yuv420p_frame_ = nullptr;
}

int SvtAv1Encoder::Init() {
  EbErrorType ret;
  ret = svt_av1_enc_init_handle(&svt_av1_encoder_, &enc_config_);
  if (ret != EB_ErrorNone) return -1;

  // Set default config
  frame_width_ = 1920;
  frame_height_ = 1080;
  enc_config_.source_width = frame_width_;
  enc_config_.source_height = frame_height_;
  enc_config_.encoder_color_format = EB_YUV420;
  enc_config_.frame_rate_numerator = 30;
  enc_config_.frame_rate_denominator = 1;
  enc_config_.encoder_bit_depth = 8;
  enc_config_.rate_control_mode = 1;  // VBR
  enc_config_.target_bit_rate = max_bitrate_;
  enc_config_.max_qp_allowed = 63;
  enc_config_.min_qp_allowed = 10;
  enc_config_.intra_period_length = I_FRAME_INTERVAL / 1000 * 30;

  const size_t luma_size = enc_config_.source_width *
                           enc_config_.source_height *
                           (enc_config_.encoder_bit_depth > 8 ? 2 : 1);

  EbSvtIOFormat *in_data;

  stream_header_buffer_ =
      (EbBufferHeaderType *)calloc(1, sizeof(EbBufferHeaderType));
  if (!stream_header_buffer_) {
    LOG_ERROR("Failed to allocate stream header buffer");
    return -1;
  }
  stream_header_buffer_->p_buffer = (uint8_t *)calloc(1, sizeof(EbSvtIOFormat));
  if (!stream_header_buffer_->p_buffer) {
    LOG_ERROR("Failed to allocate input picture buffer");
    free(stream_header_buffer_);
    stream_header_buffer_ = nullptr;
    return -1;
  }
  stream_header_buffer_->size = sizeof(*stream_header_buffer_);

  yuv420p_frame_capacity_ = frame_width_ * frame_height_ * 3 / 2;
  yuv420p_frame_ = new uint8_t[yuv420p_frame_capacity_];

  return 0;
}

int SvtAv1Encoder::Encode(
    const RawFrame &raw_frame,
    std::function<int(const EncodedFrame &encoded_frame)> on_encoded_image) {
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

  Nv12ToI420((unsigned char *)raw_frame.Buffer(), raw_frame.Width(),
             raw_frame.Height(), yuv420p_frame_);

  EbSvtIOFormat *input_picture_buffer =
      (EbSvtIOFormat *)stream_header_buffer_->p_buffer;

  const int y_stride = frame_width_;
  const int uv_stride = frame_width_ / 2;
  const int y_plane_size = y_stride * frame_height_;
  const int uv_plane_size = uv_stride * (frame_height_ / 2);

  uint8_t *y_dst = yuv420p_frame_;
  uint8_t *u_dst = y_dst + y_plane_size;
  uint8_t *v_dst = u_dst + uv_plane_size;

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
  svt_metadata_array_free(&stream_header_buffer_->metadata);

  VideoFrameType frame_type;
  if (0 == seq_++ % key_frame_interval_ || force_idr_) {
    stream_header_buffer_->pic_type = EB_AV1_KEY_PICTURE;
  } else {
    stream_header_buffer_->pic_type = EB_AV1_INVALID_PICTURE;
  }

  EbErrorType ret;
  ret = svt_av1_enc_send_picture(svt_av1_encoder_, stream_header_buffer_);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("Failed to send picture");
    return 1;
  }

  // EbBufferHeaderType *output_packet = NULL;
  // while (true) {
  //   EbErrorType packet_ret =
  //       svt_av1_enc_get_packet(svt_av1_encoder_, &output_packet, 0);
  //   if (packet_ret == EB_NoErrorEmptyQueue) {
  //     break;
  //   }
  //   if (packet_ret != EB_ErrorNone) {
  //     LOG_ERROR("Failed to get packet");
  //     break;
  //   }

  //   if (output_packet && output_packet->n_filled_len > 0) {
  //     LOG_INFO("Encoded frame size: {} bytes", output_packet->n_filled_len);
  //     // 处理 encoded_frame...
  //   }

  //   svt_av1_enc_release_out_buffer(&output_packet);
  // }

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

int SvtAv1Encoder::GetResolution(int *width, int *height) {
  if (width) *width = frame_width_;
  if (height) *height = frame_height_;
  return 0;
}

std::string SvtAv1Encoder::GetEncoderName() { return "SvtAv1Encoder"; }

int SvtAv1Encoder::ResetEncodeResolution(unsigned int width,
                                         unsigned int height) {
  LOG_INFO("Reset encode resolution from [{}x{}] to [{}x{}]]", frame_width_,
           frame_height_, width, height);

  frame_width_ = width;
  frame_height_ = height;

  enc_config_.source_width = frame_width_;
  enc_config_.source_height = frame_height_;

  EbErrorType ret;
  ret = svt_av1_enc_set_parameter(svt_av1_encoder_, &enc_config_);
  if (ret != EB_ErrorNone) return -2;

  return 0;
}