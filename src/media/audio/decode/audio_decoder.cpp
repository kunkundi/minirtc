#include "audio_decoder.h"

#include <limits>

#include "log.h"

namespace minirtc {
AudioDecoder::AudioDecoder(int rate, int channels, int frame_size)
    : sample_rate_(rate), channel_num_(channels), frame_size_(frame_size) {}
AudioDecoder::~AudioDecoder() {
  if (opus_decoder_) opus_decoder_destroy(opus_decoder_);
}
int AudioDecoder::Init() {
  if (opus_decoder_) opus_decoder_destroy(opus_decoder_);
  int error = OPUS_OK;
  opus_decoder_ = opus_decoder_create(sample_rate_, channel_num_, &error);
  if (error != OPUS_OK || !opus_decoder_) return -1;
  pcm_.resize(sample_rate_ * 120 / 1000 * channel_num_);
  have_sequence_ = false;
  recovery_stats_ = {};
  return 0;
}
int AudioDecoder::DecodeFrame(
    const uint8_t* data, size_t size, int samples, bool fec,
    const std::function<void(uint8_t*, int)>& on_frame) {
  if (!opus_decoder_ || !on_frame ||
      size > std::numeric_limits<opus_int32>::max() || samples <= 0 ||
      size_t(samples) * channel_num_ > pcm_.size())
    return -1;
  const int decoded =
      opus_decode(opus_decoder_, data, static_cast<opus_int32>(size),
                  pcm_.data(), samples, fec ? 1 : 0);
  if (decoded < 0) return -1;
  on_frame(reinterpret_cast<uint8_t*>(pcm_.data()),
           decoded * channel_num_ * sizeof(opus_int16));
  return 0;
}
int AudioDecoder::Decode(const uint8_t* data, size_t size,
                         std::function<void(uint8_t*, int)> on_frame) {
  if (!data || !size) return -1;
  return DecodeFrame(data, size, sample_rate_ * 120 / 1000, false, on_frame);
}
int AudioDecoder::DecodePacket(const uint8_t* data, size_t size,
                               uint16_t sequence, uint32_t timestamp,
                               std::function<void(uint8_t*, int)> on_frame) {
  if (!opus_decoder_ || !data || !size || !on_frame || size > 1275) return -1;
  const int samples = opus_packet_get_nb_samples(
      data, static_cast<opus_int32>(size), sample_rate_);
  if (samples <= 0 || samples > sample_rate_ * 120 / 1000) return -1;
  if (have_sequence_) {
    const uint16_t distance = uint16_t(sequence - last_sequence_);
    if (!distance || distance >= 0x8000) {
      ++recovery_stats_.late_or_duplicate_packets;
      return 0;
    }
    // RTP timestamps always use 48 kHz, independently of decoder sample rate.
    const uint32_t expected =
        last_timestamp_ + uint32_t(last_samples_ * (48000 / sample_rate_));
    const uint32_t missing_ticks = timestamp - expected;
    if (distance > 1 && distance <= 7 && missing_ticks > 0 &&
        missing_ticks <= 2880 && missing_ticks % (distance - 1) == 0) {
      const uint32_t ticks = missing_ticks / (distance - 1);
      if (ticks == 120 || ticks == 240 || ticks == 480 || ticks == 960 ||
          ticks == 1920 || ticks == 2880) {
        const int lost_samples = int(ticks * sample_rate_ / 48000);
        for (uint16_t i = 1; i < distance; ++i) {
          const bool use_fec = i == distance - 1;
          if (DecodeFrame(use_fec ? data : nullptr, use_fec ? size : 0,
                          lost_samples, use_fec, on_frame) != 0)
            return -1;
          if (use_fec)
            ++recovery_stats_.fec_attempts;
          else
            ++recovery_stats_.plc_frames;
        }
      } else {
        opus_decoder_ctl(opus_decoder_, OPUS_RESET_STATE);
        ++recovery_stats_.discontinuities;
      }
    } else if (distance > 1 || missing_ticks != 0) {
      // Long outages / sender restarts must not create an unbounded PCM burst.
      opus_decoder_ctl(opus_decoder_, OPUS_RESET_STATE);
      ++recovery_stats_.discontinuities;
    }
  }
  if (DecodeFrame(data, size, sample_rate_ * 120 / 1000, false, on_frame) != 0)
    return -1;
  have_sequence_ = true;
  last_sequence_ = sequence;
  last_timestamp_ = timestamp;
  last_samples_ = samples;
  return 0;
}
}  // namespace minirtc
