#include "android_video_encoder.h"

#include "avt/svt_av1_encoder.h"
#include "openh264/openh264_encoder.h"

namespace minirtc {

AndroidVideoEncoder::AndroidVideoEncoder(std::shared_ptr<SystemClock> clock,
                                         VideoCodecType type)
    : clock_(std::move(clock)),
      type_(type),
      hardware_(clock_, type_, AndroidMediaCodec::Mode::Encoder) {}

int AndroidVideoEncoder::Init(const MediaCodecConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_) return -1;
  config_ = config;
  config_.init_bitrate =
      ClampEncoderTargetBitrate(config.init_bitrate, config.max_bitrate);
  if (hardware_.InitEncoder(config_) != 0 &&
      Fallback("initialization failed") != 0)
    return -1;
  initialized_ = true;
  return 0;
}

int AndroidVideoEncoder::Fallback(const char* reason) {
  LOG_WARN("MediaCodec encoder fallback ({}): {}",
           type_ == VideoCodecType::H264 ? "H.264" : "AV1", reason);
  hardware_.Stop();
  if (type_ == VideoCodecType::H264)
    software_ = std::make_unique<OpenH264Encoder>(clock_);
  else
    software_ = std::make_unique<SvtAv1Encoder>(clock_);
  if (software_->Init(config_) != 0) {
    software_.reset();
    return -1;
  }
  software_->ForceIdr();
  return 0;
}

int AndroidVideoEncoder::Encode(
    const RawFrame& frame, std::function<int(const EncodedFrame&)> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ ||
      !AndroidMediaCodec::IsValidSize(frame.Width(), frame.Height()))
    return -1;
  if (software_) return software_->Encode(frame, std::move(callback));
  const auto result = hardware_.Encode(frame, callback);
  if (result == AndroidMediaCodec::Result::Ok) return 0;
  if (result == AndroidMediaCodec::Result::InvalidInput) return -1;
  config_.init_width = frame.Width();
  config_.init_height = frame.Height();
  if (Fallback(result == AndroidMediaCodec::Result::ConfigureFailed
                   ? "unsupported size or resolution change"
                   : "input/output error or exhausted input buffers") != 0)
    return -1;
  return software_->Encode(frame, std::move(callback));
}

int AndroidVideoEncoder::ForceIdr() {
  std::lock_guard<std::mutex> lock(mutex_);
  return software_ ? software_->ForceIdr() : hardware_.ForceIdr();
}

int AndroidVideoEncoder::SetTargetBitrate(int bitrate) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.init_bitrate =
      ClampEncoderTargetBitrate(bitrate, config_.max_bitrate);
  return software_ ? software_->SetTargetBitrate(config_.init_bitrate)
                   : hardware_.SetTargetBitrate(config_.init_bitrate);
}

int AndroidVideoEncoder::GetResolution(int* width, int* height) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!width || !height) return -1;
  return software_ ? software_->GetResolution(width, height)
                   : hardware_.GetResolution(width, height);
}

std::string AndroidVideoEncoder::GetEncoderName() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return software_ ? software_->GetEncoderName() : hardware_.GetName();
}

}  // namespace minirtc
