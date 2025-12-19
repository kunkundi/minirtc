#include "datachannel_connection.h"

#include "common.h"
#include "log.h"
#include "nlohmann/json.hpp"

namespace minirtc {

using nlohmann::json;

template <class T>
std::weak_ptr<T> make_weak_ptr(std::shared_ptr<T> ptr) {
  return ptr;
}

DataChannelConnection::DataChannelConnection(
    std::shared_ptr<SystemClock> clock, std::shared_ptr<WsClient> ws,
    const ConnectionInfo& info, const MediaStreamIds& media_stream_ids,
    const ConnectionCallbacks& callbacks)
    : clock_(clock),
      ws_(ws),
      info_(info),
      media_stream_ids_(media_stream_ids),
      callbacks_(callbacks) {
  // disable av1 encoding
  info_.av1_encoding = false;
  InitLogger(::rtc::LogLevel::Verbose,
             [](::rtc::LogLevel level, std::string message) {
               switch (level) {
                 case ::rtc::LogLevel::Verbose:
                   LOG_TRACE(message);
                   break;
                 case ::rtc::LogLevel::Debug:
                   LOG_DEBUG(message);
                   break;
                 case ::rtc::LogLevel::Info:
                   LOG_INFO(message);
                   break;
                 case ::rtc::LogLevel::Warning:
                   LOG_WARN(message);
                   break;
                 case ::rtc::LogLevel::Error:
                   LOG_ERROR(message);
                 case ::rtc::LogLevel::Fatal:
                   LOG_FATAL(message);
                   break;
                 case ::rtc::LogLevel::None:
                   break;
                 default:
                   break;
               }
             });
}

DataChannelConnection::~DataChannelConnection() {}

int DataChannelConnection::Init() {
  std::string stun_server = "stun:" + info_.stun_server_ip + ":" +
                            std::to_string(info_.stun_server_port);
  std::string turn_server =
      "turn:" + info_.turn_server_username + ":" + info_.turn_server_password +
      "@" + info_.turn_server_ip + ":" +
      std::to_string(info_.turn_server_port) + "?transport=udp";
  std::string turn_server_tcp =
      "turn:" + info_.turn_server_username + ":" + info_.turn_server_password +
      "@" + info_.turn_server_ip + ":" +
      std::to_string(info_.turn_server_port) + "?transport=tcp";

  peer_connection_config_.iceServers.emplace_back(stun_server);
  peer_connection_config_.iceServers.emplace_back(turn_server);
  peer_connection_config_.iceServers.emplace_back(turn_server_tcp);
  // use trickle ice by default
  peer_connection_config_.disableAutoNegotiation = true;
  peer_connection_config_.disableAutoGathering = true;

  return 0;
}

int DataChannelConnection::ReleaseAllIceTransmission() {
  if (dc_transport_) {
    auto pc = dc_transport_->GetPeerConnection();
    if (pc) {
      pc->close();
    }
  }
  return 0;
}

int DataChannelConnection::SendVideoFrame(const XVideoFrame* video_frame,
                                          const char* stream_id) {
  if (!dc_ready_) {
    return -1;
  }

  if (!dc_transport_) {
    return -1;
  }

  dc_transport_->SendVideoFrame(video_frame, stream_id);
  return 0;
}

int DataChannelConnection::SendAudioFrame(const char* data, size_t size,
                                          const char* stream_id) {
  if (!dc_ready_) {
    return -1;
  }

  if (!dc_transport_) {
    return -1;
  }

  dc_transport_->SendAudioFrame(data, size, stream_id);
  return 0;
}

int DataChannelConnection::SendDataFrame(const char* data, size_t size,
                                         const char* stream_id) {
  if (!dc_ready_) {
    return -1;
  }

  if (!dc_transport_) {
    return -1;
  }

  dc_transport_->SendDataFrame(data, size, stream_id);
  return 0;
}

int DataChannelConnection::SendReliableDataFrame(const char* data, size_t size,
                                                 const char* stream_id) {
  return SendDataFrame(data, size, stream_id);
}

void DataChannelConnection::ProcessIceWorkMsg(const IceWorkMsg& msg) {
  switch (msg.type) {
    case IceWorkMsg::Type::Login: {
      break;
    }
    case IceWorkMsg::Type::RetryWithTurn:
    case IceWorkMsg::Type::UserJoinTransmission: {
      std::string remote_user_id = msg.remote_user_id;
      LOG_INFO("[{}] Receive notification: user id [{}] join transmission",
               (void*)this, remote_user_id);
      LOG_INFO("Create transmission to user [{}]", remote_user_id);
      dc_transport_ = CreateDataChannelConnection(
          peer_connection_config_, clock_, make_weak_ptr(ws_), true,
          remote_user_id, remote_user_id);

      break;
    }
    case IceWorkMsg::Type::UserLeaveTransmission: {
      std::string remote_user_id = msg.remote_user_id;
      LOG_INFO("[{}] Receive notification: user id [{}] leave transmission",
               (void*)this, remote_user_id);

      LOG_INFO("Terminate transmission to user [{}]", remote_user_id);

      break;
    }
    case IceWorkMsg::Type::Offer: {
      std::string transmission_id = msg.transmission_id;
      std::string remote_user_id = msg.remote_user_id;

      LOG_INFO("Receive offer from user [{}]", remote_user_id);

      dc_transport_ = CreateDataChannelConnection(
          peer_connection_config_, clock_, make_weak_ptr(ws_), false,
          transmission_id, remote_user_id);

      ::rtc::Description remote_sdp(msg.remote_sdp,
                                    ::rtc::Description::Type::Offer);
      // LOG_INFO("Set remote description: {}", msg.remote_sdp.c_str());

      auto& pc = dc_transport_->GetPeerConnection();
      pc->setRemoteDescription(remote_sdp);

      pc->setLocalDescription();

      // send answer
      std::string answer = pc->localDescription().value().generateSdp();

      json message = {{"type", "answer"},
                      {"transmission_id", transmission_id},
                      {"user_id", info_.user_id},
                      {"remote_user_id", remote_user_id},
                      {"sdp", answer.c_str()}};

      ws_->Send(message.dump());
      // LOG_INFO("[{}] send answer to [{}]: {}", info_.user_id,
      // remote_user_id,
      //          message.dump());

      pc->gatherLocalCandidates();

      std::shared_ptr<::rtc::DataChannel> dc;
      pc->onDataChannel([&](std::shared_ptr<::rtc::DataChannel> _dc) {
        std::cout << "[Got a DataChannel with label: " << _dc->label() << "]"
                  << std::endl;
        dc = _dc;

        dc->onClosed([&]() {
          std::cout << "[DataChannel closed: " << dc->label() << "]"
                    << std::endl;
        });

        dc->onMessage([](auto data) {
          if (std::holds_alternative<std::string>(data)) {
            std::cout << "[Received message: " << std::get<std::string>(data)
                      << "]" << std::endl;
          }
        });
      });

      break;
    }
    case IceWorkMsg::Type::Answer: {
      std::string remote_user_id = msg.remote_user_id;
      ::rtc::Description remote_sdp(msg.remote_sdp, "answer");

      auto& pc = dc_transport_->GetPeerConnection();
      if (pc) {
        // LOG_INFO("Set remote description: {}", msg.remote_sdp.c_str());
        pc->setRemoteDescription(remote_sdp);

        pc->gatherLocalCandidates();
      }

      break;
    }
    case IceWorkMsg::Type::NewCandidate: {
      std::string transmission_id = msg.transmission_id;
      std::string new_candidate = msg.new_candidate;
      std::string remote_user_id = msg.remote_user_id;

      auto& pc = dc_transport_->GetPeerConnection();
      if (pc) {
        pc->addRemoteCandidate(new_candidate);
      }

      break;
    }
    case IceWorkMsg::Type::NewCandidateMid: {
      // std::string transmission_id = msg.transmission_id;
      // std::string remote_user_id = msg.remote_user_id;
      // std::string candidate = msg.candidate;
      // std::string mid = msg.mid;
      // LOG_INFO("Receive new candidate from [{}]: {}, mid: {}",
      // remote_user_id,
      //          candidate, mid);

      //   auto& pc = dc_transport_->GetPeerConnection();
      //   if (pc) {
      //     LOG_INFO("Add remote candidate: {}, mid: {}", candidate, mid);
      //     pc->addRemoteCandidate(::rtc::Candidate(candidate, mid));
      //   }
      break;
    }
    default: {
      break;
    }
  }
}

//
std::shared_ptr<DataChannelTransport>
DataChannelConnection::CreateDataChannelConnection(
    const ::rtc::Configuration& config, std::shared_ptr<SystemClock> clock,
    std::weak_ptr<WsClient> wws, bool offer_peer, std::string transmission_id,
    std::string remote_user_id) {
  offer_peer_ = offer_peer;
  auto peer_connection = std::make_shared<::rtc::PeerConnection>(config);
  auto dc_transport = std::make_shared<DataChannelTransport>(
      clock, peer_connection, info_.user_id, remote_user_id, offer_peer_);

  peer_connection->onLocalDescription([this, transmission_id, remote_user_id,
                                       wws](::rtc::Description description) {
    // std::string local_sdp = std::string(description);
    // if (offer_peer_ && description.type() ==
    // ::rtc::Description::Type::Offer)
    // {
    //   json message = {{"type", "offer"},
    //                   {"transmission_id", transmission_id},
    //                   {"user_id", info_.user_id},
    //                   {"remote_user_id", remote_user_id},
    //                   {"sdp", local_sdp.c_str()}};

    //   // Gathering complete, send offer
    //   if (auto ws = wws.lock()) {
    //     ws->Send(message.dump());
    //     // LOG_INFO("[{}] send offer to [{}]: {}", info_.user_id,
    //     remote_user_id,
    //     //          message.dump());
    //   }
    // } else if (!offer_peer_ &&
    //            description.type() ==
    //            ::rtc::Description::Type::Answer) {
    //   json message = {{"type", "answer"},
    //                   {"transmission_id", transmission_id},
    //                   {"user_id", info_.user_id},
    //                   {"remote_user_id", remote_user_id},
    //                   {"sdp", local_sdp.c_str()}};

    //   // Gathering complete, send answer
    //   if (auto ws = wws.lock()) {
    //     ws->Send(message.dump());
    //     // LOG_INFO("[{}] send answer to [{}]: {}", info_.user_id,
    //     remote_user_id,
    //     //          message.dump());
    //   }
    // }
  });

  peer_connection->onLocalCandidate(
      [this, transmission_id, remote_user_id, wws](::rtc::Candidate candidate) {
        json message = {{"type", "new_candidate_mid"},
                        {"transmission_id", transmission_id},
                        {"user_id", info_.user_id},
                        {"remote_user_id", remote_user_id},
                        {"candidate", candidate.candidate()},
                        {"mid", candidate.mid()}};

        if (auto ws = wws.lock()) {
          // LOG_INFO("[{}] send new candidate to [{}]: {}", info_.user_id,
          //          remote_user_id, message.dump());
          ws->Send(message.dump());
        }
      });

  peer_connection->onStateChange(
      [this, remote_user_id](::rtc::PeerConnection::State state) {
        ConnectionStatus ice_state;
        switch (state) {
          case ::rtc::PeerConnection::State::New:
            ice_state = ConnectionStatus::Connecting;
            LOG_INFO("PeerConnection state: New");
            break;
          case ::rtc::PeerConnection::State::Connecting:
            ice_state = ConnectionStatus::Connecting;
            LOG_INFO("PeerConnection state: Checking");
            break;
          case ::rtc::PeerConnection::State::Connected:
            ice_state = ConnectionStatus::Connected;
            dc_ready_ = true;
            LOG_INFO("PeerConnection state: Connected");
            break;
          case ::rtc::PeerConnection::State::Disconnected:
            ice_state = ConnectionStatus::Disconnected;
            dc_ready_ = false;
            LOG_INFO("PeerConnection state: Disconnected");
            break;
          case ::rtc::PeerConnection::State::Failed:
            ice_state = ConnectionStatus::Failed;
            dc_ready_ = false;
            LOG_ERROR("PeerConnection state: Failed");
            break;
          case ::rtc::PeerConnection::State::Closed:
            ice_state = ConnectionStatus::Closed;
            dc_ready_ = false;
            LOG_INFO("PeerConnection state: Closed");
            break;
          default:
            ice_state = ConnectionStatus::Failed;
            LOG_FATAL("PeerConnection state: Unknown");
            break;
        }
        callbacks_.on_connection_status(ice_state, remote_user_id.data(),
                                        remote_user_id.size(),
                                        callbacks_.user_data);
      });

  peer_connection->onGatheringStateChange(
      [this, wpc = make_weak_ptr(peer_connection), transmission_id,
       remote_user_id, wws](::rtc::PeerConnection::GatheringState state) {
        switch (state) {
          case ::rtc::PeerConnection::GatheringState::New:
            LOG_INFO("Gathering state: New");
            break;
          case ::rtc::PeerConnection::GatheringState::InProgress:
            LOG_INFO("Gathering state: InProgress");
            break;
          case ::rtc::PeerConnection::GatheringState::Complete:
            LOG_INFO("Gathering state: Complete");
            break;
        }
      });

  if (offer_peer_) {
    for (auto& video_stream_id : media_stream_ids_.video) {
      dc_transport->AddVideoStream(
          video_stream_id,
          AddVideo(peer_connection,
                   info_.av1_encoding ? rtp::PAYLOAD_TYPE::AV1
                                      : rtp::PAYLOAD_TYPE::H264,
                   GenerateUniqueSsrc(), video_stream_id, video_stream_id,
                   [video_stream_id, wc = make_weak_ptr(dc_transport)]() {
                     LOG_INFO("Video stream {} opened", video_stream_id);
                   }));
    }

    dc_transport->CreateCodecs(
        clock_,
        info_.av1_encoding ? rtp::PAYLOAD_TYPE::AV1 : rtp::PAYLOAD_TYPE::H264,
        info_.hardware_acceleration);

    for (auto& audio_stream_id : media_stream_ids_.audio) {
      dc_transport->AddAudioStream(
          audio_stream_id,
          AddAudio(peer_connection, rtp::PAYLOAD_TYPE::OPUS,
                   GenerateUniqueSsrc(), "audio-stream", audio_stream_id,
                   [audio_stream_id, wc = make_weak_ptr(dc_transport)]() {
                     LOG_INFO("Audio stream {} opened", audio_stream_id);
                   }));
    }

    for (auto& data_stream_id_kv : media_stream_ids_.data) {
      std::string data_stream_id = data_stream_id_kv.first;
      dc_transport->AddDataStream(
          data_stream_id,
          AddData(peer_connection, rtp::PAYLOAD_TYPE::DATA,
                  GenerateUniqueSsrc(), "data-stream", data_stream_id,
                  callbacks_,
                  [data_stream_id, wc = make_weak_ptr(dc_transport)]() {
                    LOG_INFO("Data stream {} opened", data_stream_id);
                  }));
    }

    dc_transport->GetPeerConnection()->setLocalDescription();
    std::string offer = dc_transport->GetPeerConnection()
                            ->localDescription()
                            .value()
                            .generateSdp();
    json message = {{"type", "offer"},
                    {"transmission_id", transmission_id},
                    {"user_id", info_.user_id},
                    {"remote_user_id", remote_user_id},
                    {"sdp", offer.c_str()}};

    if (auto ws = wws.lock()) {
      ws->Send(message.dump());
      // LOG_INFO("[{}] send offer to [{}]: {}", info_.user_id, remote_user_id,
      //          message.dump());
    }
  }

  return dc_transport;
};

std::shared_ptr<Stream> DataChannelConnection::AddVideo(
    const std::shared_ptr<::rtc::PeerConnection> peer_connection,
    const rtp::PAYLOAD_TYPE payload_type, const uint32_t ssrc,
    const std::string cname, const std::string msid,
    const std::function<void(void)> onOpen) {
  auto video = ::rtc::Description::Video(cname);
  if (payload_type == rtp::PAYLOAD_TYPE::AV1) {
    video.addAV1Codec((int)payload_type);
    video.addSSRC(ssrc, cname, msid, cname);
    video.setDirection(::rtc::Description::Direction::SendRecv);
    auto track = peer_connection->addTrack(video);
    // create RTP configuration
    auto rtpConfig = std::make_shared<::rtc::RtpPacketizationConfig>(
        ssrc, cname, payload_type, ::rtc::AV1RtpPacketizer::VideoClockRate);
    // create packetizer
    auto packetizer = std::make_shared<::rtc::AV1RtpPacketizer>(
        ::rtc::AV1RtpPacketizer::Packetization::Obu, rtpConfig);
    // add RTCP SR handler
    auto srReporter = std::make_shared<::rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // add RTCP NACK handler
    auto nackResponder = std::make_shared<::rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // set handler
    track->setMediaHandler(packetizer);
    track->onOpen(onOpen);
    auto trackData = std::make_shared<Stream>(track, srReporter);
    return trackData;
  } else {
    video.addH264Codec((int)payload_type);
    video.addSSRC(ssrc, cname, msid, cname);
    video.setDirection(::rtc::Description::Direction::SendRecv);
    auto track = peer_connection->addTrack(video);
    // create RTP configuration
    auto rtpConfig = std::make_shared<::rtc::RtpPacketizationConfig>(
        ssrc, cname, payload_type, ::rtc::H264RtpPacketizer::VideoClockRate);
    // create packetizer
    auto packetizer = std::make_shared<::rtc::H264RtpPacketizer>(
        ::rtc::NalUnit::Separator::StartSequence, rtpConfig);
    // add RTCP SR handler
    auto srReporter = std::make_shared<::rtc::RtcpSrReporter>(rtpConfig);
    packetizer->addToChain(srReporter);
    // add RTCP NACK handler
    auto nackResponder = std::make_shared<::rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);
    // set handler
    track->setMediaHandler(packetizer);
    track->onOpen(onOpen);
    auto trackData = std::make_shared<Stream>(track, srReporter);
    return trackData;
  }
}

