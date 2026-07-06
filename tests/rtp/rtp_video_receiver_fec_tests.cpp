#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include "clock/system_clock.h"
#include "received_frame.h"
#include "rtp_fec.h"
#include "rtp_packetizer_h264.h"
#include "rtp_video_receiver.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
  }
}

std::vector<uint8_t> MakeFrame(size_t size) {
  std::vector<uint8_t> frame(size);
  frame[0] = 0x65;
  for (size_t i = 1; i < frame.size(); ++i) {
    frame[i] = static_cast<uint8_t>((i * 19 + 23) & 0xFF);
  }
  return frame;
}

std::vector<std::unique_ptr<minirtc::RtpPacket>> BuildFecRtpPackets(
    const std::vector<uint8_t>& frame) {
  minirtc::RtpPacketizerH264 packetizer(0x10203040);
  minirtc::FecConfig fec_config;
  fec_config.enabled = true;
  fec_config.protect_keyframes_only = false;
  fec_config.repair_ratio = 0.5;
  fec_config.symbol_size = 128;
  packetizer.SetFecConfig(fec_config);

  return packetizer.Build(const_cast<uint8_t*>(frame.data()),
                          static_cast<uint32_t>(frame.size()), 12345, false);
}

void ReceiverRecoversFecFrameWithOneMissingSourceSymbol() {
  std::vector<uint8_t> frame = MakeFrame(777);
  auto packets = BuildFecRtpPackets(frame);

  auto clock = std::make_shared<minirtc::SystemClock>();
  minirtc::RtpVideoReceiver receiver(clock);
  receiver.SetSendDataFunc([](const char*, size_t) { return 0; });

  std::atomic<int> callback_count{0};
  std::vector<uint8_t> received_frame;
  receiver.SetOnReceiveCompleteFrame(
      [&](std::unique_ptr<minirtc::ReceivedFrame> received) {
        received_frame.assign(received->Buffer(),
                              received->Buffer() + received->Size());
        callback_count.fetch_add(1);
      });
  receiver.Start();

  for (auto& packet : packets) {
    minirtc::RtpFecPacket fec_packet;
    Expect(minirtc::ParseRtpFecPacket(*packet, &fec_packet),
           "packet should parse as FEC packet");
    if ((fec_packet.header.flags & minirtc::kH264FecFlagRepair) == 0 &&
        fec_packet.header.symbol_id == 2) {
      continue;
    }
    receiver.InsertRtpPacket(*packet);
  }

  for (int i = 0; i < 100 && callback_count.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  receiver.StopRtcp();
  receiver.Stop();

  Expect(callback_count.load() == 1,
         "FEC recovered frame should trigger one callback");
  Expect(received_frame == frame,
         "receiver callback should get exact recovered H264 bytes");
}

}  // namespace

int main() {
  ReceiverRecoversFecFrameWithOneMissingSourceSymbol();
  std::cout << "rtp_video_receiver_fec_tests passed" << std::endl;
  return 0;
}
