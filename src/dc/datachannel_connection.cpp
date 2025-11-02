#include "datachannel_connection.h"

#include <regex>

#include "INIReader.h"
#include "common.h"
#include "log.h"
#include "nlohmann/json.hpp"

namespace minirtc {

using nlohmann::json;

template <class T>
std::weak_ptr<T> make_weak_ptr(std::shared_ptr<T> ptr) {
  return ptr;
}

DataChannelConnection::DataChannelConnection() {}

DataChannelConnection::~DataChannelConnection() { user_data_ = nullptr; }

int DataChannelConnection::Init(PeerConnectionParams params) {
  if (inited_) {
    LOG_INFO("Peer already inited");
    return 0;
  }

  InitLogger(::rtc::LogLevel::Debug,
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
    cfg_tls_cert_path_ = reader.Get("tls", "cert_path", "");
    cfg_hardware_acceleration_ =
        reader.Get("hardware acceleration", "turn_on", "false");
    cfg_av1_encoding_ = reader.Get("av1 encoding", "turn_on", "false");
    cfg_enable_turn_ = reader.Get("enable turn", "turn_on", "false");
    cfg_enable_srtp_ = reader.Get("enable srtp", "turn_on", "true");

    std::regex regex("\n");

    signal_server_port_ = stoi(cfg_signal_server_port_);
    stun_server_port_ = stoi(cfg_stun_server_port_);
    turn_server_port_ = stoi(cfg_turn_server_port_);

    hardware_acceleration_ =
        cfg_hardware_acceleration_ == "true" ? true : false;
    av1_encoding_ = cfg_av1_encoding_ == "true" ? true : false;
    enable_turn_ = cfg_enable_turn_ == "true" ? true : false;
    enable_srtp_ = cfg_enable_srtp_ == "true" ? true : false;

  } else {
    cfg_signal_server_ip_ = params.signal_server_ip;
    signal_server_port_ = params.signal_server_port;
    cfg_stun_server_ip_ = params.stun_server_ip;
    stun_server_port_ = params.stun_server_port;
    cfg_turn_server_ip_ = params.turn_server_ip;
    turn_server_port_ = params.turn_server_port;
    cfg_turn_server_username_ = params.turn_server_username;
    cfg_turn_server_password_ = params.turn_server_password;
    cfg_tls_cert_path_ = params.tls_cert_path;
    hardware_acceleration_ = params.hardware_acceleration;
    av1_encoding_ = params.av1_encoding;
    enable_turn_ = params.enable_turn;
    enable_srtp_ = params.enable_srtp;

    cfg_signal_server_port_ = std::to_string(signal_server_port_);
    cfg_stun_server_port_ = std::to_string(stun_server_port_);
    cfg_turn_server_port_ = std::to_string(turn_server_port_);
  }

  LOG_INFO("Read config success, use configure file [{}]", params.use_cfg_file);

  LOG_INFO("Signal server ip [{}] port [{}]", cfg_signal_server_ip_,
           cfg_signal_server_port_);

  LOG_INFO("Cert file path [{}]", cfg_tls_cert_path_);

  std::string stun_server = cfg_stun_server_ip_ + ":" + cfg_stun_server_port_;
  peer_connection_config_.iceServers.emplace_back(stun_server);
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
  net_status_report_ = params.net_status_report;
  user_data_ = params.user_data;

  on_ice_status_change_ = [this](std::string ice_status,
                                 const std::string& user_id) {
    if ("connecting" == ice_status) {
      on_connection_status_(ConnectionStatus::Connecting, user_id.data(),
                            user_id.size(), user_data_);
    } else if ("gathering" == ice_status) {
      on_connection_status_(ConnectionStatus::Gathering, user_id.data(),
                            user_id.size(), user_data_);
    } else if ("disconnected" == ice_status) {
      on_connection_status_(ConnectionStatus::Disconnected, user_id.data(),
                            user_id.size(), user_data_);
    } else if ("connected" == ice_status) {
      // std::string transmission_id = std::string(user_id, user_id_size);
      // is_ice_transport_ready_[user_id] = true;
      // on_connection_status_(ConnectionStatus::Connected, user_id.data(),
      //                       user_id.size(), user_data_);
      // b_force_i_frame_ = true;
      LOG_INFO("Ice connected");
    } else if ("ready" == ice_status) {
      is_ice_transport_ready_[user_id] = true;
      b_force_i_frame_ = true;
      LOG_INFO("Ice ready");
      on_connection_status_(ConnectionStatus::Connected, user_id.data(),
                            user_id.size(), user_data_);
    } else if ("closed" == ice_status) {
      is_ice_transport_ready_[user_id] = false;
      LOG_INFO("Ice closed");
      on_connection_status_(ConnectionStatus::Closed, user_id.data(),
                            user_id.size(), user_data_);
    } else if ("failed" == ice_status) {
      is_ice_transport_ready_[user_id] = false;
      if (offer_peer_ && try_rejoin_with_turn_) {
        if (reconnect_count_ > 3) {
          LOG_INFO("Recreate with turn exceed max count, give up");
          on_connection_status_(ConnectionStatus::Failed, user_id.data(),
                                user_id.size(), user_data_);
        } else {
          LOG_INFO(
              "Ice failed, destroy ice agent and rereate it with TURN enabled");

          enable_turn_ = true;
          reliable_ice_ = false;

          if (offer_peer_) {
            reconnect_count_++;
            IceWorkMsg msg;
            msg.type = IceWorkMsg::Type::RetryWithTurn;
            msg.transmission_id = remote_transmission_id_;
            msg.user_id_list = user_id_list_;
            PushIceWorkMsg(msg);
          }
        }
      } else {
        LOG_INFO("Ice failed");
        on_connection_status_(ConnectionStatus::Failed, user_id.data(),
                              user_id.size(), user_data_);
      }
    } else {
      is_ice_transport_ready_[user_id] = false;
      LOG_INFO("Unknown ice state [{}]", ice_status);
    }
  };

  clock_ = std::make_shared<SystemClock>();

  ws_transport_ = std::make_shared<::rtc::WebSocket>();

  ws_transport_->onOpen([this]() {
    LOG_INFO("WebSocket opened");
    ws_status_ = WsStatus::WsOpened;
    signal_status_ = SignalStatus::SignalConnected;
    on_signal_status_(SignalStatus::SignalConnected, user_id_.data(),
                      user_id_.size(), user_data_);

    Login();
  });

  ws_transport_->onClosed([this]() {
    LOG_INFO("WebSocket closed");
    ws_status_ = WsStatus::WsClosed;
    signal_status_ = SignalStatus::SignalClosed;
    on_signal_status_(SignalStatus::SignalClosed, user_id_.data(),
                      user_id_.size(), user_data_);
  });

  ws_transport_->onError([this](const std::string& error) {
    LOG_ERROR("WebSocket error [{}]", error);
    ws_status_ = WsStatus::WsFailed;
    signal_status_ = SignalStatus::SignalFailed;
    on_signal_status_(SignalStatus::SignalFailed, user_id_.data(),
                      user_id_.size(), user_data_);
  });

  ws_transport_->onMessage(
      [&, this](std::variant<::rtc::binary, std::string> data) {
        if (!std::holds_alternative<std::string>(data)) {
          return;
        }

        const std::string& msg = std::get<std::string>(data);
        ProcessSignal(msg);
      });

  uri_ = "wss://" + cfg_signal_server_ip_ + ":" + cfg_signal_server_port_;
  if (ws_transport_) {
    LOG_INFO("[{}] Connecting to signal server [{}]", user_id_, uri_);
    ws_transport_->open(uri_);
    ws_status_ = WsStatus::WsOpening;
    signal_status_ = SignalStatus::SignalConnecting;
    on_signal_status_(SignalStatus::SignalConnecting, user_id_.data(),
                      user_id_.size(), user_data_);
  }

  StartIceWorker();

  LOG_INFO("[{}] Init finish", user_id_);

  inited_ = true;
  return 0;
}

int DataChannelConnection::Login() {
  if (WsStatus::WsOpened != ws_status_) {
    LOG_ERROR("Websocket not opened");
    return -1;
  }

  int ret = 0;

  json message = {{"type", "login"}, {"user_id", user_id_with_pwd_}};

  if (ws_transport_) {
    ws_transport_->send(message.dump());
    LOG_INFO("[{}] send login request to signal server", user_id_);
  }
  return ret;
}

int DataChannelConnection::Join(const std::string& transmission_id) {
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
    ws_transport_->send(message.dump());
    LOG_INFO(
        "[{}] sends join transmission request to transmission "
        "id [{}]",
        user_id_, transmission_id);
  }

