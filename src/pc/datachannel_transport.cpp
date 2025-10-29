#include "datachannel_transport.h"

namespace minirtc {

Stream::Stream(std::shared_ptr<::rtc::Track> track,
               std::shared_ptr<::rtc::RtcpSrReporter> sender)
    : track_(track), sender_(sender) {}

DataChannelTransport::DataChannelTransport(
    std::shared_ptr<::rtc::PeerConnection> peer_connection)
    : peer_connection_(peer_connection) {}

DataChannelTransport::~DataChannelTransport() {}

}  // namespace minirtc