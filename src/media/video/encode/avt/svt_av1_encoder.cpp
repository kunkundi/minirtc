#include "svt_av1_encoder.h"

#include <cstring>

#include "libyuv.h"
#include "log.h"

// #define SAVE_RECEIVED_NV12_STREAM
// #define SAVE_ENCODED_AV1_STREAM

namespace minirtc {

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
}

int SvtAv1Encoder::Init(const MediaCodecConfig& config) {
  frame_width_ = config.init_width;
  frame_height_ = config.init_height;
  max_bitrate_ = config.max_bitrate;
  target_bitrate_ =
      ClampEncoderTargetBitrate(config.average_bitrate, max_bitrate_);
  max_fps_ = config.max_frame_rate;
  key_frame_interval_ = config.key_frame_interval;
  max_payload_size_ = config.max_payload_size;
  video_content_type_ = config.video_content_type;
  video_degradation_preference_ = config.video_degradation_preference;

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

  Release();
  ret = svt_av1_enc_init_handle(&svt_av1_encoder_, &enc_config_);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("Failed to initialize SVT-AV1 encoder handle");
    return -1;
  }

  // Set default config
  frame_width_ = frame_width;
  frame_height_ = frame_height;
  enc_config_.source_width = frame_width_;
  enc_config_.source_height = frame_height_;
  enc_config_.encoder_color_format = EB_YUV420;
  enc_config_.encoder_bit_depth = hdr_ ? 10 : 8;
  enc_config_.color_primaries = hdr_ ? EB_CICP_CP_BT_2020 : EB_CICP_CP_BT_709;
  enc_config_.transfer_characteristics = hdr_ ? EB_CICP_TC_SMPTE_2084 : EB_CICP_TC_SRGB;
  enc_config_.matrix_coefficients = hdr_ ? EB_CICP_MC_BT_2020_NCL : EB_CICP_MC_BT_601;
  enc_config_.color_range = EB_CR_STUDIO_RANGE;
  enc_config_.frame_rate_numerator = max_fps_;
  enc_config_.frame_rate_denominator = 1;
  enc_config_.enc_mode =
      video_degradation_preference_ ==
              MiniRtcVideoDegradationPreference::MaintainFrameRate
          ? 10
          : 8;
  enc_config_.rate_control_mode = SVT_AV1_RC_MODE_CBR;
  enc_config_.pred_structure = SVT_AV1_PRED_LOW_DELAY_B;
  enc_config_.target_bit_rate = target_bitrate_;
  enc_config_.over_shoot_pct =
      EncoderOvershootPercent(target_bitrate_, max_bitrate_);
  enc_config_.max_qp_allowed = 60;
  enc_config_.min_qp_allowed = 10;
  enc_config_.intra_period_length = key_frame_interval_;
  enc_config_.screen_content_mode =
      video_content_type_ == MiniRtcVideoContentType::ScreenContent ? 1 : 0;
  // enc_config_.intra_refresh_type = SVT_AV1_KF_REFRESH;
  enc_config_.level = 52;
  // enc_config_.qp = 63;
  // enc_config_.sframe_dist = key_frame_interval_;

  ret = svt_av1_enc_set_parameter(svt_av1_encoder_, &enc_config_);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("svt_av1_enc_set_parameter failed, return {}",
              static_cast<int>(ret));
    svt_av1_enc_deinit_handle(svt_av1_encoder_);
    svt_av1_encoder_ = nullptr;
    return -1;
  }

  ret = svt_av1_enc_init(svt_av1_encoder_);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("svt_av1_enc_init failed, return {}", static_cast<int>(ret));
    svt_av1_enc_deinit_handle(svt_av1_encoder_);
    svt_av1_encoder_ = nullptr;
    return -1;
  }

  if (!hdr_) yuv420p_frame_.resize(size_t(frame_width_) * frame_height_ * 3 / 2);

  // A reconfigured SVT instance starts a new coded sequence. Make the
  // requirement explicit and restart the periodic key-frame interval.
  force_idr_ = true;
  seq_ = 0;

  return 0;
}

