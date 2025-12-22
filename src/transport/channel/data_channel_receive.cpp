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
    rtp_data_sender_ = std::make_unique<RtpDataSender>(ice_io_statistics_);
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

        int ret = ikcp_input(kcp_, data, static_cast<long>(size));
        if (ret < 0) {
          LOG_ERROR("ikcp_input failed, ret={}, size={}, conv={}", ret, size,
                    kcp_->conv);
          return;
        }

        uint32_t now = GetCurrentTimeMs();
        ikcp_update(kcp_, now);

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
    LOG_ERROR("KCP conv fallback to object address, channel={}", channel_name_);
  }

  kcp_ = ikcp_create(conv, this);
  if (!kcp_) {
    LOG_ERROR("Failed to create KCP for data channel [{}]", channel_name_);
    return false;
  }

  ikcp_nodelay(kcp_, 1, 10, 2, 1);
  ikcp_wndsize(kcp_, 256, 256);
  ikcp_setmtu(kcp_, 1200);
  kcp_->output = &DataChannelReceive::KcpOutputCallback;

  // Create and start periodic update timer for this KCP instance
  // The timer will also trigger TryReceiveKcpData() to continuously read data
  kcp_update_timer_ = std::make_unique<KcpUpdateTimerReceive>(
      kcp_, channel_name_, [this]() { this->TryReceiveKcpData(); });
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

void DataChannelReceive::ProcessReceiveBuffer() {
  if (!on_receive_data_) {
    return;
  }

  // Process complete messages from the accumulated buffer
  // KCP is a byte stream, so we need to parse message boundaries
  // For file transfer protocol, we parse FileChunkHeader to determine
  // boundaries For other data (e.g., JSON), we deliver as-is if no file magic
  // is found

  constexpr size_t kMinHeaderSize =
      31;  // sizeof(FileChunkHeader) = 4+4+8+8+4+2+1 = 31
  constexpr uint32_t kFileChunkMagic = 0x4A4E544D;  // 'JNTM'

  // Keep processing until no more complete messages can be extracted
  // and no more data can be read from KCP
  constexpr size_t kRecvBufferSize = 64 * 1024;
  std::vector<char> temp_buffer(kRecvBufferSize);
  bool made_progress = true;

  while (made_progress) {
    made_progress = false;

    // Process all complete messages in the buffer
    while (receive_buffer_.size() >= kMinHeaderSize) {
      // Check magic to find message start
      uint32_t magic = 0;
      memcpy(&magic, receive_buffer_.data(), sizeof(uint32_t));

      if (magic != kFileChunkMagic) {
        // Not a file chunk, might be other data (e.g., JSON)
        // Try to find the magic in the buffer (skip up to 1KB to avoid scanning
        // too much)
        bool found = false;
        size_t search_limit =
            std::min<size_t>(1024, receive_buffer_.size() - kMinHeaderSize);
        for (size_t i = 1; i <= search_limit; ++i) {
          memcpy(&magic, receive_buffer_.data() + i, sizeof(uint32_t));
          if (magic == kFileChunkMagic) {
            // Found magic, remove data before it (might be incomplete JSON or
            // other data)
            LOG_ERROR(
                "ProcessReceiveBuffer: found file magic at offset {}, removing "
                "{} "
                "bytes of non-file data, conv={}",
                i, i, kcp_->conv);
            receive_buffer_.erase(receive_buffer_.begin(),
                                  receive_buffer_.begin() + i);
            found = true;
            break;
          }
        }
        if (!found) {
          // No magic found in reasonable range, might be non-file data
          // Deliver a chunk (up to 64KB) and let application layer handle it
          size_t deliver_size =
              std::min<size_t>(64 * 1024, receive_buffer_.size());
          if (on_receive_data_) {
            on_receive_data_(receive_buffer_.data(), deliver_size);
          }
          receive_buffer_.erase(receive_buffer_.begin(),
                                receive_buffer_.begin() + deliver_size);
          continue;
        }
        // Continue to parse the message after removing non-file data
      }

      // Parse FileChunkHeader (packed structure, 31 bytes)
      // Use pragma pack for cross-platform compatibility (MSVC and GCC/Clang)
#pragma pack(push, 1)
      struct FileChunkHeader {
        uint32_t magic;
        uint32_t file_id;
        uint64_t offset;
        uint64_t total_size;
        uint32_t chunk_size;
        uint16_t name_len;
        uint8_t flags;
      } header;
#pragma pack(pop)

      if (receive_buffer_.size() < sizeof(FileChunkHeader)) {
        break;  // Not enough data for header
      }

      memcpy(&header, receive_buffer_.data(), sizeof(FileChunkHeader));

      size_t header_and_name =
          sizeof(FileChunkHeader) + static_cast<size_t>(header.name_len);
      size_t total_message_size =
          header_and_name + static_cast<size_t>(header.chunk_size);

      if (receive_buffer_.size() < total_message_size) {
        // Incomplete message, wait for more data
        LOG_ERROR(
            "ProcessReceiveBuffer: incomplete message, need {} bytes, have {} "
            "bytes, file_id={}, chunk_size={}, conv={}",
            total_message_size, receive_buffer_.size(), header.file_id,
            header.chunk_size, kcp_->conv);
        break;
      }

      // Complete message found, deliver it
      if (on_receive_data_) {
        on_receive_data_(receive_buffer_.data(), total_message_size);
      }

      LOG_ERROR(
          "ProcessReceiveBuffer: delivered complete message, size={}, "
          "file_id={}, offset={}, remaining_buffer={}, conv={}",
          total_message_size, header.file_id, header.offset,
          receive_buffer_.size() - total_message_size, kcp_->conv);

      // Remove processed message from buffer
      receive_buffer_.erase(receive_buffer_.begin(),
                            receive_buffer_.begin() + total_message_size);
      made_progress = true;
    }

    // After processing messages, try to read more data from KCP
    // This is important for large files where multiple chunks are sent
    // KCP might have more data ready after we processed the current buffer
    if (kcp_) {
      // Try to read more data from KCP (might be ready after processing)
      while (true) {
        int recv_len = ikcp_recv(kcp_, temp_buffer.data(),
                                 static_cast<int>(temp_buffer.size()));
        if (recv_len <= 0) {
          break;
        }

        size_t old_size = receive_buffer_.size();
        receive_buffer_.resize(old_size + recv_len);
        memcpy(receive_buffer_.data() + old_size, temp_buffer.data(), recv_len);

        // Update KCP after receiving
        ikcp_update(kcp_, GetCurrentTimeMs());
        made_progress = true;

        LOG_ERROR(
            "ProcessReceiveBuffer: read additional {} bytes from KCP, "
            "total_buffer={}, conv={}",
            recv_len, receive_buffer_.size(), kcp_->conv);
      }
    }
  }
}

