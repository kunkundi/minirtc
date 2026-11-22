#include "h264_high10.h"

#include <algorithm>
#include <atomic>

#include <x264.h>

namespace minirtc {
namespace {

class High10Encoder final : public MediaCodec {
 public:
  High10Encoder(std::unique_ptr<MediaCodec> sdr, std::shared_ptr<SystemClock> clock,
                const MediaCodecConfig& config)
      : sdr_(std::move(sdr)),
        clock_(std::move(clock)),
        config_(config),
        bitrate_(ClampEncoderTargetBitrate(config.init_bitrate, config.max_bitrate)),
        fps_(std::max(config.max_frame_rate, 1)) {}

  int Encode(const RawFrame& raw, std::function<int(const EncodedFrame&)> output) override {
    const auto* native = raw.NativeFrame();
    if (!native || native->type != MiniRtcNativeVideoFrameHdr10) {
      if (encoder_) {
        if (sdr_->ForceIdr() != 0) {
          return -1;
        }
        encoder_.reset();
        hdr_ = false;
      }
      return sdr_->Encode(raw, std::move(output));
    }

    const int w = native->width, h = native->height;
    if (!encoder_ || width_ != w || height_ != h || opened_fps_ != fps_) {
      if (!Open(w, h)) {
        return -1;
      }
    }
    x264_picture_t input, encoded;
    x264_picture_init(&input);
    // x264 borrows the planes for this call. Its C API predates const input planes.
    auto* y = const_cast<uint16_t*>(native->payload.hdr10.i010);
    const size_t plane = size_t(w) * h;
    input.img.i_csp = X264_CSP_I420 | X264_CSP_HIGH_DEPTH;
    input.img.i_plane = 3;
    input.img.plane[0] = reinterpret_cast<uint8_t*>(y);
    input.img.plane[1] = reinterpret_cast<uint8_t*>(y + plane);
    input.img.plane[2] = reinterpret_cast<uint8_t*>(y + plane + plane / 4);
    input.img.i_stride[0] = w * 2;
    input.img.i_stride[1] = input.img.i_stride[2] = w;
    input.i_pts = picture_index_++;
    input.i_type = force_idr_ ? X264_TYPE_IDR : X264_TYPE_AUTO;

    x264_nal_t* nals = nullptr;
    int count = 0;
    const int bytes = x264_encoder_encode(encoder_.get(), &nals, &count, &input, &encoded);
    if (bytes <= 0 || !count) {
      return -1;
    }
    force_idr_ = false;
    // x264 guarantees that the Annex B NAL payloads are contiguous.
    EncodedFrame frame(nals[0].p_payload, bytes, w, h);
    frame.SetFrameType(encoded.b_keyframe ? kVideoFrameKey : kVideoFrameDelta);
    frame.SetEncodedWidth(w);
    frame.SetEncodedHeight(h);
    frame.SetCapturedTimestamp(raw.CapturedTimestamp());
    frame.SetEncodedTimestamp(clock_->CurrentTime());
    if (encoded.i_qpplus1 > 0) {
      frame.SetQp(encoded.i_qpplus1 - 1, 0, 63, false);
    }
    if (output) {
      output(frame);
    }
    return 0;
  }

