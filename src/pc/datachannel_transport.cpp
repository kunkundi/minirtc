#include "datachannel_transport.h"

namespace minirtc {

DataChannelTransport::DataChannelTransport(
    std::shared_ptr<::rtc::PeerConnection> peer_connection)
    : peer_connection_(peer_connection) {}

DataChannelTransport::~DataChannelTransport() {}

}  // namespace minirtc