std::shared_ptr<Stream> DataChannelConnection::AddAudio(
    const std::shared_ptr<::rtc::PeerConnection> peer_connection,
    const rtp::PAYLOAD_TYPE payload_type, const uint32_t ssrc,
    const std::string cname, const std::string msid,
    const std::function<void(void)> onOpen) {
  auto audio = ::rtc::Description::Audio(cname);
  audio.addOpusCodec((int)payload_type);
  audio.addSSRC(ssrc, cname, msid, cname);
  audio.setDirection(::rtc::Description::Direction::SendRecv);
  auto track = peer_connection->addTrack(audio);
  // create RTP configuration
  auto rtpConfig = std::make_shared<::rtc::RtpPacketizationConfig>(
      ssrc, cname, (int)payload_type,
      ::rtc::OpusRtpPacketizer::DefaultClockRate);
  // create packetizer
  auto packetizer = std::make_shared<::rtc::OpusRtpPacketizer>(rtpConfig);
  // add RTCP SR handler
  auto srReporter = std::make_shared<::rtc::RtcpSrReporter>(rtpConfig);
  packetizer->addToChain(srReporter);
  // add RTCP NACK handler
  auto nackResponder = std::make_shared<::rtc::RtcpNackResponder>();
  packetizer->addToChain(nackResponder);
  // set handler
  track->setMediaHandler(packetizer);
  track->onOpen(onOpen);
  auto trackData = std::make_shared<Stream>(track, srReporter);
  return trackData;
}

std::shared_ptr<::rtc::DataChannel> DataChannelConnection::AddData(
    const std::shared_ptr<::rtc::PeerConnection> peer_connection,
    const rtp::PAYLOAD_TYPE payload_type, const uint32_t ssrc,
    const std::string cname, const std::string msid,
    const ConnectionCallbacks& callbacks,
    const std::function<void(void)> onOpen) {
  auto dc = peer_connection->createDataChannel("ping-pong");
  dc->onOpen(onOpen);

  dc->onClosed([dc]() {
    std::cout << "[DataChannel closed: " << dc->label() << "]" << std::endl;
  });

  dc->onMessage([msid, wdc = std::weak_ptr(dc),
                 callbacks](std::variant<::rtc::binary, std::string> msg) {
    if (callbacks.on_receive_data_buffer) {
      if (std::holds_alternative<std::string>(msg)) {
        const auto& str = std::get<std::string>(msg);
        // LOG_INFO("[DataChannel receive: {}]", str);
        callbacks.on_receive_data_buffer(str.data(), str.size(), msid.data(),
                                         msid.size(), callbacks.user_data);
      }
    }
  });

  return dc;
}

}  // namespace minirtc