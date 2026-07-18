#include "minirtc_connection.h"

#include "log.h"
#include "nlohmann/json.hpp"

namespace minirtc {

using nlohmann::json;

MiniRTCConnection::MiniRTCConnection(std::shared_ptr<SystemClock> clock,
                                     std::shared_ptr<WsClient> ws,
                                     const ConnectionInfo& info,
                                     const MediaStreamIds& media_stream_ids,
                                     const ConnectionCallbacks& callbacks)
    : clock_(clock),
      ws_(ws),
      info_(info),
      media_stream_ids_(media_stream_ids),
      callbacks_(callbacks) {}

MiniRTCConnection::~MiniRTCConnection() {}

int MiniRTCConnection::Init() {
  on_ice_status_change_ = [this](std::string ice_status,
                                 const std::string& user_id) {
    if ("connecting" == ice_status) {
      callbacks_.on_connection_status(ConnectionStatus::Connecting,
                                      user_id.data(), user_id.size(),
                                      callbacks_.user_data);
    } else if ("gathering" == ice_status) {
      callbacks_.on_connection_status(ConnectionStatus::Gathering,
                                      user_id.data(), user_id.size(),
                                      callbacks_.user_data);
    } else if ("disconnected" == ice_status) {
      callbacks_.on_connection_status(ConnectionStatus::Disconnected,
                                      user_id.data(), user_id.size(),
                                      callbacks_.user_data);
    } else if ("connected" == ice_status) {
      // std::string transmission_id = std::string(user_id, user_id_size);
      // is_ice_transport_ready_ = true;
      // callbacks_.on_connection_status(ConnectionStatus::Connected,
      // user_id.data(),
      //                       user_id.size(), callbacks_.user_data);
      // b_force_i_frame_ = true;
      LOG_INFO("Ice connected");
    } else if ("ready" == ice_status) {
      is_ice_transport_ready_ = true;
      b_force_i_frame_ = true;
      LOG_INFO("Ice ready");
      callbacks_.on_connection_status(ConnectionStatus::Connected,
                                      user_id.data(), user_id.size(),
                                      callbacks_.user_data);
    } else if ("closed" == ice_status) {
      is_ice_transport_ready_ = false;
      LOG_INFO("Ice closed");
      callbacks_.on_connection_status(ConnectionStatus::Closed, user_id.data(),
                                      user_id.size(), callbacks_.user_data);
    } else if ("failed" == ice_status) {
      is_ice_transport_ready_ = false;
      // if (offer_peer_ && try_rejoin_with_turn_) {
      //   if (reconnect_count_ > 3) {
      //     LOG_INFO("Recreate with turn exceed max count, give up");
      //     callbacks_.on_connection_status(ConnectionStatus::Failed,
      //                                     user_id.data(), user_id.size(),
      //                                     callbacks_.user_data);
      //   } else {
      //     LOG_INFO(
      //         "Ice failed, destroy ice agent and rereate it with TURN
      //         enabled");

      //     info_.turn_mode = TurnMode::TurnAutoUdpTcp;
      //     info_.reliable_ice = false;

      //     if (offer_peer_) {
      //       reconnect_count_++;
      //       IceWorkMsg msg;
      //       msg.type = IceWorkMsg::Type::RetryWithTurn;
      //       msg.transmission_id = remote_transmission_id_;
      //       msg.user_id_list = user_id_list_;
      //       PushIceWorkMsg(msg);
      //     }
      //   }
      // } else {
      LOG_INFO("Ice failed");
      callbacks_.on_connection_status(ConnectionStatus::Failed, user_id.data(),
                                      user_id.size(), callbacks_.user_data);
      // }
    } else {
      is_ice_transport_ready_ = false;
      LOG_INFO("Unknown ice state [{}]", ice_status);
    }
  };

  return 0;
}

int MiniRTCConnection::SendVideoFrame(const XVideoFrame* video_frame,
                                      const char* stream_id) {
  if (!ice_transport_) {
    return -1;
  }

  ice_transport_->SendVideoFrame(video_frame, stream_id);

  return 0;
}

int MiniRTCConnection::RequestVideoKeyFrame(const char* stream_id) {
  if (!ice_transport_) {
    return -1;
  }

  return ice_transport_->RequestVideoKeyFrame(stream_id ? stream_id : "");
}

int MiniRTCConnection::RequestAllVideoKeyFrames() {
  if (!ice_transport_) {
    return -1;
  }

  return ice_transport_->RequestAllVideoKeyFrames();
}

int MiniRTCConnection::ReleaseAllIceTransmission() {
  if (ice_transport_) {
    ice_transport_->DestroyIceTransmission();
  }

  is_ice_transport_ready_ = false;
  return 0;
}

int MiniRTCConnection::SendAudioFrame(const char* data, size_t size,
                                      const char* stream_id) {
  if (!ice_transport_) {
    return -1;
  }

  if (!is_ice_transport_ready_) {
    return -1;
  }

  ice_transport_->SendAudioFrame(data, size, stream_id);

  return 0;
}

int MiniRTCConnection::SendDataFrame(const char* data, size_t size,
                                     const char* stream_id) {
  if (!ice_transport_) {
    return -1;
  }

  if (!is_ice_transport_ready_) {
    return -1;
  }

  ice_transport_->SendDataFrame(data, size, stream_id);

  return 0;
}

int MiniRTCConnection::SendReliableDataFrame(const char* data, size_t size,
                                             const char* stream_id) {
  if (!ice_transport_) {
    return -1;
  }

  if (!is_ice_transport_ready_) {
    return -1;
  }

  ice_transport_->SendReliableDataFrame(data, size, stream_id);

  return 0;
}

void MiniRTCConnection::ProcessIceWorkMsg(const IceWorkMsg& msg) {
  switch (msg.type) {
    case IceWorkMsg::Type::Login: {
      break;
    }
    case IceWorkMsg::Type::RetryWithTurn:
    case IceWorkMsg::Type::UserJoinTransmission: {
      std::string remote_user_id = msg.remote_user_id;
      std::string transmission_id = msg.transmission_id;
      LOG_INFO("[{}] Receive notification: user id [{}] join transmission",
               info_.user_id, remote_user_id);

      if (remote_user_id == info_.user_id) {
        break;
      }

      if (ice_transport_) {
        ice_transport_->DestroyIceTransmission();
      }

      ice_transport_ = std::make_shared<IceTransport>(
          clock_, true, transmission_id, info_.user_id, remote_user_id, ws_,
          on_ice_status_change_, callbacks_.user_data);

      ice_transport_->SetLocalCapabilities(
          info_.hardware_acceleration, info_.trickle_ice, info_.reliable_ice,
          info_.turn_mode, info_.enable_srtp, info_.video_quality,
          info_.av1_encoding ? rtp::PAYLOAD_TYPE::AV1 : rtp::PAYLOAD_TYPE::H264,
          video_payload_types_, audio_payload_types_);

      ice_transport_->SetOnReceiveFunc(callbacks_.on_receive_video_frame,
                                       callbacks_.on_receive_audio_buffer,
                                       callbacks_.on_receive_data_buffer);

      ice_transport_->SetOnReceiveNetStatusReportFunc(
          callbacks_.on_net_status_report);

      ice_transport_->InitIceTransmission(
          info_.stun_server_ip, info_.stun_server_port, info_.turn_server_ip,
          info_.turn_server_port, info_.turn_server_username,
          info_.turn_server_password);

      for (auto& stream_id : media_stream_ids_.video) {
        ice_transport_->AddVideoStream(stream_id);
      }
      for (auto& stream_id : media_stream_ids_.audio) {
        ice_transport_->AddAudioStream(stream_id);
      }
      for (auto& stream_id_kv : media_stream_ids_.data) {
        ice_transport_->AddDataStream(stream_id_kv.first, stream_id_kv.second);
      }

      if (info_.trickle_ice) {
        ice_transport_->SendOffer();
      } else {
        ice_transport_->GatherCandidates();
      }

      break;
    }
    case IceWorkMsg::Type::UserLeaveTransmission: {
      std::string remote_user_id = msg.remote_user_id;
      LOG_INFO("[{}] Receive notification: user id [{}] leave transmission",
               (void*)this, remote_user_id);
      if (ice_transport_) {
        ice_transport_->DestroyIceTransmission();
        is_ice_transport_ready_ = false;
        LOG_INFO("Terminate transmission to user [{}]", remote_user_id);
      }
      break;
    }
    case IceWorkMsg::Type::Offer: {
      std::string transmission_id = msg.transmission_id;
      std::string remote_user_id = msg.remote_user_id;
      if (ice_transport_) {
        ice_transport_->DestroyIceTransmission();
        is_ice_transport_ready_ = false;
      }

      // Enable TURN for answer peer by default
      ice_transport_ = std::make_shared<IceTransport>(
          clock_, false, transmission_id, info_.user_id, remote_user_id, ws_,
          on_ice_status_change_, callbacks_.user_data);

      ice_transport_->SetLocalCapabilities(
          info_.hardware_acceleration, info_.trickle_ice, info_.reliable_ice,
          info_.turn_mode, info_.enable_srtp, info_.video_quality,
          info_.av1_encoding ? rtp::PAYLOAD_TYPE::AV1 : rtp::PAYLOAD_TYPE::H264,
          video_payload_types_, audio_payload_types_);

      ice_transport_->SetOnReceiveFunc(callbacks_.on_receive_video_frame,
                                       callbacks_.on_receive_audio_buffer,
                                       callbacks_.on_receive_data_buffer);

      ice_transport_->SetOnReceiveNetStatusReportFunc(
          callbacks_.on_net_status_report);

      ice_transport_->InitIceTransmission(
          info_.stun_server_ip, info_.stun_server_port, info_.turn_server_ip,
          info_.turn_server_port, info_.turn_server_username,
          info_.turn_server_password);
      ice_transport_->SetTransmissionId(transmission_id);

      for (auto& stream_id : media_stream_ids_.video) {
        ice_transport_->AddVideoStream(stream_id);
      }
      for (auto& stream_id : media_stream_ids_.audio) {
        ice_transport_->AddAudioStream(stream_id);
      }
      for (auto& stream_id_kv : media_stream_ids_.data) {
        ice_transport_->AddDataStream(stream_id_kv.first, stream_id_kv.second);
      }

      std::string remote_sdp = msg.remote_sdp;
      int ret = ice_transport_->SetRemoteSdp(remote_sdp);
      if (0 != ret) {
        LOG_ERROR("Set remote sdp failed");
        break;
      }

      if (info_.trickle_ice) {
        sdp_without_cands_ = remote_sdp;
        ice_transport_->SendAnswer();
      }
      ice_transport_->GatherCandidates();

      break;
    }
    case IceWorkMsg::Type::Answer: {
      std::string remote_user_id = msg.remote_user_id;
      std::string remote_sdp = msg.remote_sdp;

      if (ice_transport_) {
        int ret = ice_transport_->SetRemoteSdp(remote_sdp);
        if (0 != ret) {
          LOG_ERROR("Set remote sdp failed");
          break;
        }

        if (info_.trickle_ice) {
          sdp_without_cands_ = remote_sdp;
          ice_transport_->GatherCandidates();
        }
      }

      break;
    }
    case IceWorkMsg::Type::NewCandidate: {
      std::string transmission_id = msg.transmission_id;
      std::string new_candidate = msg.new_candidate;
      std::string remote_user_id = msg.remote_user_id;

      // LOG_INFO("[{}] receive new candidate from [{}]:[{}]", info_.user_id,
      //          remote_user_id, new_candidate);
      if (ice_transport_) {
        ice_transport_->SetRemoteSdp(sdp_without_cands_ + new_candidate);
      }
      break;
    }
    default: {
      break;
    }
  }
}

}  // namespace minirtc
