#include "rtp_packet.h"

#include <string>

namespace minirtc {

RtpPacket::RtpPacket() {}

RtpPacket::RtpPacket(size_t size) : buffer_(size) {}

RtpPacket::RtpPacket(const uint8_t* buffer, uint32_t size)
    : buffer_(buffer, size) {}

RtpPacket::RtpPacket(const RtpPacket& rtp_packet) = default;

RtpPacket::RtpPacket(RtpPacket&& rtp_packet) = default;

RtpPacket& RtpPacket::operator=(const RtpPacket& rtp_packet) = default;

RtpPacket& RtpPacket::operator=(RtpPacket&& rtp_packet) = default;

RtpPacket::~RtpPacket() = default;

bool RtpPacket::Build(const uint8_t* buffer, uint32_t size) {
  if (!Parse(buffer, size)) {
    LOG_WARN("RtpPacket::Build: parse failed");
    return false;
  }
  buffer_.SetData(buffer, size);
  size_ = size;
  return true;
}

bool RtpPacket::Parse(const uint8_t* buffer, uint32_t size) {
  payload_offset_ = 0;
  csrcs_.clear();
  extensions_.clear();

  if (size < kFixedHeaderSize) {
    LOG_WARN("RtpPacket::Parse: size is too small");
    return false;
  }

  // 1st byte
  version_ = (buffer[payload_offset_] >> 6) & 0x03;
  if (version_ != kRtpVersion) {
    LOG_WARN("RtpPacket::Parse: version is not qual to kRtpVersion");
    return false;
  }
  has_padding_ = (buffer[payload_offset_] >> 5) & 0x01;
  has_extension_ = (buffer[payload_offset_] >> 4) & 0x01;
  csrc_count_ = buffer[payload_offset_] & 0x0f;
  if (csrc_count_ > kMaxRtpCsrcSize) {
    LOG_WARN("RtpPacket::Parse: csrc count is too large");
    return false;
  }
  payload_offset_ += 1;

  // 2nd byte
  marker_ = (buffer[payload_offset_] >> 7) & 0x01;
  payload_type_ = buffer[payload_offset_] & 0x7f;
  payload_offset_ += 1;

  // 3rd byte and 4th byte
  sequence_number_ =
      (buffer[payload_offset_] << 8) | buffer[payload_offset_ + 1];
  payload_offset_ += 2;

  // 5th byte to 8th byte
  timestamp_ = (buffer[payload_offset_] << 24) |
               (buffer[payload_offset_ + 1] << 16) |
               (buffer[payload_offset_ + 2] << 8) | buffer[payload_offset_ + 3];
  payload_offset_ += 4;

  // 9th byte to 12th byte
  ssrc_ = (buffer[payload_offset_] << 24) |
          (buffer[payload_offset_ + 1] << 16) |
          (buffer[payload_offset_ + 2] << 8) | buffer[payload_offset_ + 3];

  payload_offset_ = kFixedHeaderSize;

  if (kFixedHeaderSize + csrc_count_ * 4 > size) {
    LOG_WARN("RtpPacket::Parse: csrc count is too large");
    return false;
  }
  // csrc
  for (uint32_t csrc_index = 0; csrc_index < csrc_count_; csrc_index++) {
    uint32_t csrc = (buffer[payload_offset_ + csrc_index * 4] << 24) |
                    (buffer[payload_offset_ + 1 + csrc_index * 4] << 16) |
                    (buffer[payload_offset_ + 2 + csrc_index * 4] << 8) |
                    buffer[payload_offset_ + 3 + csrc_index * 4];
    csrcs_.push_back(csrc);
  }

  payload_offset_ = kFixedHeaderSize + csrc_count_ * 4;
  if (payload_offset_ > size) {
    LOG_WARN("RtpPacket::Parse: payload offset is too large");
    return false;
  }

  // extensions
  if (has_extension_) {
    if (payload_offset_ + 4 > size) {
      LOG_WARN("RtpPacket::Parse: extension profile is too large");
      return false;
    }
    extension_profile_ =
        (buffer[payload_offset_] << 8) | buffer[payload_offset_ + 1];
    extension_len_ =
        (buffer[payload_offset_ + 2] << 8) | buffer[payload_offset_ + 3];
    payload_offset_ += 4;

    if (payload_offset_ + extension_len_ * 4 > size) {
      LOG_WARN("RtpPacket::Parse: extension len is too large");
      return false;
    }

    size_t total_ext_len = extension_len_ * 4;
    if (extension_profile_ == kOneByteExtensionProfileId) {
      size_t offset = payload_offset_;
      const size_t extension_end = payload_offset_ + total_ext_len;
      while (offset < extension_end) {
        const uint8_t id = buffer[offset] >> 4;
        if (id == 0) {
          ++offset;
          continue;
        }
        if (id == 15) {
          break;
        }

        const uint8_t len = (buffer[offset] & 0x0F) + 1;
        if (offset + 1 + len > extension_end) {
          LOG_WARN("RtpPacket::Parse: extension data is too large");
          return false;
        }
        Extension extension;
        extension.id = id;
        extension.len = len;
        extension.data_offset = offset + 1;
        extension.data =
            std::vector<uint8_t>(buffer + extension.data_offset,
                                 buffer + extension.data_offset + len);
        extensions_.push_back(std::move(extension));
        offset += 1 + len;
      }
    }
    payload_offset_ += total_ext_len;
  }

  if (has_padding_ && payload_offset_ < size) {
    padding_size_ = buffer[size - 1];
    if (padding_size_ == 0) {
      // TODO: consider this case as invalid packet or just treat it as no
      // padding?
      has_padding_ = false;
      padding_size_ = 0;
    }
  } else {
    padding_size_ = 0;
  }

  // payload
  if (payload_offset_ + padding_size_ > size) {
    LOG_WARN("RtpPacket::Parse: payload size is too large");
    return false;
  }

  payload_size_ = size - payload_offset_ - padding_size_;

  return true;
}

bool RtpPacket::UpdateAbsoluteSendTimestamp(uint8_t extension_id,
                                            uint32_t abs_send_time) {
  if (extension_profile_ != kOneByteExtensionProfileId) {
    return false;
  }

  for (auto& extension : extensions_) {
    if (extension.id != extension_id || extension.len != 3 ||
        extension.data_offset + 3 > buffer_.size()) {
      continue;
    }

    abs_send_time &= 0x00FFFFFF;
    uint8_t* data = buffer_.MutableData() + extension.data_offset;
    data[0] = static_cast<uint8_t>(abs_send_time >> 16);
    data[1] = static_cast<uint8_t>(abs_send_time >> 8);
    data[2] = static_cast<uint8_t>(abs_send_time);
    extension.data.assign(data, data + 3);
    return true;
  }
  return false;
}

bool RtpPacket::GetAbsoluteSendTimestamp(uint32_t* abs_send_time) const {
  return abs_send_time_ext_id_.has_value() &&
         GetAbsoluteSendTimestamp(*abs_send_time_ext_id_, abs_send_time);
}

bool RtpPacket::GetAbsoluteSendTimestamp(uint8_t extension_id,
                                         uint32_t* abs_send_time) const {
  if (!abs_send_time) {
    return false;
  }
  for (const auto& extension : extensions_) {
    if (extension.id == extension_id && extension.data.size() == 3) {
      *abs_send_time = (static_cast<uint32_t>(extension.data[0]) << 16) |
                       (static_cast<uint32_t>(extension.data[1]) << 8) |
                       extension.data[2];
      return true;
    }
  }
  return false;
}
}  // namespace minirtc