  return ret;
}

int DataChannelConnection::NegotiationFailed() {
  if (SignalStatus::SignalConnected != GetSignalStatus()) {
    LOG_ERROR("Signal not connected");
    return -1;
  }

  json message = {{"type", "negotiation_failed"},
                  {"user_id", user_id_},
                  {"transmission_id", local_transmission_id_}};
  if (ws_transport_) {
    ws_transport_->send(message.dump());
    LOG_INFO(
        "[{}] sends negotiation failed notification to [{}] for transmission "
        "id [{}]",
        user_id_, remote_user_id_, local_transmission_id_);
  }

  ReleaseAllIceTransmission();

  return 0;
}

int DataChannelConnection::Leave(const std::string& transmission_id) {
  if (SignalStatus::SignalConnected != GetSignalStatus()) {
    LOG_ERROR("Signal not connected");
    return -1;
  }

  json message = {{"type", "leave_transmission"},
                  {"user_id", user_id_},
                  {"transmission_id", transmission_id}};
  if (ws_transport_) {
    ws_transport_->send(message.dump());
    LOG_INFO("[{}] sends leave transmission [{}] notification ", user_id_,
             transmission_id);
  }

  is_ice_transport_ready_[user_id_] = false;
  leave_ = true;

  ReleaseAllIceTransmission();
  return 0;
}

