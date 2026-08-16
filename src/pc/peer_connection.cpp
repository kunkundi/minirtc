#include "peer_connection.h"

#include <chrono>
#include <regex>

#include "INIReader.h"
#include "common.h"
#include "log.h"
#include "nlohmann/json.hpp"

namespace minirtc {

using nlohmann::json;

namespace {

const char* ConnectionStatusToString(ConnectionStatus status) {
  switch (status) {
    case ConnectionStatus::Connecting:
      return "connecting";
    case ConnectionStatus::Connected:
      return "connected";
    case ConnectionStatus::Gathering:
      return "gathering";
    case ConnectionStatus::Disconnected:
      return "disconnected";
    case ConnectionStatus::Failed:
      return "failed";
    case ConnectionStatus::Closed:
      return "closed";
    case ConnectionStatus::IncorrectPassword:
      return "incorrect_password";
    case ConnectionStatus::NoSuchTransmissionId:
      return "no_such_transmission_id";
    case ConnectionStatus::RemoteUnavailable:
      return "remote_unavailable";
    default:
      return "unknown";
  }
}

const char* SignalStatusToString(SignalStatus status) {
  switch (status) {
    case SignalStatus::SignalConnecting:
      return "connecting";
    case SignalStatus::SignalConnected:
      return "connected";
    case SignalStatus::SignalFailed:
      return "failed";
    case SignalStatus::SignalClosed:
      return "closed";
    case SignalStatus::SignalReconnecting:
      return "reconnecting";
    case SignalStatus::SignalServerClosed:
      return "server_closed";
    case SignalStatus::SignalTlsCertError:
      return "tls_cert_error";
    default:
      return "unknown";
  }
}

bool IsValidTurnMode(TurnMode mode) {
  return mode >= TurnMode::TurnDisabled && mode <= TurnMode::TurnForceTcp;
}

const char* TurnModeToString(TurnMode mode) {
  switch (mode) {
    case TurnMode::TurnDisabled:
      return "disabled";
    case TurnMode::TurnAutoUdpTcp:
      return "auto_udp_tcp";
    case TurnMode::TurnForceUdp:
      return "force_udp";
    case TurnMode::TurnForceTcp:
      return "force_tcp";
    default:
      return "invalid";
  }
}

TurnMode ParseTurnMode(const std::string& mode, bool legacy_enabled) {
  if (mode.empty()) {
    return legacy_enabled ? TurnMode::TurnAutoUdpTcp
                          : TurnMode::TurnDisabled;
  }
  if (mode == "disabled" || mode == "0") {
    return TurnMode::TurnDisabled;
  }
  if (mode == "auto" || mode == "auto_udp_tcp" || mode == "1") {
    return TurnMode::TurnAutoUdpTcp;
  }
  if (mode == "force_udp" || mode == "2") {
    return TurnMode::TurnForceUdp;
  }
  if (mode == "force_tcp" || mode == "3") {
    return TurnMode::TurnForceTcp;
  }

  LOG_WARN("Invalid TURN mode [{}], falling back to legacy enable flag", mode);
  return legacy_enabled ? TurnMode::TurnAutoUdpTcp
                        : TurnMode::TurnDisabled;
}

}  // namespace

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
    cfg_turn_mode_ = reader.Get("turn mode", "mode", "");
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
    turn_mode_ = ParseTurnMode(cfg_turn_mode_, cfg_enable_turn_ == "true");
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
    if (IsValidTurnMode(params.turn_mode)) {
      turn_mode_ = params.turn_mode;
    } else {
      LOG_WARN("Invalid TURN mode value [{}], disabling TURN",
               static_cast<int>(params.turn_mode));
      turn_mode_ = TurnMode::TurnDisabled;
    }
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
  connection_info_.turn_mode = turn_mode_;
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
    LOG_INFO("Turn server ip [{}] port [{}] username [{}]",
             cfg_turn_server_ip_, turn_server_port_, cfg_turn_server_username_);
  }

  LOG_INFO("Hardware accelerated codec [{}]",
           hardware_acceleration_ ? "ON" : "OFF");
  LOG_INFO("Video format [{}]", av1_encoding_ ? "AV1" : "H.264");
  LOG_INFO("TURN mode [{}]", TurnModeToString(turn_mode_));

  on_receive_video_buffer_ = params.on_receive_video_buffer;
  on_receive_audio_buffer_ = params.on_receive_audio_buffer;
  on_receive_data_buffer_ = params.on_receive_data_buffer;

  on_receive_video_frame_ = params.on_receive_video_frame;

  on_signal_status_ = params.on_signal_status;
  on_connection_status_ = params.on_connection_status;
  on_net_status_report_ = params.on_net_status_report;
  user_data_ = params.user_data;

  connection_callbacks_.on_receive_video_frame = params.on_receive_video_frame;
  connection_callbacks_.on_receive_audio_buffer =
      params.on_receive_audio_buffer;
  connection_callbacks_.on_receive_data_buffer = params.on_receive_data_buffer;

  connection_callbacks_.on_net_status_report = params.on_net_status_report;
  connection_callbacks_.user_data = user_data_;

  on_signal_message_ = params.on_signal_message;

  on_receive_ws_msg_ = [this](const std::string& msg) {
    auto j = json::parse(msg, nullptr, /*allow_exceptions=*/false);
    if (!j.is_discarded() && j.contains("type") && j["type"].is_string()) {
      std::string t = j["type"].get<std::string>();
      if (internal_signal_types_.find(t) != internal_signal_types_.end()) {
        ProcessSignal(msg);
        return;
      }
    }
    if (on_signal_message_) {
      on_signal_message_(msg.data(), msg.size(), user_data_);
    }
  };

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
      ClearPeerConnections("signal failed");
      on_signal_status_(SignalStatus::SignalFailed, user_id_.data(),
                        user_id_.size(), user_data_);
    } else if (WsStatus::WsClosed == ws_status) {
      ws_status_ = WsStatus::WsClosed;
      signal_status_ = SignalStatus::SignalClosed;
      ClearPeerConnections("signal closed");
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
      ClearPeerConnections("signal server closed");
      on_signal_status_(SignalStatus::SignalServerClosed, user_id_.data(),
                        user_id_.size(), user_data_);
    } else if (WsStatus::WsTlsCertError == ws_status) {
      ws_status_ = WsStatus::WsTlsCertError;
      signal_status_ = SignalStatus::SignalTlsCertError;
      ClearPeerConnections("signal TLS certificate error");
      on_signal_status_(SignalStatus::SignalTlsCertError, user_id_.data(),
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
  SignalStatus signal_status = GetSignalStatus();
  if (SignalStatus::SignalConnected != signal_status) {
    if (signal_status == SignalStatus::SignalConnecting ||
        signal_status == SignalStatus::SignalReconnecting) {
      LOG_WARN("Signal service not ready for join yet, status = [{}]",
               SignalStatusToString(signal_status));
    } else {
      LOG_ERROR("Signal server not connected, status = [{}]",
                SignalStatusToString(signal_status));
    }
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
  SignalStatus signal_status = GetSignalStatus();
  if (SignalStatus::SignalConnected != signal_status) {
    if (signal_status == SignalStatus::SignalConnecting ||
        signal_status == SignalStatus::SignalReconnecting) {
      LOG_WARN("Signal not ready for leave yet, status=[{}]",
               SignalStatusToString(signal_status));
    } else {
      LOG_ERROR("Signal not connected, status=[{}]",
                SignalStatusToString(signal_status));
    }
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

  ClearPeerConnections("leave transmission");

  return 0;
}

int PeerConnection::AddVideoStream(const char* stream_id) {
  LOG_INFO("Add video stream [{}]", stream_id);
  media_stream_ids_.video.push_back(stream_id);
  return 0;
}

int PeerConnection::AddAudioStream(const char* stream_id) {
  LOG_INFO("Add audio stream [{}]", stream_id);
  media_stream_ids_.audio.push_back(stream_id);
  return 0;
}

int PeerConnection::AddDataStream(const char* stream_id, bool reliable) {
  LOG_INFO("Add data stream [{}] reliable: {}", stream_id, reliable);
  media_stream_ids_.data.insert(std::make_pair(stream_id, reliable));
  return 0;
}

int PeerConnection::Destroy() {
  ClearPeerConnections("destroy peer connection");
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

bool PeerConnection::IsTerminalConnectionStatus(ConnectionStatus status) const {
  return status == ConnectionStatus::Disconnected ||
         status == ConnectionStatus::Failed ||
         status == ConnectionStatus::Closed;
}

std::shared_ptr<ConnectionInterface>
PeerConnection::CreateManagedPeerConnection(const std::string& remote_user_id) {
  auto weak_connection = std::make_shared<std::weak_ptr<ConnectionInterface>>();
  ConnectionCallbacks callbacks = connection_callbacks_;
  callbacks.on_connection_status = [this, remote_user_id, weak_connection](
                                       ConnectionStatus status,
                                       const char* peer_id, size_t peer_id_size,
                                       void* user_data) {
    auto connection = weak_connection->lock();
    if (!connection) {
      return;
    }

    if (IsTerminalConnectionStatus(status)) {
      // Removing first serializes terminal transport callbacks against an
      // explicit leave or a bulk clear. Exactly one path wins the map entry
      // and therefore exactly one terminal callback reaches the API user.
      if (!CleanupPeerConnection(remote_user_id, connection, status)) {
        return;
      }
      if (on_connection_status_) {
        on_connection_status_(status, peer_id, peer_id_size, user_data);
      }
      return;
    }

    if (!IsCurrentPeerConnection(remote_user_id, connection)) {
      // A managed connection can be removed before its transport finishes
      // shutting down. Ignore those late callbacks so callers observe one
      // authoritative terminal transition for that connection generation.
      return;
    }

    if (on_connection_status_) {
      on_connection_status_(status, peer_id, peer_id_size, user_data);
    }
  };

  std::shared_ptr<ConnectionInterface> connection;
  if (remote_user_id.find("web") == std::string::npos) {
    connection = std::make_shared<MiniRTCConnection>(
        clock_, ws_transport_, connection_info_, media_stream_ids_, callbacks);
  } else {
    connection = std::make_shared<DataChannelConnection>(
        clock_, ws_transport_, connection_info_, media_stream_ids_, callbacks);
  }

  *weak_connection = connection;
  return connection;
}

bool PeerConnection::CleanupPeerConnection(
    const std::string& remote_user_id,
    const std::shared_ptr<ConnectionInterface>& connection,
    ConnectionStatus status) {
  std::unique_lock lock(peer_connection_map_mutex_);
  auto it = peer_connection_map_.find(remote_user_id);
  if (it != peer_connection_map_.end() && it->second == connection) {
    peer_connection_map_.erase(it);
    LOG_INFO("[{}] Remove peer connection for user [{}] after status [{}]",
             user_id_, remote_user_id, ConnectionStatusToString(status));
    return true;
  }
  return false;
}

bool PeerConnection::IsCurrentPeerConnection(
    const std::string& remote_user_id,
    const std::shared_ptr<ConnectionInterface>& connection) {
  std::shared_lock lock(peer_connection_map_mutex_);
  auto it = peer_connection_map_.find(remote_user_id);
  return it != peer_connection_map_.end() && it->second == connection;
}

std::shared_ptr<ConnectionInterface>
PeerConnection::ReplaceOrCreatePeerConnection(const std::string& remote_user_id,
                                              const char* context) {
  std::shared_ptr<ConnectionInterface> replaced;
  std::shared_ptr<ConnectionInterface> connection;
  {
    std::unique_lock lock(peer_connection_map_mutex_);
    auto it = peer_connection_map_.find(remote_user_id);
    if (it != peer_connection_map_.end()) {
      replaced = it->second;
      peer_connection_map_.erase(it);
      LOG_WARN("[{}] Replace existing peer connection for user [{}] on {}",
               user_id_, remote_user_id, context);
    }

    connection = CreateManagedPeerConnection(remote_user_id);
    peer_connection_map_.emplace(remote_user_id, connection);
  }

  if (replaced) {
    replaced->ReleaseAllIceTransmission();
  }

  if (connection) {
    connection->Init();
  }

  return connection;
}

void PeerConnection::ClearPeerConnections(const char* reason) {
  std::vector<std::pair<std::string, std::shared_ptr<ConnectionInterface>>>
      connections;
  {
    std::unique_lock lock(peer_connection_map_mutex_);
    for (auto& [remote_user_id, connection] : peer_connection_map_) {
      if (connection) {
        connections.emplace_back(remote_user_id, connection);
      }
    }
    peer_connection_map_.clear();
  }

  if (connections.empty()) {
    return;
  }

  LOG_INFO("[{}] Clear [{}] peer connection(s), reason=[{}]", user_id_,
           connections.size(), reason);
  for (auto& [remote_user_id, connection] : connections) {
    if (on_connection_status_) {
      on_connection_status_(ConnectionStatus::Closed, remote_user_id.data(),
                            remote_user_id.size(), user_data_);
    }
    connection->ReleaseAllIceTransmission();
  }
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

int PeerConnection::RequestVideoKeyFrame(const char* stream_id) {
  std::shared_lock lock(peer_connection_map_mutex_);
  int ret = -1;
  for (auto& peer_connection : peer_connection_map_) {
    if (peer_connection.second &&
        peer_connection.second->RequestVideoKeyFrame(stream_id) == 0) {
      ret = 0;
    }
  }

  return ret;
}

int PeerConnection::RequestAllVideoKeyFrames() {
  std::shared_lock lock(peer_connection_map_mutex_);
  int ret = -1;
  for (auto& peer_connection : peer_connection_map_) {
    if (peer_connection.second &&
        peer_connection.second->RequestAllVideoKeyFrames() == 0) {
      ret = 0;
    }
  }

  return ret;
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

int PeerConnection::SendSignalMessage(const char* message, size_t size) {
  if (!message || size == 0) {
    LOG_ERROR("Invalid signal message");
    return -1;
  }
  if (!ws_transport_ || WsStatus::WsOpened != ws_status_) {
    LOG_ERROR("Websocket not opened");
    return -1;
  }
  ws_transport_->Send(std::string(message, size));
  return 0;
}

int64_t PeerConnection::GetSystemTimeMicros() {
  if (clock_) {
    return clock_->CurrentTimeUs();
  }
  return 0;
}

bool PeerConnection::ApplyTurnCredentials(const json& message) {
  if (!message.contains("turn") || !message["turn"].is_object()) {
    return false;
  }

  const json& turn = message["turn"];
  if (!turn.contains("host") || !turn["host"].is_string() ||
      !turn.contains("port") ||
      (!turn["port"].is_number_integer() &&
       !turn["port"].is_number_unsigned()) ||
      !turn.contains("username") || !turn["username"].is_string() ||
      !turn.contains("password") || !turn["password"].is_string() ||
      !turn.contains("expires_at") ||
      (!turn["expires_at"].is_number_integer() &&
       !turn["expires_at"].is_number_unsigned())) {
    LOG_WARN("Ignore malformed dynamic TURN credentials");
    return false;
  }

  const std::string host = turn["host"].get<std::string>();
  const int64_t port = turn["port"].get<int64_t>();
  const std::string username = turn["username"].get<std::string>();
  const std::string password = turn["password"].get<std::string>();
  const int64_t expires_at = turn["expires_at"].get<int64_t>();
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

  if (host.empty() || port < 1 || port > 65535 || username.empty() ||
      password.empty() || expires_at <= now) {
    LOG_WARN("Ignore invalid or expired dynamic TURN credentials");
    return false;
  }

  cfg_turn_server_ip_ = host;
  turn_server_port_ = static_cast<int>(port);
  cfg_turn_server_port_ = std::to_string(turn_server_port_);
  cfg_turn_server_username_ = username;
  cfg_turn_server_password_ = password;
  turn_credential_expires_at_ = expires_at;

  connection_info_.turn_server_ip = cfg_turn_server_ip_;
  connection_info_.turn_server_port = turn_server_port_;
  connection_info_.turn_server_username = cfg_turn_server_username_;
  connection_info_.turn_server_password = cfg_turn_server_password_;

  LOG_INFO("Updated dynamic TURN credentials for [{}:{}], expires at [{}]",
           cfg_turn_server_ip_, turn_server_port_, turn_credential_expires_at_);
  return true;
}

// Process signal message from signal server
void PeerConnection::ProcessSignal(const std::string& signal) {
  auto j = json::parse(signal);
  std::string type = j["type"];
  // LOG_INFO("signal type: {}", type);
  switch (HASH_STRING_PIECE(type.c_str())) {
    case "login"_H: {
      if (j["status"].get<std::string>() == "success") {
        ApplyTurnCredentials(j);
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
        } else if ("Remote unavailable" == reason) {
          on_connection_status_(ConnectionStatus::RemoteUnavailable,
                                transmission_id.data(), transmission_id.size(),
                                user_data_);
        }
      } else {
        ApplyTurnCredentials(j);
        std::string remote_user_id = j["user_id"].get<std::string>();

        if (remote_user_id.empty()) {
          LOG_ERROR(
              "Invalid remote user join transmission msg without user id");
          break;
        }

        if (remote_user_id == user_id_) {
          break;
        }

        connection_info_.transmission_id = transmission_id;
        connection_info_.user_id = user_id_;
        connection_info_.remote_user_id = remote_user_id;

        ReplaceOrCreatePeerConnection(remote_user_id, "join");

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
        ApplyTurnCredentials(j);
        std::string remote_sdp = j["sdp"].get<std::string>();
        LOG_INFO("[{}] receive offer from [{}]", user_id_, remote_user_id);

        if (remote_user_id.empty()) {
          LOG_ERROR("Invalid offer msg without remote user id");
          break;
        }

        connection_info_.transmission_id = transmission_id;
        connection_info_.user_id = user_id_;
        connection_info_.remote_user_id = remote_user_id;

        ReplaceOrCreatePeerConnection(remote_user_id, "offer");

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
    case "new_candidate_mid"_H: {
      std::string transmission_id = j["transmission_id"].get<std::string>();
      std::string remote_user_id = j["remote_user_id"].get<std::string>();
      std::string candidate = j["candidate"].get<std::string>();
      std::string mid = j["mid"].get<std::string>();

      IceWorkMsg msg;
      msg.type = IceWorkMsg::Type::NewCandidateMid;
      msg.transmission_id = transmission_id;
      msg.remote_user_id = remote_user_id;
      msg.candidate = candidate;
      msg.mid = mid;
      PushIceWorkMsg(msg);

      break;
    }
    case "turn_credentials"_H: {
      if (j.value("status", "fail") == "success") {
        ApplyTurnCredentials(j);
      } else {
        LOG_WARN("Failed to refresh dynamic TURN credentials: [{}]",
                 j.value("reason", "Unknown error"));
      }
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
  if (msg.remote_user_id.empty()) {
    return;
  }

  if (msg.type == IceWorkMsg::Type::UserLeaveTransmission) {
    std::string remote_user_id = msg.remote_user_id;
    std::shared_ptr<ConnectionInterface> connection;
    {
      std::unique_lock lock(peer_connection_map_mutex_);
      auto it = peer_connection_map_.find(remote_user_id);
      if (it != peer_connection_map_.end()) {
        connection = it->second;
        peer_connection_map_.erase(it);
      }
    }
    if (connection) {
      LOG_INFO(
          "[{}] Remove peer connection for user [{}] after leave "
          "transmission",
          user_id_, remote_user_id);
      if (on_connection_status_) {
        on_connection_status_(ConnectionStatus::Closed, remote_user_id.data(),
                              remote_user_id.size(), user_data_);
      }
      // The map entry is already gone, so any transport callback emitted by
      // teardown is recognized as stale and suppressed by the managed
      // callback wrapper above.
      connection->ProcessIceWorkMsg(msg);
    }
  } else {
    std::shared_ptr<ConnectionInterface> connection;
    {
      std::shared_lock lock(peer_connection_map_mutex_);
      auto it = peer_connection_map_.find(msg.remote_user_id);
      if (it != peer_connection_map_.end()) {
        connection = it->second;
      }
    }
    if (connection) {
      connection->ProcessIceWorkMsg(msg);
    }
  }
}
}  // namespace minirtc
