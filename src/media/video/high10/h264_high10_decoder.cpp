#include "h264_high10.h"

#include <atomic>
#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
}
#include "h264_bitstream_parser.h"
#include "hdr_video_frame.h"
#include "libyuv.h"

namespace minirtc {
namespace {

class High10Decoder final : public MediaCodec {
 public:
  High10Decoder(std::unique_ptr<MediaCodec> sdr, std::shared_ptr<SystemClock> clock,
                bool native_output)
      : sdr_(std::move(sdr)), clock_(std::move(clock)), native_output_(native_output) {}

  ~High10Decoder() override {
    avcodec_free_context(&decoder_);
    av_frame_free(&picture_);
    av_packet_free(&packet_);
  }

  int Decode(std::unique_ptr<ReceivedFrame> input,
             std::function<void(const DecodedFrame*)> output) override {
    if (!input || !input->Buffer() || !input->Size()) {
      return -1;
    }
    if (auto profile = H264BitstreamParser::ReadSpsProfile(input->Buffer(), input->Size())) {
      const bool high10 = *profile == 110;
      if (high10 != hdr_) {
        if (high10) sdr_->FinishPendingOutput();
        avcodec_free_context(&decoder_);
        hdr_ = high10;
      }
    }
    if (!hdr_) {
      return sdr_->Decode(std::move(input), std::move(output));
    }
    if (!decoder_ && !Open()) {
      return -1;
    }
    if (input->Size() > std::numeric_limits<int>::max() - AV_INPUT_BUFFER_PADDING_SIZE) {
      return -1;
    }

    // av_new_packet also supplies the zero padding required by libavcodec.
    if (av_new_packet(packet_, static_cast<int>(input->Size())) < 0) {
      return -1;
    }
    std::memcpy(packet_->data, input->Buffer(), input->Size());
    packet_->pts = input->CapturedTimestamp();
    const int sent = avcodec_send_packet(decoder_, packet_);
    av_packet_unref(packet_);
    if (sent < 0) {
      return -1;
    }
    for (;;) {
      const int result = avcodec_receive_frame(decoder_, picture_);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return 0;
      }
      if (result < 0) {
        return -1;
      }
      const bool delivered = Deliver(input->ReceivedTimestamp(), output);
      av_frame_unref(picture_);
      if (!delivered) {
        return -1;
      }
    }
  }

  std::string GetDecoderName() const override {
    return hdr_ ? "FFmpeg (H.264 High 10)" : sdr_->GetDecoderName();
  }

 private:
  bool Open() {
    if (!picture_) {
      picture_ = av_frame_alloc();
    }
    if (!packet_) {
      packet_ = av_packet_alloc();
    }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec || !picture_ || !packet_) {
      return false;
    }
    decoder_ = avcodec_alloc_context3(codec);
    if (!decoder_) {
      return false;
    }
    decoder_->thread_type = FF_THREAD_SLICE;
    decoder_->max_pixels = int64_t{16384} * 16384;
    if (avcodec_open2(decoder_, codec, nullptr) < 0) {
      avcodec_free_context(&decoder_);
      return false;
    }
    return true;
  }

  bool Deliver(int64_t received, const std::function<void(const DecodedFrame*)>& output) {
    if (picture_->format != AV_PIX_FMT_YUV420P10 || picture_->color_primaries != AVCOL_PRI_BT2020 ||
        picture_->color_trc != AVCOL_TRC_SMPTE2084 ||
        picture_->colorspace != AVCOL_SPC_BT2020_NCL || picture_->color_range != AVCOL_RANGE_MPEG) {
      return false;
    }
    const int w = picture_->width, h = picture_->height;
    auto hdr = HdrVideoFrame::Create(w, h);
    if (!hdr || libyuv::I010Copy(
                    reinterpret_cast<const uint16_t*>(picture_->data[0]), picture_->linesize[0] / 2,
                    reinterpret_cast<const uint16_t*>(picture_->data[1]), picture_->linesize[1] / 2,
                    reinterpret_cast<const uint16_t*>(picture_->data[2]), picture_->linesize[2] / 2,
                    hdr->Y(), w, hdr->U(), w / 2, hdr->V(), w / 2, w, h) != 0) {
      return false;
    }

    const size_t size = size_t(w) * h * 3 / 2;
    DecodedFrame frame;
    if (native_output_) {
      frame.SetNativeFrame(hdr->Descriptor());
      frame.SetSize(size);
      frame.SetWidth(w);
      frame.SetHeight(h);
    } else {
      frame = DecodedFrame(size, w, h);
      if (hdr->CopySdr(frame.MutableBuffer(), size) != 0) {
        return false;
      }
    }
    frame.SetDecodedWidth(w);
    frame.SetDecodedHeight(h);
    frame.SetCapturedTimestamp(picture_->pts);
    frame.SetReceivedTimestamp(received);
    frame.SetDecodedTimestamp(clock_->CurrentTime());
    if (output) {
      output(&frame);
    }
    return true;
  }

  std::unique_ptr<MediaCodec> sdr_;
  std::shared_ptr<SystemClock> clock_;
  bool native_output_;
  std::atomic<bool> hdr_{false};
  AVCodecContext* decoder_ = nullptr;
  AVFrame* picture_ = nullptr;
  AVPacket* packet_ = nullptr;
};

}  // namespace

std::unique_ptr<MediaCodec> MakeHigh10Decoder(std::unique_ptr<MediaCodec> sdr,
                                              std::shared_ptr<SystemClock> clock,
                                              bool native_output) {
  return std::make_unique<High10Decoder>(std::move(sdr), std::move(clock), native_output);
}

}  // namespace minirtc