int DataChannelConnection::AddVideoStream(const char* stream_id) {
  video_stream_ids_.push_back("video-stream");
  return 0;
}

int DataChannelConnection::AddAudioStream(const char* stream_id) {
  audio_stream_ids_.push_back(stream_id);
  return 0;
}

int DataChannelConnection::AddDataStream(const char* stream_id) {
  data_stream_ids_.push_back(stream_id);
  return 0;
}

int DataChannelConnection::ReleaseAllIceTransmission() {
  for (auto& user_id_it : ice_transport_list_) {
    user_id_it.second->DestroyIceTransmission();
  }
  ice_transport_list_.clear();
  is_ice_transport_ready_.clear();
  video_stream_ids_.clear();
  audio_stream_ids_.clear();
  data_stream_ids_.clear();
  return 0;
}

int DataChannelConnection::Destroy() {
  StopIceWorker();
  if (ws_transport_) {
    LOG_INFO("Close websocket");
    ws_transport_->close();
  }

  return 0;
}

int DataChannelConnection::RequestTransmissionMemberList(
    const std::string& transmission_id) {
  if (SignalStatus::SignalConnected != GetSignalStatus()) {
    LOG_ERROR("Signal not connected");
    return -1;
  }

  LOG_INFO("[{}] Request member list", transmission_id);

  json message = {{"type", "query_user_id_list"},
                  {"transmission_id", transmission_id}};

  if (ws_transport_) {
    ws_transport_->send(message.dump());
  }
  return 0;
}

SignalStatus DataChannelConnection::GetSignalStatus() {
  std::lock_guard<std::mutex> l(signal_status_mutex_);
  return signal_status_;
}

int DataChannelConnection::SendVideoFrame(const XVideoFrame* video_frame,
                                          const char* stream_id) {
  for (auto& it : dc_transport_list_) {
    auto& pc = it.second;
    pc->SendVideoFrame(video_frame, "video-stream");
  }
  return 0;
}

int DataChannelConnection::SendAudioFrame(const char* data, size_t size,
                                          const char* stream_id) {
  for (auto& it : dc_transport_list_) {
    auto& pc = it.second;
    pc->SendAudioFrame(data, size, stream_id);
  }
  return 0;
}

int DataChannelConnection::SendDataFrame(const char* data, size_t size,
                                         const char* stream_id) {
  for (auto& it : dc_transport_list_) {
    auto& pc = it.second;
    pc->SendDataFrame(data, size, stream_id);
  }

  return 0;
}