void DataChannelReceive::TryReceiveKcpData() {
  if (!kcp_ || !on_receive_data_) {
    return;
  }

  // 按 KCP 推荐方式：不停调用 ikcp_recv，直到返回 < 0
  // 不再依赖 ikcp_peeksize（在 stream 模式下语义不稳定）
  constexpr size_t kRecvBufferSize = 64 * 1024;  // 单次读取最大 64KB
  std::vector<char> buffer(kRecvBufferSize);
  int total_received = 0;

  while (true) {
    int recv_len =
        ikcp_recv(kcp_, buffer.data(), static_cast<int>(buffer.size()));
    if (recv_len <= 0) {
      break;  // No more data or error
    }

    total_received += recv_len;
    LOG_ERROR(
        "TryReceiveKcpData: recv_len={}, total_received={}, conv={}, "
        "first_bytes=0x{:02X}{:02X}{:02X}{:02X}",
        recv_len, total_received, kcp_->conv,
        recv_len > 0 ? static_cast<uint8_t>(buffer[0]) : 0,
        recv_len > 1 ? static_cast<uint8_t>(buffer[1]) : 0,
        recv_len > 2 ? static_cast<uint8_t>(buffer[2]) : 0,
        recv_len > 3 ? static_cast<uint8_t>(buffer[3]) : 0);

    // Accumulate data in receive buffer (KCP is a byte stream, not
    // message-based). We need to accumulate until we have complete messages.
    size_t old_size = receive_buffer_.size();
    receive_buffer_.resize(old_size + recv_len);
    memcpy(receive_buffer_.data() + old_size, buffer.data(), recv_len);
  }

  // Update KCP once after receiving all available data
  // This is more efficient than updating after each recv
  if (total_received > 0) {
    ikcp_update(kcp_, GetCurrentTimeMs());
  }

  // Process accumulated data to extract complete messages
  if (total_received > 0) {
    LOG_ERROR(
        "KCP recv: total_received={} bytes in TryReceiveKcpData, conv={}, "
        "buffer_size={}",
        total_received, kcp_->conv, receive_buffer_.size());
    ProcessReceiveBuffer();
  }
}

}  // namespace minirtc