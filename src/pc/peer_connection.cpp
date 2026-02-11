#include "peer_connection.h"

#include <regex>

#include "INIReader.h"
#include "common.h"
#include "log.h"
#include "nlohmann/json.hpp"

namespace minirtc {

using nlohmann::json;

PeerConnection::PeerConnection() {}

PeerConnection::~PeerConnection() { user_data_ = nullptr; }

int PeerConnection::Init(PeerConnectionParams params) {
  if (inited_) {
    LOG_INFO("Peer already inited");
    return 0;
  }

  user_id_with_pwd_ = params.user_id ? params.user_id : "";
  auto at_pos = user_id_with_pwd_.find('@');
  if (at_pos != std::string::npos) {
    user_id_ = user_id_with_pwd_.substr(0, at_pos);
  } else {
    user_id_ = user_id_with_pwd_;
  }

  if (params.use_cfg_file) {
    INIReader reader(params.cfg_path);
    cfg_signal_server_ip_ = reader.Get("signal server", "ip", "-1");
    cfg_signal_server_port_ = reader.Get("signal server", "port", "-1");
    cfg_stun_server_ip_ = reader.Get("stun server", "ip", "-1");
    cfg_stun_server_port_ = reader.Get("stun server", "port", "-1");
    cfg_turn_server_ip_ = reader.Get("turn server", "ip", "");
    cfg_turn_server_port_ = reader.Get("turn server", "port", "-1");
    cfg_turn_server_username_ = reader.Get("turn server", "username", "");
    cfg_turn_server_password_ = reader.Get("turn server", "password", "");
    cfg_hardware_acceleration_ =
        reader.Get("hardware acceleration", "turn_on", "false");
    cfg_av1_encoding_ = reader.Get("av1 encoding", "turn_on", "false");
    cfg_enable_turn_ = reader.Get("enable turn", "turn_on", "false");
    cfg_enable_srtp_ = reader.Get("enable srtp", "turn_on", "true");
    cfg_video_quality_ = reader.Get("video quality", "quality", "high");

    std::regex regex("\n");

    signal_server_port_ = stoi(cfg_signal_server_port_);
    stun_server_port_ = stoi(cfg_stun_server_port_);
    turn_server_port_ = stoi(cfg_turn_server_port_);

    hardware_acceleration_ =
        cfg_hardware_acceleration_ == "true" ? true : false;
    av1_encoding_ = cfg_av1_encoding_ == "true" ? true : false;
    enable_turn_ = cfg_enable_turn_ == "true" ? true : false;
    enable_srtp_ = cfg_enable_srtp_ == "true" ? true : false;
    if (cfg_video_quality_ == "low") {
      video_quality_ = VideoQuality::QualityLow;
    } else if (cfg_video_quality_ == "medium") {
      video_quality_ = VideoQuality::QualityMedium;
    } else {
      video_quality_ = VideoQuality::QualityHigh;
    }
  } else {
    cfg_signal_server_ip_ = params.signal_server_ip;
    signal_server_port_ = params.signal_server_port;
    cfg_stun_server_ip_ = params.stun_server_ip;
    stun_server_port_ = params.stun_server_port;
    cfg_turn_server_ip_ = params.turn_server_ip;
    turn_server_port_ = params.turn_server_port;
    cfg_turn_server_username_ = params.turn_server_username;
    cfg_turn_server_password_ = params.turn_server_password;
    hardware_acceleration_ = params.hardware_acceleration;
    av1_encoding_ = params.av1_encoding;
    enable_turn_ = params.enable_turn;
    enable_srtp_ = params.enable_srtp;
    video_quality_ = params.video_quality;

    cfg_signal_server_port_ = std::to_string(signal_server_port_);
    cfg_stun_server_port_ = std::to_string(stun_server_port_);
    cfg_turn_server_port_ = std::to_string(turn_server_port_);
  }

  connection_info_.stun_server_ip = cfg_stun_server_ip_;
  connection_info_.stun_server_port = stun_server_port_;
  connection_info_.turn_server_ip = cfg_turn_server_ip_;
  connection_info_.turn_server_port = turn_server_port_;
  connection_info_.turn_server_username = cfg_turn_server_username_;
  connection_info_.turn_server_password = cfg_turn_server_password_;
  connection_info_.hardware_acceleration = hardware_acceleration_;
  connection_info_.trickle_ice = trickle_ice_;
  connection_info_.reliable_ice = reliable_ice_;
  connection_info_.enable_turn = enable_turn_;
  connection_info_.enable_srtp = enable_srtp_;
  connection_info_.av1_encoding = av1_encoding_;
  connection_info_.video_quality = video_quality_;

  LOG_INFO("Read config success, use configure file [{}]", params.use_cfg_file);

  LOG_INFO("Signal server ip [{}] port [{}]", cfg_signal_server_ip_,
           cfg_signal_server_port_);

  LOG_INFO("Stun server ip [{}] port [{}]", cfg_stun_server_ip_,
           cfg_stun_server_port_);

  if (!cfg_turn_server_ip_.empty() && 0 != turn_server_port_ &&
      !cfg_turn_server_username_.empty() &&
      !cfg_turn_server_password_.empty()) {
    LOG_INFO("Turn server ip [{}] port [{}] username [{}] password [{}]",
             cfg_turn_server_ip_, turn_server_port_, cfg_turn_server_username_,
             cfg_turn_server_password_);
  }

  LOG_INFO("Hardware accelerated codec [{}]",
           hardware_acceleration_ ? "ON" : "OFF");
  LOG_INFO("Video format [{}]", av1_encoding_ ? "AV1" : "H.264");

  on_receive_video_buffer_ = params.on_receive_video_buffer;
  on_receive_audio_buffer_ = params.on_receive_audio_buffer;
  on_receive_data_buffer_ = params.on_receive_data_buffer;

  on_receive_video_frame_ = params.on_receive_video_frame;

  on_signal_status_ = params.on_signal_status;
  on_connection_status_ = params.on_connection_status;
  on_net_status_report_ = params.on_net_status_report;
  user_data_ = params.user_data;

  connection_callbacks_.on_connection_status = params.on_connection_status;

  connection_callbacks_.on_receive_video_frame = params.on_receive_video_frame;
  connection_callbacks_.on_receive_audio_buffer =
      params.on_receive_audio_buffer;
  connection_callbacks_.on_receive_data_buffer = params.on_receive_data_buffer;

  connection_callbacks_.on_net_status_report = params.on_net_status_report;
  connection_callbacks_.user_data = user_data_;

  on_receive_ws_msg_ = [this](const std::string& msg) { ProcessSignal(msg); };

  on_ws_status_ = [this](WsStatus ws_status) {
    if (WsStatus::WsOpening == ws_status) {
      ws_status_ = WsStatus::WsOpening;
      signal_status_ = SignalStatus::SignalConnecting;
      on_signal_status_(SignalStatus::SignalConnecting, user_id_.data(),
                        user_id_.size(), user_data_);
    } else if (WsStatus::WsOpened == ws_status) {
      ws_status_ = WsStatus::WsOpened;
      LOG_INFO("Login to signal server");
      Login();
    } else if (WsStatus::WsFailed == ws_status) {
      ws_status_ = WsStatus::WsFailed;
      signal_status_ = SignalStatus::SignalFailed;
      on_signal_status_(SignalStatus::SignalFailed, user_id_.data(),
                        user_id_.size(), user_data_);
    } else if (WsStatus::WsClosed == ws_status) {
      ws_status_ = WsStatus::WsClosed;
      signal_status_ = SignalStatus::SignalClosed;
      on_signal_status_(SignalStatus::SignalClosed, user_id_.data(),
                        user_id_.size(), user_data_);
    } else if (WsStatus::WsReconnecting == ws_status) {
      ws_status_ = WsStatus::WsReconnecting;
      signal_status_ = SignalStatus::SignalReconnecting;
      on_signal_status_(SignalStatus::SignalReconnecting, user_id_.data(),
                        user_id_.size(), user_data_);
    } else if (WsStatus::WsServerClosed == ws_status) {
      ws_status_ = WsStatus::WsServerClosed;
      signal_status_ = SignalStatus::SignalServerClosed;
      on_signal_status_(SignalStatus::SignalServerClosed, user_id_.data(),
                        user_id_.size(), user_data_);
    }
  };

  clock_ = std::make_shared<SystemClock>();
  ws_transport_ = std::make_shared<WsClient>(on_receive_ws_msg_, on_ws_status_);
  uri_ = "wss://" + cfg_signal_server_ip_ + ":" + cfg_signal_server_port_;
  if (ws_transport_) {
    ws_transport_->Connect(uri_);
  }

  StartIceWorker();

  // do {
  // } while (SignalStatus::SignalConnected != GetSignalStatus());

  LOG_INFO("[{}] Init finish", user_id_);

  inited_ = true;
  return 0;
}

int PeerConnection::Login() {
  if (WsStatus::WsOpened != ws_status_) {
    LOG_ERROR("Websocket not opened");
    return -1;
  }

  int ret = 0;

  json message = {{"type", "login"}, {"user_id", user_id_with_pwd_}};

  if (ws_transport_) {
    ws_transport_->Send(message.dump());
    LOG_INFO("[{}] send login request to signal server", user_id_);
  }
  return ret;
}

int PeerConnection::Join(const std::string& transmission_id) {
  if (SignalStatus::SignalConnected != GetSignalStatus()) {
    LOG_ERROR("Signal not connected");
    return -1;
  }

  int ret = 0;

  offer_peer_ = false;
  leave_ = false;

  json message = {{"type", "join_transmission"},
                  {"user_id", user_id_},
                  {"transmission_id", transmission_id}};
  remote_transmission_id_ = transmission_id;

  if (ws_transport_) {
    ws_transport_->Send(message.dump());
    // LOG_INFO(
    //     "[{}] sends join transmission request to transmission "
    //     "id [{}]",
    //     user_id_, transmission_id);
  }

  return ret;
}

int PeerConnection::Leave(const std::string& transmission_id) {
  if (SignalStatus::SignalConnected != GetSignalStatus()) {
    LOG_ERROR("Signal not connected");
    return -1;
  }

  json message = {{"type", "user_leave_transmission"},
                  {"user_id", user_id_},
                  {"transmission_id", transmission_id}};
  if (ws_transport_) {
    ws_transport_->Send(message.dump());
    LOG_INFO("[{}] sends leave transmission [{}] notification ", user_id_,
             transmission_id);
  }

  leave_ = true;

  std::shared_lock lock(peer_connection_map_mutex_);
  for (auto& peer_connection : peer_connection_map_) {
    if (peer_connection.second) {
      peer_connection.second->ReleaseAllIceTransmission();
    }
  }

  return 0;
}

int PeerConnection::AddVideoStream(const char* stream_id) {
  media_stream_ids_.video.push_back(stream_id);
  return 0;
}

int PeerConnection::AddAudioStream(const char* stream_id) {
  media_stream_ids_.audio.push_back(stream_id);
  return 0;
}

int PeerConnection::AddDataStream(const char* stream_id, bool reliable) {
  media_stream_ids_.data.insert(std::make_pair(stream_id, reliable));
  return 0;
}

int PeerConnection::Destroy() {
  StopIceWorker();
  if (ws_transport_) {
    LOG_INFO("Close websocket");
    ws_transport_->Close();
  }

  return 0;
}

SignalStatus PeerConnection::GetSignalStatus() {
  std::lock_guard<std::mutex> l(signal_status_mutex_);
  return signal_status_;
}

int PeerConnection::SendVideoFrame(const XVideoFrame* video_frame,
                                   const char* stream_id) {
  std::shared_lock lock(peer_connection_map_mutex_);
  for (auto& peer_connection : peer_connection_map_) {
    if (peer_connection.second) {
      peer_connection.second->SendVideoFrame(video_frame, stream_id);
    }
  }

  return 0;
}

int PeerConnection::SendAudioFrame(const char* data, size_t size,
                                   const char* stream_id) {
  std::shared_lock lock(peer_connection_map_mutex_);
  for (auto& peer_connection : peer_connection_map_) {
    if (peer_connection.second) {
      peer_connection.second->SendAudioFrame(data, size, stream_id);
    }
  }

  return 0;
}

int PeerConnection::SendDataFrame(const char* data, size_t size,
                                  const char* stream_id) {
  std::shared_lock lock(peer_connection_map_mutex_);
  for (auto& peer_connection : peer_connection_map_) {
    if (peer_connection.second) {
      peer_connection.second->SendDataFrame(data, size, stream_id);
    }
  }

  return 0;
}

int PeerConnection::SendReliableDataFrame(const char* data, size_t size,
                                          const char* stream_id) {
  std::shared_lock lock(peer_connection_map_mutex_);
  for (auto& peer_connection : peer_connection_map_) {
    if (peer_connection.second) {
      peer_connection.second->SendReliableDataFrame(data, size, stream_id);
    }
  }

  return 0;
}

int PeerConnection::SendVideoFrameToPeer(const XVideoFrame* video_frame,
                                         const char* stream_id,
                                         const char* remote_peer_id,
                                         size_t remote_peer_id_size) {
  std::shared_lock lock(peer_connection_map_mutex_);
  auto it = peer_connection_map_.find(
      std::string(remote_peer_id, remote_peer_id_size));
  if (it != peer_connection_map_.end() && it->second) {
    it->second->SendVideoFrame(video_frame, stream_id);
  } else {
    LOG_WARN("SendVideoFrame to remote peer [{}] failed, peer not found",
             std::string(remote_peer_id, remote_peer_id_size));
    return -1;
  }

  return 0;
}

int PeerConnection::SendAudioFrameToPeer(const char* data, size_t size,
                                         const char* stream_id,
                                         const char* remote_peer_id,
                                         size_t remote_peer_id_size) {
  std::shared_lock lock(peer_connection_map_mutex_);
  auto it = peer_connection_map_.find(
      std::string(remote_peer_id, remote_peer_id_size));
  if (it != peer_connection_map_.end() && it->second) {
    it->second->SendAudioFrame(data, size, stream_id);
  } else {
    LOG_WARN("SendAudioFrame to remote peer [{}] failed, peer not found",
             std::string(remote_peer_id, remote_peer_id_size));
    return -1;
  }

  return 0;
}

int PeerConnection::SendDataFrameToPeer(const char* data, size_t size,
                                        const char* stream_id,
                                        const char* remote_peer_id,
                                        size_t remote_peer_id_size) {
  std::shared_lock lock(peer_connection_map_mutex_);
  auto it = peer_connection_map_.find(
      std::string(remote_peer_id, remote_peer_id_size));
  if (it != peer_connection_map_.end() && it->second) {
    it->second->SendDataFrame(data, size, stream_id);
  } else {
    LOG_WARN("SendDataFrame to remote peer [{}] failed, peer not found",
             std::string(remote_peer_id, remote_peer_id_size));
    return -1;
  }

  return 0;
}

int PeerConnection::SendReliableDataFrameToPeer(const char* data, size_t size,
                                                const char* stream_id,
                                                const char* remote_peer_id,
                                                size_t remote_peer_id_size) {
  std::shared_lock lock(peer_connection_map_mutex_);
  auto it = peer_connection_map_.find(
      std::string(remote_peer_id, remote_peer_id_size));
  if (it != peer_connection_map_.end() && it->second) {
    it->second->SendReliableDataFrame(data, size, stream_id);
  } else {
    LOG_WARN("SendReliableDataFrame to remote peer [{}] failed, peer not found",
             std::string(remote_peer_id, remote_peer_id_size));
    return -1;
  }

  return 0;
}

int64_t PeerConnection::GetSystemTimeMicros() {
  if (clock_) {
    return clock_->CurrentTimeUs();
  }
  return 0;
}

// Process signal message from signal server
void PeerConnection::ProcessSignal(const std::string& signal) {
  auto j = json::parse(signal);
  std::string type = j["type"];
  // LOG_INFO("signal type: {}", type);
  switch (HASH_STRING_PIECE(type.c_str())) {
    case "login"_H: {
      if (j["status"].get<std::string>() == "success") {
        std::string user_id_with_pwd = j["user_id"].get<std::string>();
        std::string password;

        if (user_id_with_pwd.find("@") != std::string::npos) {
          user_id_ = user_id_with_pwd.substr(0, user_id_with_pwd.find("@"));
          password = user_id_with_pwd.substr(user_id_with_pwd.find("@") + 1);
        } else {
          user_id_ = user_id_with_pwd;
          password = "";
        }

        XNetTrafficStats net_traffic_stats;
        memset(&net_traffic_stats, 0, sizeof(net_traffic_stats));

        on_net_status_report_(user_id_with_pwd.data(), user_id_with_pwd.size(),
                              TraversalMode::UnknownMode, &net_traffic_stats,
                              user_id_.data(), user_id_.size(), user_data_);
        LOG_INFO("Login success with id [{}]", user_id_);
        signal_status_ = SignalStatus::SignalConnected;
        on_signal_status_(SignalStatus::SignalConnected, user_id_.data(),
                          user_id_.size(), user_data_);
      } else if (j["status"].get<std::string>() == "fail") {
        LOG_WARN("Login failed with id [{}]", user_id_);
      }
      break;
    }
    case "transmission_id"_H: {
      if (j["status"].get<std::string>() == "success") {
        local_transmission_id_ = j["transmission_id"].get<std::string>();
        user_id_ = local_transmission_id_;
        LOG_INFO("Create transmission success with id [{}]",
                 local_transmission_id_);
      } else if (j["status"].get<std::string>() == "fail") {
        LOG_WARN("Create transmission failed with id [{}], due to [{}]",
                 local_transmission_id_,
                 j["reason"].get<std::string>().c_str());
      }
      break;
    }
    case "user_join_transmission"_H: {
      std::string transmission_id = j["transmission_id"].get<std::string>();
      std::string status = j["status"].get<std::string>();
      if (status == "failed") {
        std::string reason = j["reason"].get<std::string>();
        LOG_ERROR("{}", reason);
        if ("Incorrect password" == reason) {
          on_connection_status_(ConnectionStatus::IncorrectPassword,
                                transmission_id.data(), transmission_id.size(),
                                user_data_);
        } else if ("No such transmission id" == reason) {
          on_connection_status_(ConnectionStatus::NoSuchTransmissionId,
                                transmission_id.data(), transmission_id.size(),
                                user_data_);
        }
      } else {
        std::string remote_user_id = j["user_id"].get<std::string>();

        connection_info_.transmission_id = transmission_id;
        connection_info_.user_id = user_id_;
        connection_info_.remote_user_id = remote_user_id;

        {
          std::unique_lock lock(peer_connection_map_mutex_);
          if (peer_connection_map_.find(remote_user_id) ==
              peer_connection_map_.end()) {
            if (remote_user_id.find("web") == std::string::npos) {
              peer_connection_map_.emplace(
                  remote_user_id,
                  std::make_shared<MiniRTCConnection>(
                      clock_, ws_transport_, connection_info_,
                      media_stream_ids_, connection_callbacks_));
            } else {
              // web client use libdatachannel
              peer_connection_map_.emplace(
                  remote_user_id,
                  std::make_shared<DataChannelConnection>(
                      clock_, ws_transport_, connection_info_,
                      media_stream_ids_, connection_callbacks_));
            }

            peer_connection_map_[remote_user_id]->Init();
          } else {
            LOG_ERROR("Peer connection already exists");
            break;
          }
        }

        if (remote_user_id.empty()) {
          LOG_ERROR(
              "Invalid remote user join transmission msg without user id");
          break;
        }

        if (remote_user_id == user_id_) {
          break;
        }

        IceWorkMsg msg;
        msg.type = IceWorkMsg::Type::UserJoinTransmission;
        msg.transmission_id = transmission_id;
        msg.remote_user_id = remote_user_id;
        PushIceWorkMsg(msg);
      }

      break;
    }
    case "user_leave_transmission"_H: {
      std::string remote_user_id = j["user_id"];
      IceWorkMsg msg;
      msg.type = IceWorkMsg::Type::UserLeaveTransmission;
      msg.remote_user_id = remote_user_id;
      PushIceWorkMsg(msg);

      break;
    }
    case "offer"_H: {
      std::string transmission_id = j["transmission_id"].get<std::string>();
      std::string remote_user_id = j["remote_user_id"].get<std::string>();
      remote_user_id_ = remote_user_id;

      if (j.contains("sdp")) {
        std::string remote_sdp = j["sdp"].get<std::string>();
        LOG_INFO("[{}] receive offer from [{}]", user_id_, remote_user_id);

        connection_info_.transmission_id = transmission_id;
        connection_info_.user_id = user_id_;
        connection_info_.remote_user_id = remote_user_id;

        {
          std::unique_lock lock(peer_connection_map_mutex_);
          if (peer_connection_map_.find(remote_user_id) ==
              peer_connection_map_.end()) {
            if (remote_user_id.find("web") == std::string::npos) {
              peer_connection_map_.emplace(
                  remote_user_id,
                  std::make_shared<MiniRTCConnection>(
                      clock_, ws_transport_, connection_info_,
                      media_stream_ids_, connection_callbacks_));
            } else {
              peer_connection_map_.emplace(
                  remote_user_id,
                  std::make_shared<DataChannelConnection>(
                      clock_, ws_transport_, connection_info_,
                      media_stream_ids_, connection_callbacks_));
            }

            peer_connection_map_[remote_user_id]->Init();
          } else {
            LOG_ERROR("Peer connection already exists");
            break;
          }
        }

        IceWorkMsg msg;
        msg.type = IceWorkMsg::Type::Offer;
        msg.transmission_id = transmission_id;
        msg.remote_user_id = remote_user_id;
        msg.remote_sdp = remote_sdp;
        PushIceWorkMsg(msg);
        on_connection_status_(ConnectionStatus::Connecting,
                              remote_user_id.data(), remote_user_id.size(),
                              user_data_);
      } else {
        LOG_ERROR("Invalid offer msg");
      }

      break;
    }
    case "answer"_H: {
      std::string transmission_id = j["transmission_id"].get<std::string>();
      std::string remote_user_id = j["remote_user_id"].get<std::string>();
      remote_user_id_ = remote_user_id;

      if (j.contains("sdp")) {
        std::string remote_sdp = j["sdp"].get<std::string>();
        LOG_INFO("[{}] receive answer from [{}]", user_id_, remote_user_id);

        IceWorkMsg msg;
        msg.type = IceWorkMsg::Type::Answer;
        msg.transmission_id = transmission_id;
        msg.remote_user_id = remote_user_id;
        msg.remote_sdp = remote_sdp;
        PushIceWorkMsg(msg);
        on_connection_status_(ConnectionStatus::Connecting,
                              remote_user_id.data(), remote_user_id.size(),
                              user_data_);
      } else {
        LOG_ERROR("Invalid answer msg");
      }

      break;
    }
    case "new_candidate"_H: {
      std::string transmission_id = j["transmission_id"].get<std::string>();
      std::string new_candidate = j["sdp"].get<std::string>();
      std::string remote_user_id = j["remote_user_id"].get<std::string>();

      IceWorkMsg msg;
      msg.type = IceWorkMsg::Type::NewCandidate;
      msg.transmission_id = transmission_id;
      msg.remote_user_id = remote_user_id;
      msg.new_candidate = new_candidate;
      PushIceWorkMsg(msg);

      break;
    }
    default: {
      break;
    }
  }
}

void PeerConnection::StartIceWorker() {
  ice_worker_ = std::thread([this]() {
    while (true) {
      std::unique_lock<std::mutex> lck(ice_work_mutex_);
      while (ice_work_msg_queue_.empty() && ice_worker_running_) {
        ice_work_cv_.wait(lck, [this] {
          return !ice_work_msg_queue_.empty() || !ice_worker_running_;
        });
      }

      if (!ice_worker_running_) {
        break;
      }

      IceWorkMsg msg = ice_work_msg_queue_.front();
      ice_work_msg_queue_.pop();
      lck.unlock();
      ProcessIceWorkMsg(msg);
    }
    std::queue<IceWorkMsg> empty_queue;
    std::swap(ice_work_msg_queue_, empty_queue);
  });
}

void PeerConnection::StopIceWorker() {
  ice_worker_running_ = false;
  ice_work_cv_.notify_one();
  if (ice_worker_.joinable()) {
    ice_worker_.join();
  }
}

void PeerConnection::PushIceWorkMsg(const IceWorkMsg& msg) {
  std::lock_guard<std::mutex> lck(ice_work_mutex_);
  ice_work_msg_queue_.push(msg);
  ice_work_cv_.notify_one();
}

void PeerConnection::ProcessIceWorkMsg(const IceWorkMsg& msg) {
  if (msg.remote_user_id != "") {
    if (msg.type == IceWorkMsg::Type::UserLeaveTransmission) {
      std::string remote_user_id = msg.remote_user_id;
      {
        std::shared_lock lock(peer_connection_map_mutex_);
        auto it = peer_connection_map_.find(remote_user_id);
        if (it != peer_connection_map_.end()) {
          it->second->ProcessIceWorkMsg(msg);
        }
      }
      {
        std::unique_lock lock(peer_connection_map_mutex_);
        auto it = peer_connection_map_.find(remote_user_id);
        if (it != peer_connection_map_.end()) {
          LOG_INFO(
              "[{}] Remove peer connection for user [{}] after leave "
              "transmission",
              user_id_, remote_user_id);
          peer_connection_map_.erase(it);
        }
      }
    } else {
      std::shared_lock lock(peer_connection_map_mutex_);
      if (peer_connection_map_.find(msg.remote_user_id) !=
          peer_connection_map_.end()) {
        peer_connection_map_[msg.remote_user_id]->ProcessIceWorkMsg(msg);
      }
    }
  }
}
}  // namespace minirtc