#include "data_channel_receive.h"

#include <chrono>
#include <vector>

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
  rtp_data_receiver_ = std::make_unique<RtpDataReceiver>(ice_io_statistics_);

  if (use_reliable_) {
    const bool is_file_model = (channel_name_.find("file") != std::string::npos);
    rtp_data_sender_ = std::make_unique<RtpDataSender>(ice_io_statistics_, is_file_model);
    rtp_packetizer_ = RtpPacketizer::Create(payload_type, ssrc_);

    rtp_data_sender_->SetSendDataFunc([this](const char* data,
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

    rtp_data_sender_->Start();
  }

  rtp_data_receiver_->SetOnReceiveData(
      [this](const char* data, size_t size) -> void {
        if (!use_reliable_) {
          if (on_receive_data_) {
            on_receive_data_(data, size);
          }
          return;
        }

        if (!InitKcp()) {
          LOG_ERROR("InitKcp failed in KCP frame path");
          return;
        }

        {
          std::lock_guard<std::mutex> lock(kcp_mutex_);
          int ret = ikcp_input(kcp_, data, static_cast<long>(size));
          if (ret < 0) {
            LOG_ERROR("ikcp_input failed, ret={}, size={}, conv={}", ret, size,
                      kcp_->conv);
            return;
          }

          uint32_t now = GetCurrentTimeMs();
          ikcp_update(kcp_, now);
        }

        TryReceiveKcpData();
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
  // Stop KCP update timer first
  if (kcp_update_timer_) {
    kcp_update_timer_->Stop();
    kcp_update_timer_.reset();
  }

  if (rtp_data_sender_) {
    rtp_data_sender_->Stop();
    rtp_data_sender_ = nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(kcp_mutex_);
    if (kcp_) {
      ikcp_release(kcp_);
      kcp_ = nullptr;
    }
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
  // Double-checked locking pattern
  if (kcp_) {
    return true;
  }

  std::lock_guard<std::mutex> lock(kcp_mutex_);
  // Check again after acquiring lock
  if (kcp_) {
    return true;
  }

  uint32_t conv = ssrc_;
  if (conv == 0) {
    conv =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) & 0xffffffffu);
    LOG_ERROR("KCP conv fallback to object address, channel={}", channel_name_);
  }

  kcp_ = ikcp_create(conv, this);
  if (!kcp_) {
    LOG_ERROR("Failed to create KCP for data channel [{}]", channel_name_);
    return false;
  }

  // Create and start periodic update timer for this KCP instance
  // The timer will also trigger TryReceiveKcpData() to continuously read data
  kcp_update_timer_ = std::make_unique<KcpUpdateTimerReceive>(
      kcp_, kcp_mutex_, channel_name_, [this]() { this->TryReceiveKcpData(); });

  if (channel_name_.find("file") != std::string::npos) {
    LOG_INFO("KCP initialized for file channel [{}], kcp params: nodelay=1, 2, 2, 1, wndsize=2048, 2048", 
             channel_name_);
    ikcp_nodelay(kcp_, 1, 2, 2, 1);
    ikcp_wndsize(kcp_, 2048, 2048);
    kcp_update_timer_->SetPeriod(std::chrono::milliseconds(2));
  } else {
    LOG_INFO("KCP initialized for data channel [{}], kcp params: nodelay=1, 10, 2, 1, wndsize=256, 256", 
             channel_name_);
    ikcp_nodelay(kcp_, 1, 10, 2, 1);
    ikcp_wndsize(kcp_, 256, 256);
  }
  ikcp_setmtu(kcp_, 1200);
  kcp_->output = &DataChannelReceive::KcpOutputCallback;

  kcp_update_timer_->Start();

  LOG_INFO("KCP initialized for data channel [{}], conv={}, ssrc={}",
           channel_name_, conv, ssrc_);
  return true;
}

int DataChannelReceive::OnKcpOutput(const char* data, int len) {
  if (!rtp_data_sender_ || !rtp_packetizer_) {
    LOG_ERROR("OnKcpOutput called before initialization");
    return -1;
  }

  std::vector<std::unique_ptr<RtpPacket>> rtp_packets =
      rtp_packetizer_->Build((uint8_t*)data, len, 0, true);

  if (rtp_packets.size() > 1) {
    LOG_ERROR(
        "KCP output segment split into {} RTP packets (violates rule 15), "
        "len={}, conv={}, channel={}",
        rtp_packets.size(), len, kcp_->conv, channel_name_);
  }

  rtp_data_sender_->Enqueue(rtp_packets);

  return len;
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

void DataChannelReceive::TryReceiveKcpData() {
  if (!kcp_ || !on_receive_data_) {
    return;
  }

  // Collect all received data while holding the lock
  std::vector<std::vector<char>> received_messages;
  int total_received = 0;

  {
    std::lock_guard<std::mutex> lock(kcp_mutex_);
    while (true) {
      // Get next message size
      int peek_size = ikcp_peeksize(kcp_);
      if (peek_size < 0) {
        break;
      }

      std::vector<char> buffer(static_cast<size_t>(peek_size));
      int recv_len =
          ikcp_recv(kcp_, buffer.data(), static_cast<int>(buffer.size()));
      if (recv_len <= 0) {
        break;
      }

      total_received += recv_len;
      // Resize buffer to actual received size and store for later delivery
      buffer.resize(static_cast<size_t>(recv_len));
      received_messages.push_back(std::move(buffer));
    }

    if (total_received > 0) {
      ikcp_update(kcp_, GetCurrentTimeMs());
    }
  }

  // LOG_TRACE("Receiver:\n Send Queue (snd_queue): {}\n Send Buffer (snd_buf): {} \n Recv Buffer (rcv_buf): {} \n Recv Queue (rcv_queue): {}", 
  //   kcp_->nsnd_que, kcp_->nsnd_buf, kcp_->nrcv_buf, kcp_->nrcv_que);

  // Deliver all received messages outside the lock
  for (const auto& message : received_messages) {
    if (on_receive_data_) {
      on_receive_data_(message.data(), message.size());
    }
  }
}

}  // namespace minirtc