  bool SupportsNativeFrameInput(MiniRtcNativeVideoFrameType type) const override {
    return type == MiniRtcNativeVideoFrameHdr10 || sdr_->SupportsNativeFrameInput(type);
  }
  int ForceIdr() override {
    force_idr_ = true;
    return hdr_ ? 0 : sdr_->ForceIdr();
  }
  int SetTargetBitrate(int bps) override {
    if (bps <= 0) {
      return -1;
    }
    const int target = ClampEncoderTargetBitrate(bps, config_.max_bitrate);
    if (encoder_) {
      x264_param_t params;
      x264_encoder_parameters(encoder_.get(), &params);
      SetRateControl(params, target);
      if (x264_encoder_reconfig(encoder_.get(), &params) < 0) {
        return -1;
      }
    }
    bitrate_ = target;
    const int sdr_result = sdr_->SetTargetBitrate(target);
    return hdr_ ? 0 : sdr_result;
  }
  int SetTargetFrameRate(int fps) override {
    if (fps < 1 || fps > MINIRTC_MAX_FRAME_RATE) {
      return -1;
    }
    fps_ = fps;
    const int result = sdr_->SetTargetFrameRate(fps);
    return hdr_ ? 0 : result;
  }
  bool SupportsInputFrameRateUpdates() const override {
    return !hdr_ && sdr_->SupportsInputFrameRateUpdates();
  }
  int SetInputFrameRate(int fps) override { return sdr_->SetInputFrameRate(fps); }
  bool SupportsDynamicEncodingSpeedPriority() const override {
    return !hdr_ && sdr_->SupportsDynamicEncodingSpeedPriority();
  }
  int SetPrioritizeEncodingSpeedOverQuality(bool speed) override {
    return sdr_->SetPrioritizeEncodingSpeedOverQuality(speed);
  }
  int GetResolution(int* w, int* h) const override {
    if (!hdr_) {
      return sdr_->GetResolution(w, h);
    }
    if (w) {
      *w = width_;
    }
    if (h) {
      *h = height_;
    }
    return 0;
  }
  std::string GetEncoderName() const override {
    return hdr_ ? "x264 (H.264 High 10)" : sdr_->GetEncoderName();
  }

 private:
  static void SetRateControl(x264_param_t& params, int bitrate) {
    const int kbps = std::max(1, bitrate / 1000);
    params.rc.i_bitrate = kbps;
    params.rc.i_vbv_max_bitrate = kbps;
    params.rc.i_vbv_buffer_size = std::max(1, kbps / 2);
  }

  bool Open(int w, int h) {
    x264_param_t params;
    if (x264_param_default_preset(&params, "veryfast", "zerolatency") < 0) {
      return false;
    }
    params.i_width = w;
    params.i_height = h;
    params.i_bitdepth = 10;
    params.i_csp = X264_CSP_I420;
    params.vui.i_colorprim = 9;  // BT.2020
    params.vui.i_transfer = 16;  // SMPTE ST 2084
    params.vui.i_colmatrix = 9;  // BT.2020 non-constant luminance
    params.vui.b_fullrange = 0;
    params.i_fps_num = fps_;
    params.i_fps_den = 1;
    params.b_vfr_input = 0;  // Zero-latency: no lookahead to infer frame duration.
    params.b_repeat_headers = 1;
    params.b_annexb = 1;
    params.i_keyint_max = std::max(1, config_.key_frame_interval);
    params.i_log_level = X264_LOG_WARNING;
    params.rc.i_rc_method = X264_RC_ABR;
    SetRateControl(params, bitrate_);
    if (x264_param_apply_profile(&params, "high10") < 0) {
      return false;
    }
    auto* encoder = x264_encoder_open(&params);
    if (!encoder) {
      LOG_ERROR("Could not open x264 High 10 encoder for {}x{}", w, h);
      return false;
    }
    if (!hdr_) sdr_->FinishPendingOutput();
    encoder_.reset(encoder);
    width_ = w;
    height_ = h;
    opened_fps_ = fps_;
    picture_index_ = 0;
    hdr_ = true;
    force_idr_ = true;
    return true;
  }

  std::unique_ptr<MediaCodec> sdr_;
  std::shared_ptr<SystemClock> clock_;
  MediaCodecConfig config_;
  std::unique_ptr<x264_t, decltype(&x264_encoder_close)> encoder_{nullptr, x264_encoder_close};
  int bitrate_, fps_, opened_fps_ = 0;
  int64_t picture_index_ = 0;
  std::atomic<int> width_{0}, height_{0};
  std::atomic<bool> hdr_{false};
  bool force_idr_ = false;
};

}  // namespace

std::unique_ptr<MediaCodec> MakeHigh10Encoder(std::unique_ptr<MediaCodec> sdr,
                                              std::shared_ptr<SystemClock> clock,
                                              const MediaCodecConfig& config) {
  return std::make_unique<High10Encoder>(std::move(sdr), std::move(clock), config);
}

}  // namespace minirtc
