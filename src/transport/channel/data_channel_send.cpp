#include "data_channel_send.h"

#include <chrono>

#include "log.h"

namespace {
uint32_t GetCurrentTimeMs() {
  using namespace std::chrono;
  return static_cast<uint32_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
}
}  // namespace

namespace minirtc {

DataChannelSend::DataChannelSend() {}

DataChannelSend::~DataChannelSend() { Destroy(); }

DataChannelSend::DataChannelSend(
    const std::string& channel_name, std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics)
    : channel_name_(channel_name),
      ice_agent_(ice_agent),
      ice_io_statistics_(ice_io_statistics),
      rtp_data_sender_(std::make_unique<RtpDataSender>(ice_io_statistics)) {}

void DataChannelSend::Initialize(rtp::PAYLOAD_TYPE payload_type,
                                 std::shared_ptr<PacedSender> packet_sender) {
  paced_sender_ = packet_sender;
  rtp_packetizer_ =
      RtpPacketizer::Create(payload_type, rtp_data_sender_->GetSsrc());

  rtp_data_sender_->SetSendDataFunc(
      [this](const char* data, size_t size) -> int {
        if (!ice_agent_) {
          LOG_ERROR("ice_agent_ is nullptr");
          return -1;
        }

        auto ice_state = ice_agent_->GetIceState();

        if (ICE_STATE_DESTROYED == ice_state) {
          return -2;
        }

        ice_io_statistics_->UpdateDataOutboundBytes((uint32_t)size);
        return ice_agent_->Send(data, size);
      });

  rtp_data_sender_->Start();
}

void DataChannelSend::Destroy() {
  if (rtp_data_sender_) {
    rtp_data_sender_->Stop();
  }

  if (kcp_) {
    ikcp_release(kcp_);
    kcp_ = nullptr;
  }
}

int DataChannelSend::SendData(const char* data, size_t size) {
  if (!rtp_data_sender_ || !rtp_packetizer_) {
    LOG_ERROR("DataChannelSend not initialized");
    return -1;
  }

  std::vector<std::unique_ptr<RtpPacket>> rtp_packets = rtp_packetizer_->Build(
      reinterpret_cast<uint8_t*>(const_cast<char*>(data)),
      static_cast<uint32_t>(size), 0, true);
  // paced_sender_->EnqueueRtpPackets(rtp_packets, 0);
  rtp_data_sender_->Enqueue(rtp_packets);

  return 0;
}

int DataChannelSend::SendReliableData(const char* data, size_t size) {
  if (!rtp_data_sender_ || !rtp_packetizer_) {
    LOG_ERROR("DataChannelSend not initialized");
    return -1;
  }

  if (!InitKcp()) {
    return -1;
  }

  int ret = ikcp_send(kcp_, data, static_cast<int>(size));
  if (ret < 0) {
    LOG_ERROR("ikcp_send failed, ret={}", ret);
    return ret;
  }

  ikcp_update(kcp_, GetCurrentTimeMs());
  return 0;
}

bool DataChannelSend::InitKcp() {
  if (kcp_) {
    return true;
  }

  uint32_t conv = GetSsrc();
  if (conv == 0) {
    conv =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xffffffffu);
  }

  kcp_ = ikcp_create(conv, this);
  if (!kcp_) {
    LOG_ERROR("Failed to create KCP for data channel [{}]", channel_name_);
    return false;
  }

  ikcp_nodelay(kcp_, 1, 10, 2, 1);
  ikcp_wndsize(kcp_, 128, 128);
  ikcp_setmtu(kcp_, 1200);
  kcp_->output = &DataChannelSend::KcpOutputCallback;

  LOG_INFO("KCP initialized for data channel [{}], conv={}", channel_name_,
           conv);
  return true;
}

int DataChannelSend::OnKcpOutput(const char* data, int len) {
  if (!rtp_data_sender_ || !rtp_packetizer_) {
    LOG_ERROR("OnKcpOutput called before initialization");
    return -1;
  }

  // KCP frame format
  //  0      1      2        3        4 ...
  // +------+------+--------+--------+-------------------+
  // | 'K'  | 'C'  | ver=1 | flags  | KCP segment bytes |
  // +------+------+--------+--------+-------------------+
  std::vector<uint8_t> framed;
  framed.resize(4 + static_cast<size_t>(len));
  framed[0] = 'K';
  framed[1] = 'C';
  framed[2] = 0x01;  // version
  framed[3] = 0x00;  // flags, reserved
  memcpy(framed.data() + 4, data, static_cast<size_t>(len));

  std::vector<std::unique_ptr<RtpPacket>> rtp_packets = rtp_packetizer_->Build(
      framed.data(), static_cast<uint32_t>(framed.size()), 0, true);
  rtp_data_sender_->Enqueue(rtp_packets);

  return len;
}

int DataChannelSend::KcpOutputCallback(const char* buf, int len, ikcpcb* kcp,
                                       void* user) {
  (void)kcp;
  if (!user || !buf || len <= 0) {
    return 0;
  }

  auto* self = static_cast<DataChannelSend*>(user);
  return self->OnKcpOutput(buf, len);
}
}  // namespace minirtc