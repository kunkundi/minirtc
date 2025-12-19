#include "data_channel_receive.h"

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

DataChannelReceive::DataChannelReceive() {}

DataChannelReceive::DataChannelReceive(
    const std::string& channel_name, uint32_t ssrc,
    std::shared_ptr<IceAgent> ice_agent,
    std::shared_ptr<IOStatistics> ice_io_statistics,
    std::function<void(const char*, size_t)> on_receive_data, bool use_reliable)
    : channel_name_(channel_name),
      ssrc_(ssrc),
      use_reliable_(use_reliable),
      ice_agent_(ice_agent),
      ice_io_statistics_(ice_io_statistics),
      on_receive_data_(on_receive_data) {}

DataChannelReceive::~DataChannelReceive() { Destroy(); }

void DataChannelReceive::Initialize(rtp::PAYLOAD_TYPE payload_type) {
  (void)payload_type;

  rtp_data_receiver_ = std::make_unique<RtpDataReceiver>(ice_io_statistics_);

  rtp_data_receiver_->SetOnReceiveData([this](const char* data,
                                              size_t size) -> void {
    if (!use_reliable_) {
      if (on_receive_data_) {
        on_receive_data_(data, size);
      }
      return;
    } else {
      LOG_DEBUG("KCP init receive");
    }

    if (!InitKcp()) {
      LOG_ERROR("InitKcp failed in KCP frame path");
      return;
    }

    RtpPacket rtp_packet;
    rtp_packet.Build((uint8_t*)data, (uint32_t)size);
    const char* kcp_seg = reinterpret_cast<const char*>(rtp_packet.Payload());
    long kcp_len = static_cast<long>(rtp_packet.PayloadSize());

    LOG_ERROR("KCP received size [{}]", kcp_len);
    int ret = ikcp_input(kcp_, kcp_seg, kcp_len);
    if (ret < 0) {
      LOG_ERROR("ikcp_input failed, ret={}", ret);
      return;
    }

    ikcp_update(kcp_, GetCurrentTimeMs());

    char buffer[1500];
    int recv_len = 0;
    while ((recv_len = ikcp_recv(kcp_, buffer, sizeof(buffer))) > 0) {
      if (on_receive_data_) {
        on_receive_data_(buffer, static_cast<size_t>(recv_len));
      }
    }
  });

  rtp_data_receiver_->SetSendDataFunc([this](const char* data,
                                             size_t size) -> int {
    if (!ice_agent_) {
      LOG_ERROR("ice_agent_ is nullptr");
      return -1;
    }

    auto ice_state = ice_agent_->GetIceState();

    if (ICE_STATE_NULLPTR == ice_state || ICE_STATE_DESTROYED == ice_state) {
      LOG_ERROR("Ice is not connected, state = [{}]", (int)ice_state);
      return -2;
    }

    ice_io_statistics_->UpdateDataOutboundBytes((uint32_t)size);
    return ice_agent_->Send(data, size);
  });
}

void DataChannelReceive::Destroy() {
  if (kcp_) {
    ikcp_release(kcp_);
    kcp_ = nullptr;
  }
}

int DataChannelReceive::OnReceiveRtpPacket(const char* data, size_t size) {
  if (ice_io_statistics_) {
    ice_io_statistics_->UpdateDataInboundBytes((uint32_t)size);
  }

  if (rtp_data_receiver_) {
    RtpPacket rtp_packet;
    rtp_packet.Build((uint8_t*)data, (uint32_t)size);
    rtp_data_receiver_->InsertRtpPacket(rtp_packet);
  }

  return 0;
}

bool DataChannelReceive::InitKcp() {
  if (kcp_) {
    return true;
  }

  uint32_t conv = ssrc_;
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

  kcp_->output = &DataChannelReceive::KcpOutputCallback;

  LOG_INFO("KCP initialized for data channel [{}], conv={}", channel_name_,
           conv);
  return true;
}

int DataChannelReceive::OnKcpOutput(const char* data, int len) {
  if (!ice_agent_) {
    LOG_ERROR("OnKcpOutput: ice_agent_ is nullptr");
    return -1;
  }

  auto ice_state = ice_agent_->GetIceState();
  if (ICE_STATE_NULLPTR == ice_state || ICE_STATE_DESTROYED == ice_state) {
    LOG_ERROR("OnKcpOutput: Ice is not connected, state = [{}]",
              (int)ice_state);
    return 0;
  }

  if (ice_io_statistics_) {
    ice_io_statistics_->UpdateDataOutboundBytes(static_cast<uint32_t>(len));
  }

  return ice_agent_->Send(data, static_cast<size_t>(len));
}

int DataChannelReceive::KcpOutputCallback(const char* buf, int len, ikcpcb* kcp,
                                          void* user) {
  (void)kcp;
  if (!user || !buf || len <= 0) {
    return 0;
  }

  auto* self = static_cast<DataChannelReceive*>(user);
  return self->OnKcpOutput(buf, len);
}

}  // namespace minirtc