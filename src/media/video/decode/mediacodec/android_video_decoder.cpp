#include "android_video_decoder.h"

#include "dav1d/dav1d_av1_decoder.h"
#include "openh264/openh264_decoder.h"

namespace minirtc {

AndroidVideoDecoder::AndroidVideoDecoder(std::shared_ptr<SystemClock> clock,
                                         VideoCodecType type,
                                         bool native_video_output)
    : clock_(std::move(clock)),
      type_(type),
      native_video_output_(native_video_output),
      hardware_(clock_, type_, AndroidMediaCodec::Mode::Decoder,
                native_video_output_) {}

int AndroidVideoDecoder::Init() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_) return -1;
  if (hardware_.InitDecoder() != 0 && Fallback("no hardware component") != 0)
    return -1;
  initialized_ = true;
  return 0;
}

int AndroidVideoDecoder::Fallback(const char* reason) {
  LOG_WARN("MediaCodec decoder fallback ({}): {}",
           type_ == VideoCodecType::H264 ? "H.264" : "AV1", reason);
  hardware_.Stop();
  if (type_ == VideoCodecType::H264)
    software_ = std::make_unique<OpenH264Decoder>(clock_, native_video_output_);
  else
    software_ = std::make_unique<Dav1dAv1Decoder>(clock_, native_video_output_);
  if (software_->Init() != 0) {
    software_.reset();
    return -1;
  }
  return 0;
}

int AndroidVideoDecoder::Decode(
    std::unique_ptr<ReceivedFrame> frame,
    std::function<void(const DecodedFrame*)> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ || !frame || !frame->Buffer() || !frame->Size() ||
      frame->Size() > 8 * 1024 * 1024)
    return -1;
  if (software_)
    return software_->Decode(std::move(frame), std::move(callback));
  const auto result = hardware_.Decode(*frame, callback);
  if (result == AndroidMediaCodec::Result::Ok) return 0;
  if (result == AndroidMediaCodec::Result::InvalidInput ||
      result == AndroidMediaCodec::Result::NeedKeyFrame)
    return -1;
  if (Fallback(result == AndroidMediaCodec::Result::ConfigureFailed
                   ? "unsupported stream size or configure failure"
                   : "input/output/Surface error or stalled component") != 0)
    return -1;
  const int status = software_->Decode(std::move(frame), std::move(callback));
  // After a hardware failure, ask the transport for a fresh key frame because
  // software cannot reuse the abandoned hardware reference frames.
  return result == AndroidMediaCodec::Result::CodecError ? -1 : status;
}

int AndroidVideoDecoder::GetResolution(int* width, int* height) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!width || !height) return -1;
  return software_ ? software_->GetResolution(width, height)
                   : hardware_.GetResolution(width, height);
}

std::string AndroidVideoDecoder::GetDecoderName() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return software_ ? software_->GetDecoderName() : hardware_.GetName();
}

}  // namespace minirtc
