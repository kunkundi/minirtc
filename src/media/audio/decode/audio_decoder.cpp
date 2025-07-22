#include "audio_decoder.h"

#include "log.h"

#define MAX_FRAME_SIZE 6 * 960
#define CHANNELS 1

namespace minirtc {
namespace {
unsigned char pcm_bytes[MAX_FRAME_SIZE * CHANNELS * 2];
opus_int16 out_data[MAX_FRAME_SIZE * CHANNELS];
}  // namespace

AudioDecoder::AudioDecoder(int sample_rate, int channel_num, int frame_size)
    : sample_rate_(sample_rate),
      channel_num_(channel_num),
      frame_size_(frame_size) {}

AudioDecoder::~AudioDecoder() {
  if (opus_decoder_) {
    opus_decoder_destroy(opus_decoder_);
  }
}

int AudioDecoder::Init() {
  int err;
  opus_decoder_ = opus_decoder_create(sample_rate_, channel_num_, &err);
  opus_decoder_ctl(opus_decoder_, OPUS_SET_LSB_DEPTH(16));
  // opus_decoder_ctl(opus_decoder_, OPUS_SET_INBAND_FEC(1));

  if (err < 0 || opus_decoder_ == NULL) {
    LOG_ERROR("Create opus opus_decoder_ failed");
    return -1;
  }

  // pcm_file = fopen("decode.pcm", "wb+");
  // pcm_file1 = fopen("decode1.pcm", "wb+");

  return 0;
}

int AudioDecoder::Decode(
    const uint8_t* data, size_t size,
    std::function<void(uint8_t*, int)> on_receive_decoded_frame) {
  // LOG_ERROR("input opus size = {}", size);
  auto frame_size = opus_decode(opus_decoder_, data, (opus_int32)size, out_data,
                                MAX_FRAME_SIZE, 0);

  if (frame_size < 0) {
    LOG_ERROR("Decode opus frame failed");
    return -1;
  }

  // LOG_ERROR("frame_size = {}", frame_size);

  // for (auto i = 0; i < channel_num_ * frame_size; i++) {
  //   pcm_bytes[2 * i] = out_data[i] & 0xFF;
  //   pcm_bytes[2 * i + 1] = (out_data[i] >> 8) & 0xFF;
  // }

  // fwrite(pcm_bytes, sizeof(short), frame_size * channel_num_, pcm_file);
  // fflush(pcm_file);

  if (on_receive_decoded_frame) {
    on_receive_decoded_frame((uint8_t*)out_data,
                             frame_size * channel_num_ * sizeof(opus_int16));
  }

  return 0;
}
}  // namespace minirtc