int SvtAv1Encoder::Encode(
    const RawFrame& raw_frame,
    std::function<int(const EncodedFrame& encoded_frame)> on_encoded_image) {
  const auto* native = raw_frame.NativeFrame();
  const bool hdr = native && native->type == MiniRtcNativeVideoFrameHdr10;
  if (!svt_av1_encoder_ || hdr != hdr_ || raw_frame.Width() != frame_width_ ||
      raw_frame.Height() != frame_height_) {
    hdr_ = hdr;
    if (Reconfigure(raw_frame.Width(), raw_frame.Height()) != 0) return -1;
  }

  const size_t plane = size_t(frame_width_) * frame_height_;
  const size_t bytes_per_sample = hdr ? 2 : 1;
  uint8_t* planar = yuv420p_frame_.data();
  if (hdr) {
    // SVT borrows the plane addresses during send_picture and uses sample strides.
    planar = reinterpret_cast<uint8_t*>(const_cast<uint16_t*>(native->payload.hdr10.i010));
  } else {
    if (!raw_frame.Buffer() || raw_frame.Size() < plane * 3 / 2) return -1;
    if (libyuv::NV12ToI420(raw_frame.Buffer(), frame_width_, raw_frame.Buffer() + plane,
                           frame_width_, planar, frame_width_, planar + plane, frame_width_ / 2,
                           planar + plane + plane / 4, frame_width_ / 2, frame_width_,
                           frame_height_) != 0)
      return -1;
  }
  EbSvtIOFormat picture{};
  EbBufferHeaderType input{};
  input.size = sizeof(input);
  input.p_buffer = reinterpret_cast<uint8_t*>(&picture);
  picture.luma = planar;
  picture.cb = planar + plane * bytes_per_sample;
  picture.cr = picture.cb + plane / 4 * bytes_per_sample;
  picture.y_stride = frame_width_;
  picture.cb_stride = frame_width_ / 2;
  picture.cr_stride = frame_width_ / 2;
  input.n_filled_len = static_cast<uint32_t>(plane * 3 / 2 * bytes_per_sample);
  input.pts = raw_frame.CapturedTimestamp();

  if (0 == seq_++ % key_frame_interval_ || force_idr_) {
    input.pic_type = EB_AV1_KEY_PICTURE;
    force_idr_ = false;
  } else {
    input.pic_type = EB_AV1_INVALID_PICTURE;
  }

  EbErrorType ret;
  ret = svt_av1_enc_send_picture(svt_av1_encoder_, &input);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("Failed to send picture");
    return -1;
  }

  EbBufferHeaderType* output_packet = NULL;

  EbErrorType packet_ret =
      svt_av1_enc_get_packet(svt_av1_encoder_, &output_packet, 0);
  if (packet_ret == EB_NoErrorEmptyQueue) {
    LOG_DEBUG("No packet available");
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
      encoded_frame.SetQp(static_cast<int>(output_packet->avg_qp), 0, 63,
                          true);
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
  if (!svt_av1_encoder_ || bitrate <= 0) {
    return -1;
  }
  const int target_bitrate =
      ClampEncoderTargetBitrate(bitrate, max_bitrate_);
  if (target_bitrate != bitrate) {
    LOG_WARN("SVT-AV1 target bitrate clamped: requested={} max={}", bitrate,
             max_bitrate_);
  }

  EbSvtAv1EncConfiguration updated_config = enc_config_;
  updated_config.target_bit_rate = target_bitrate;
  updated_config.over_shoot_pct =
      EncoderOvershootPercent(target_bitrate, max_bitrate_);
  const EbErrorType ret =
      svt_av1_enc_set_parameter(svt_av1_encoder_, &updated_config);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("Failed to set SVT-AV1 target bitrate: bitrate={} return={}",
              target_bitrate, static_cast<int>(ret));
    return -1;
  }

  target_bitrate_ = target_bitrate;
  enc_config_ = updated_config;
  return 0;
}

int SvtAv1Encoder::GetResolution(int* width, int* height) const {
  if (width) *width = frame_width_;
  if (height) *height = frame_height_;
  return 0;
}

std::string SvtAv1Encoder::GetEncoderName() const { return "SVT-AV1 (AV1)"; }

int SvtAv1Encoder::ResetEncodeResolution(unsigned int width,
                                         unsigned int height) {
  LOG_INFO("Reset encode resolution from [{}x{}] to [{}x{}]]", frame_width_,
           frame_height_, width, height);

  return Reconfigure(width, height);
}
}  // namespace minirtc
