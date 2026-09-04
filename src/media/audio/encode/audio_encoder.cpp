#include "audio_encoder.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "log.h"

#define MAX_PACKET_SIZE 4000

namespace minirtc {

AudioEncoder::AudioEncoder(int sample_rate, int channel_num, int frame_size)
    : sample_rate_(sample_rate),
      channel_num_(channel_num),
      frame_size_(frame_size) {}

AudioEncoder::~AudioEncoder() {
  if (opus_encoder_) {
    opus_encoder_destroy(opus_encoder_);
    opus_encoder_ = nullptr;
  }
}

int AudioEncoder::Init(const MediaCodecConfig& config) {
  int err;
  opus_encoder_ = opus_encoder_create(sample_rate_, channel_num_,
                                      OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK || opus_encoder_ == nullptr) {
    LOG_ERROR("Create opus encoder failed: {}", opus_strerror(err));
    return -1;
  }

  opus_encoder_ctl(opus_encoder_, OPUS_SET_LSB_DEPTH(16));
  opus_encoder_ctl(opus_encoder_,
                   OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));

  return 0;
}

int AudioEncoder::Encode(
    const uint8_t* data, size_t size,
    EncodedAudioCallback on_encoded_audio_buffer) {
  if (!data || size == 0 || !on_encoded_audio_buffer) {
    return -1;
  }

  if (!opus_encoder_) {
    LOG_ERROR("Opus encoder not initialized");
    return -1;
  }

  // Ensure input frame size is correct
  int input_samples_per_channel = static_cast<int>(size) / (2 * channel_num_);
  if (input_samples_per_channel != frame_size_) {
    LOG_ERROR(
        "Input frame size mismatch: expected {} samples per channel, got {}",
        frame_size_, input_samples_per_channel);
    return -1;
  }

  // Local output buffer (thread-safe)
  unsigned char out_data[MAX_PACKET_SIZE] = {0};

  int ret =
      opus_encode(opus_encoder_, reinterpret_cast<const opus_int16*>(data),
                  frame_size_, out_data, MAX_PACKET_SIZE);
  if (ret < 0) {
    LOG_ERROR("Opus encode failed: %s", opus_strerror(ret));
    return -1;
  }

  return on_encoded_audio_buffer(reinterpret_cast<char*>(out_data), ret,
                                 input_samples_per_channel);
}
}  // namespace minirtc