#ifndef _VIDEO_TOOLBOX_DECODER_H_
#define _VIDEO_TOOLBOX_DECODER_H_

#include <memory>

#include "video_decoder.h"

class VideoToolboxDecoder : public VideoDecoder {
 public:
  VideoToolboxDecoder(std::shared_ptr<SystemClock> clock);
  ~VideoToolboxDecoder();

  int Init() override;
  int Decode(std::unique_ptr<ReceivedFrame> received_frame,
             std::function<void(const DecodedFrame*)> on_receive_decoded_frame)
      override;
  std::string GetDecoderName() override;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

#endif  // _VIDEO_TOOLBOX_DECODER_H_