int64_t DataChannelConnection::GetSystemTimeMicros() {
  if (clock_) {
    return clock_->CurrentTimeUs();
  }
  return 0;
}

void DataChannelConnection::ProcessSignal(const std::string& signal) {
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

        net_status_report_(user_id_with_pwd.data(), user_id_with_pwd.size(),
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
    case "user_id_list"_H: {
      user_id_list_ = j["user_id_list"];

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
        if (leave_) {
          break;
        }

        IceWorkMsg msg;
        msg.type = IceWorkMsg::Type::UserIdList;
        msg.transmission_id = transmission_id;
        msg.user_id_list = user_id_list_;
        PushIceWorkMsg(msg);
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
        msg.remote_user_id = remote_user_id;
        PushIceWorkMsg(msg);
      }

      break;
    }
    case "user_leave_transmission"_H: {
      std::string user_id = j["user_id"];
      IceWorkMsg msg;
      msg.type = IceWorkMsg::Type::UserLeaveTransmission;
      msg.user_id = user_id;
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

void DataChannelConnection::StartIceWorker() {
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

void DataChannelConnection::StopIceWorker() {
  ice_worker_running_ = false;
  ice_work_cv_.notify_one();
  if (ice_worker_.joinable()) {
    ice_worker_.join();
  }
}

void DataChannelConnection::PushIceWorkMsg(const IceWorkMsg& msg) {
  std::lock_guard<std::mutex> lck(ice_work_mutex_);
  ice_work_msg_queue_.push(msg);
  ice_work_cv_.notify_one();
}

void DataChannelConnection::ProcessIceWorkMsg(const IceWorkMsg& msg) {
  switch (msg.type) {
    case IceWorkMsg::Type::Login: {
      break;
    }
    case IceWorkMsg::Type::UserIdList:
    case IceWorkMsg::Type::RetryWithTurn: {
      std::vector<std::string> user_id_list = msg.user_id_list;
      std::string transmission_id = msg.transmission_id;

      if (user_id_list.empty()) {
        LOG_WARN("Wait for host create transmission [{}]", transmission_id);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        RequestTransmissionMemberList(transmission_id);
        break;
      }

      LOG_INFO("Transmission [{}] members: [", transmission_id);
      for (const auto& user_id : user_id_list) {
        LOG_INFO("{}", user_id);
      }
      LOG_INFO("]");

      for (auto& remote_user_id : user_id_list) {
        if (remote_user_id == user_id_) {
          continue;
        }

        std::unique_lock lock(ice_transport_list_mutex_);
        dc_transport_list_.emplace(
            remote_user_id,
            CreateDataChannelConnection(peer_connection_config_, clock_,
                                        make_weak_ptr(ws_transport_), true,
                                        remote_user_id, remote_user_id));
      }

      break;
    }
    case IceWorkMsg::Type::UserLeaveTransmission: {
      std::string user_id = msg.user_id;
      LOG_INFO("[{}] Receive notification: user id [{}] leave transmission",
               (void*)this, user_id);
      std::unique_lock lock(ice_transport_list_mutex_);
      auto user_id_it = ice_transport_list_.find(user_id);
      if (user_id_it != ice_transport_list_.end()) {
        user_id_it->second->DestroyIceTransmission();
        ice_transport_list_.erase(user_id_it);
        is_ice_transport_ready_[user_id] = false;
        LOG_INFO("Terminate transmission to user [{}]", user_id);
      }
      break;
    }
    case IceWorkMsg::Type::UserJoinTransmission: {
      std::string remote_user_id = msg.remote_user_id;
      LOG_INFO("[{}] Receive notification: user id [{}] join transmission",
               (void*)this, remote_user_id);
      std::unique_lock lock(ice_transport_list_mutex_);
      if (ice_transport_list_.end() ==
          ice_transport_list_.find(remote_user_id)) {
        dc_transport_list_.emplace(
            remote_user_id,
            CreateDataChannelConnection(peer_connection_config_, clock_,
                                        make_weak_ptr(ws_transport_), true,
                                        remote_user_id, remote_user_id));
        LOG_INFO("Create transmission to user [{}]", remote_user_id);
      }
      break;
    }
    case IceWorkMsg::Type::Offer: {
      std::string transmission_id = msg.transmission_id;
      std::string remote_user_id = msg.remote_user_id;
      std::unique_lock lock(ice_transport_list_mutex_);
      if (ice_transport_list_.end() !=
          ice_transport_list_.find(remote_user_id)) {
        ice_transport_list_[remote_user_id]->DestroyIceTransmission();
        ice_transport_list_.erase(remote_user_id);
        is_ice_transport_ready_[remote_user_id] = false;
      }

      LOG_INFO("Receive offer");

      dc_transport_list_.emplace(
          remote_user_id,
          CreateDataChannelConnection(peer_connection_config_, clock_,
                                      make_weak_ptr(ws_transport_), false,
                                      transmission_id, remote_user_id));

      ::rtc::Description remote_sdp(msg.remote_sdp,
                                    ::rtc::Description::Type::Offer);
      // LOG_INFO("Set remote description: {}", msg.remote_sdp.c_str());
      dc_transport_list_[remote_user_id]
          ->peer_connection_->setRemoteDescription(remote_sdp);

      dc_transport_list_[remote_user_id]
          ->peer_connection_->setLocalDescription();

      std::shared_ptr<::rtc::DataChannel> dc;
      dc_transport_list_[remote_user_id]->peer_connection_->onDataChannel(
          [&](std::shared_ptr<::rtc::DataChannel> _dc) {
            std::cout << "[Got a DataChannel with label: " << _dc->label()
                      << "]" << std::endl;
            dc = _dc;

            dc->onClosed([&]() {
              std::cout << "[DataChannel closed: " << dc->label() << "]"
                        << std::endl;
            });

            dc->onMessage([](auto data) {
              if (std::holds_alternative<std::string>(data)) {
                std::cout << "[Received message: "
                          << std::get<std::string>(data) << "]" << std::endl;
              }
            });
          });

      break;
    }
    case IceWorkMsg::Type::Answer: {
      std::string remote_user_id = msg.remote_user_id;
      std::unique_lock lock(ice_transport_list_mutex_);

      ::rtc::Description remote_sdp(msg.remote_sdp, "answer");

      if (dc_transport_list_.find(remote_user_id) != dc_transport_list_.end()) {
        auto pc = dc_transport_list_[remote_user_id]->peer_connection_;
        if (pc) {
          // LOG_INFO("Set remote description: {}", msg.remote_sdp.c_str());
          pc->setRemoteDescription(remote_sdp);
        }
      }

      break;
    }
    case IceWorkMsg::Type::NewCandidate: {
      std::string transmission_id = msg.transmission_id;
      std::string new_candidate = msg.new_candidate;
      std::string remote_user_id = msg.remote_user_id;

      std::shared_lock lock(ice_transport_list_mutex_);
      if (dc_transport_list_.find(remote_user_id) != dc_transport_list_.end()) {
        auto pc = dc_transport_list_[remote_user_id]->peer_connection_;
        if (pc) {
          pc->addRemoteCandidate(new_candidate);
        }
      }
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
    std::weak_ptr<::rtc::WebSocket> wws, bool offer_peer,
    std::string transmission_id, std::string remote_user_id) {
  offer_peer_ = offer_peer;
  auto peer_connection = std::make_shared<::rtc::PeerConnection>(config);
  auto dc_transport = std::make_shared<DataChannelTransport>(
      clock, peer_connection, user_id_, remote_user_id, offer_peer_);

  peer_connection->onLocalDescription([this, transmission_id, remote_user_id,
                                       wws](::rtc::Description description) {
    std::string local_sdp = std::string(description);
    if (offer_peer_ && description.type() == ::rtc::Description::Type::Offer) {
      json message = {{"type", "offer"},
                      {"transmission_id", transmission_id},
                      {"user_id", user_id_},
                      {"remote_user_id", remote_user_id},
                      {"sdp", local_sdp.c_str()}};

      // Gathering complete, send offer
      if (auto ws = wws.lock()) {
        ws->send(message.dump());
        // LOG_INFO("[{}] send offer to [{}]: {}", user_id_, remote_user_id,
        //          message.dump());
      }
    } else if (!offer_peer_ &&
               description.type() == ::rtc::Description::Type::Answer) {
      json message = {{"type", "answer"},
                      {"transmission_id", transmission_id},
                      {"user_id", user_id_},
                      {"remote_user_id", remote_user_id},
                      {"sdp", local_sdp.c_str()}};

      // Gathering complete, send answer
      if (auto ws = wws.lock()) {
        ws->send(message.dump());
        // LOG_INFO("[{}] send answer to [{}]: {}", user_id_, remote_user_id,
        //          message.dump());
      }
    }
  });

  peer_connection->onLocalCandidate(
      [this, transmission_id, remote_user_id, wws](::rtc::Candidate candidate) {
        json message = {{"type", "new_candidate"},
                        {"transmission_id", transmission_id},
                        {"user_id", user_id_},
                        {"remote_user_id", remote_user_id},
                        {"sdp", std::string(candidate)}};

        if (auto ws = wws.lock()) ws->send(message.dump());
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
            LOG_INFO("PeerConnection state: Connected");
            break;
          case ::rtc::PeerConnection::State::Disconnected:
            ice_state = ConnectionStatus::Disconnected;
            LOG_INFO("PeerConnection state: Disconnected");
            break;
          case ::rtc::PeerConnection::State::Failed:
            ice_state = ConnectionStatus::Failed;
            LOG_ERROR("PeerConnection state: Failed");
            break;
          case ::rtc::PeerConnection::State::Closed:
            ice_state = ConnectionStatus::Closed;
            LOG_INFO("PeerConnection state: Closed");
            break;
          default:
            ice_state = ConnectionStatus::Failed;
            LOG_FATAL("PeerConnection state: Unknown");
            break;
        }
        on_connection_status_(ice_state, remote_user_id.data(),
                              remote_user_id.size(), user_data_);
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
        }
      });

  if (offer_peer_) {
    for (auto& video_stream_id : video_stream_ids_) {
      dc_transport->video_streams_.emplace(
          video_stream_id,
          AddVideo(
              peer_connection,
              av1_encoding_ ? rtp::PAYLOAD_TYPE::AV1 : rtp::PAYLOAD_TYPE::H264,
              GenerateUniqueSsrc(), video_stream_id, "stream1",
              [video_stream_id, wc = make_weak_ptr(dc_transport)]() {
                LOG_INFO("Video stream {} opened", video_stream_id);
              }));
    }

    for (auto& audio_stream_id : audio_stream_ids_) {
      dc_transport->audio_streams_.emplace(
          audio_stream_id,
          AddAudio(peer_connection, rtp::PAYLOAD_TYPE::OPUS,
                   GenerateUniqueSsrc(), "audio-stream", audio_stream_id,
                   [audio_stream_id, wc = make_weak_ptr(dc_transport)]() {
                     LOG_INFO("Audio stream {} opened", audio_stream_id);
                   }));
    }

    for (auto& data_stream_id : data_stream_ids_) {
      dc_transport->data_streams_.emplace(
          data_stream_id,
          AddData(peer_connection, rtp::PAYLOAD_TYPE::DATA,
                  GenerateUniqueSsrc(), "data-stream", data_stream_id,
                  [data_stream_id, wc = make_weak_ptr(dc_transport)]() {
                    LOG_INFO("Data stream {} opened", data_stream_id);
                  }));
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
        ssrc, cname, payload_type, ::rtc::AV1RtpPacketizer::ClockRate);
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
        ssrc, cname, payload_type, ::rtc::H264RtpPacketizer::ClockRate);
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
    const std::function<void(void)> onOpen) {
  auto dc = peer_connection->createDataChannel("ping-pong");
  dc->onOpen(onOpen);

  dc->onClosed([&]() {
    std::cout << "[DataChannel closed: " << dc->label() << "]" << std::endl;
  });

  dc->onMessage(nullptr, [msid, wdc = make_weak_ptr(dc)](std::string msg) {
    std::cout << "Message from " << msid << " received: " << msg << std::endl;
    if (auto dc = wdc.lock()) {
      dc->send("Ping");
    }
  });

  return dc;
}

}  // namespace minirtc