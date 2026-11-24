#include "svt_av1_encoder.h"

#include <limits>
#include <new>

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
    EbBufferHeaderType stream_header_buffer{};
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
  Release();
  EbErrorType ret = svt_av1_enc_init_handle(&svt_av1_encoder_, &enc_config_);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("Fail to init svt_av1_encoder_");
    return -1;
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
  enc_config_.enc_mode =
      video_degradation_preference_ ==
              MiniRtcVideoDegradationPreference::MaintainFrameRate
          ? 10
          : 8;
  // SVT-AV1 3.x rejects AQ=0 with CBR. Keep the library's supported AQ
  // configuration for this single-pass, low-delay encoder under either policy.
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

  // A reconfigured SVT instance starts a new coded sequence. Make the
  // requirement explicit and restart the periodic key-frame interval.
  force_idr_ = true;
  seq_ = 0;

  return 0;
}

int SvtAv1Encoder::Encode(
    const RawFrame& raw_frame,
    std::function<int(const EncodedFrame& encoded_frame)> on_encoded_image) {
  const auto width = raw_frame.Width();
  const auto height = raw_frame.Height();
  size_t frame_size = 0;
  if (!raw_frame.Buffer() || !GetNv12FrameSize(width, height, &frame_size) ||
      raw_frame.Size() < frame_size ||
      frame_size > std::numeric_limits<uint32_t>::max()) {
    return -1;
  }
  if (yuv420p_frame_capacity_ < frame_size) {
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[frame_size]);
    if (!buffer) return -1;
    yuv420p_frame_ = std::move(buffer);
    yuv420p_frame_capacity_ = frame_size;
  }

  if (!svt_av1_encoder_ || width != frame_width_ || height != frame_height_) {
    if (ResetEncodeResolution(width, height) != 0) {
      return -1;
    }
  }

#ifdef SAVE_RECEIVED_NV12_STREAM
  fwrite(raw_frame.Buffer(), 1, raw_frame.Size(), file_nv12_);
#endif

  const size_t pixels = static_cast<size_t>(width) * height;
  EbSvtIOFormat picture{};
  picture.luma = yuv420p_frame_.get();
  picture.cb = picture.luma + pixels;
  picture.cr = picture.cb + pixels / 4;
  picture.y_stride = width;
  picture.cb_stride = picture.cr_stride = width / 2;
  if (libyuv::NV12ToI420(raw_frame.Buffer(), width, raw_frame.Buffer() + pixels,
                         width, picture.luma, width, picture.cb, width / 2,
                         picture.cr, width / 2, width, height) != 0) {
    return -1;
  }

  EbBufferHeaderType input{};
  input.size = sizeof(input);
  input.p_buffer = reinterpret_cast<uint8_t*>(&picture);
  input.n_filled_len = static_cast<uint32_t>(frame_size);
  input.pts = raw_frame.CapturedTimestamp();
  input.pic_type = (seq_++ % key_frame_interval_ == 0 || force_idr_)
                       ? EB_AV1_KEY_PICTURE
                       : EB_AV1_INVALID_PICTURE;
  const EbErrorType ret = svt_av1_enc_send_picture(svt_av1_encoder_, &input);
  if (ret != EB_ErrorNone) {
    LOG_ERROR("Failed to send picture");
    return -1;
  }

  force_idr_ = false;

  EbBufferHeaderType* output_packet = nullptr;

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
