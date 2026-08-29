/*
 * @Author: DI JUNKUN
 * @Date: 2025-05-28
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_TOOLBOX_DECODER_H_
#define _VIDEO_TOOLBOX_DECODER_H_

#include <memory>

#include "video_decoder.h"

namespace minirtc {

class VideoToolboxDecoder : public MediaCodec {
 public:
  explicit VideoToolboxDecoder(std::shared_ptr<SystemClock> clock,
                               bool native_video_output = false);
  ~VideoToolboxDecoder();

  int Init() override;
  int Decode(std::unique_ptr<ReceivedFrame> received_frame,
             std::function<void(const DecodedFrame*)> on_receive_decoded_frame)
      override;
  std::string GetDecoderName() const override;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace minirtc

#endif
