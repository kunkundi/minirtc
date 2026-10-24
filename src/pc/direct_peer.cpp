#include <cstring>
#include <utility>

#include "peer_connection.h"

namespace minirtc {
namespace {
bool ValidIdentity(const char* value) {
  if (!value) return false;
  const size_t length = std::strlen(value);
  if (!length || length > 128) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  }
  return true;
}
}  // namespace

int PeerConnection::InitDirect(PeerConnectionParams params,
                               const MiniRtcDirectOptions& options) {
  std::lock_guard lock(direct_mutex_);
  if (inited_ || options.struct_size < sizeof(MiniRtcDirectOptions) ||
      options.version != 1 || !options.send_signal || !options.on_error ||
      !params.on_connection_status || !ValidIdentity(options.local_id) ||
      !ValidIdentity(options.remote_id) || !ValidIdentity(options.session_id) ||
      std::strcmp(options.local_id, options.remote_id) == 0 ||
      !options.local_address || !options.remote_address ||
      !IsDirectIpv4(options.local_address) ||
      !IsDirectIpv4(options.remote_address))
    return -1;
  direct_ = true;
  direct_offerer_ = options.offerer;
  direct_error_ = options.on_error;
  direct_context_ = options.context;
  user_id_ = options.local_id;
  remote_user_id_ = options.remote_id;
  local_transmission_id_ = options.session_id;
  user_data_ = params.user_data;
  on_connection_status_ = params.on_connection_status;
  on_net_status_report_ = params.on_net_status_report;
  connection_callbacks_.on_receive_video_frame = params.on_receive_video_frame;
  connection_callbacks_.on_receive_audio_buffer =
      params.on_receive_audio_buffer;
  connection_callbacks_.on_receive_data_buffer = params.on_receive_data_buffer;
  connection_callbacks_.on_net_status_report = params.on_net_status_report;
  connection_callbacks_.user_data = params.user_data;
  connection_info_ = {};
  connection_info_.direct_policy = {IcePolicy::HostOnly, options.local_address,
                                    options.remote_address};
  connection_info_.direct_error = [this](MiniRtcDirectError error) {
    ReportDirectError(error);
  };
  connection_info_.user_id = user_id_;
  connection_info_.remote_user_id = remote_user_id_;
  connection_info_.transmission_id = local_transmission_id_;
  connection_info_.hardware_acceleration = params.hardware_acceleration;
  connection_info_.native_video_output = params.native_video_output;
  connection_info_.trickle_ice = true;
  connection_info_.reliable_ice = false;
  connection_info_.turn_mode = TurnMode::TurnDisabled;
  connection_info_.enable_srtp = true;
  connection_info_.video_content_type = params.video_content_type;
  connection_info_.av1_encoding = params.av1_encoding;
  connection_info_.video_quality = params.video_quality;
  connection_info_.video_frame_rate = params.video_frame_rate == 30 ? 30 : 60;
  connection_info_.video_degradation_preference =
      params.video_degradation_preference;
  clock_ = std::make_shared<SystemClock>();
  signal_sender_ = std::make_shared<CallbackSignalSender>(
      [this, send = options.send_signal,
       context = options.context](const std::string& message) {
        if (direct_closing_) return -1;
        const int result = send(message.data(), message.size(), context);
        if (result != 0) ReportDirectError(MiniRtcDirectSignalSendFailed);
        return result;
      });
  StartIceWorker();
  inited_ = true;
  return 0;
}

int PeerConnection::StartDirect(bool offerer) {
  std::lock_guard lock(direct_mutex_);
  if (!direct_ || !inited_ || direct_started_ || direct_closing_ ||
      offerer != direct_offerer_)
    return -1;
  if (!ReplaceOrCreatePeerConnection(remote_user_id_, "direct",
                                     connection_info_))
    return -1;
  direct_started_ = true;
  if (offerer) {
    IceWorkMsg msg{};
    msg.type = IceWorkMsg::UserJoinTransmission;
    msg.transmission_id = local_transmission_id_;
    msg.remote_user_id = remote_user_id_;
    PushIceWorkMsg(msg);
  }
  return 0;
}

void PeerConnection::ReportDirectError(MiniRtcDirectError error) {
  if (!direct_ || direct_error_reported_.exchange(true)) return;
  direct_closing_ = true;
  if (direct_error_) direct_error_(error, direct_context_);
  if (!direct_terminal_reported_.exchange(true) && on_connection_status_)
    on_connection_status_(ConnectionStatus::Failed, remote_user_id_.data(),
                          remote_user_id_.size(), user_data_);
}

int PeerConnection::ReceiveDirect(const char* message, size_t size) {
  std::unique_lock lock(direct_mutex_);
  if (!direct_ || !direct_started_ || direct_closing_) return -1;
  const auto fail = [&] {
    lock.unlock();
    ReportDirectError(MiniRtcDirectInvalidSignal);
    return -1;
  };
  if (!message || !size || size > 256 * 1024 ||
      ++direct_received_messages_ > 256 ||
      (direct_received_bytes_ += size) > 1024 * 1024)
    return fail();
  const auto j = nlohmann::json::parse(message, message + size, nullptr, false);
  if (!j.is_object() || !j.contains("type") || !j["type"].is_string() ||
      !j.contains("session_id") || j["session_id"] != local_transmission_id_ ||
      j.contains("user_id") || j.contains("remote_user_id"))
    return fail();
  const auto type = j["type"].get<std::string>();
  IceWorkMsg msg{};
  msg.transmission_id = local_transmission_id_;
  msg.remote_user_id = remote_user_id_;
  if (type == "offer" || type == "answer") {
    if (direct_remote_sdp_ || (type == "answer") != direct_offerer_ ||
        !j.contains("sdp") || !j["sdp"].is_string())
      return fail();
    msg.remote_sdp = j["sdp"].get<std::string>();
    std::string remote_ufrag;
    if (!ValidateDirectSdp(msg.remote_sdp,
                           connection_info_.direct_policy.remote_address,
                           &remote_ufrag, &direct_candidates_) ||
        (!direct_remote_ufrag_.empty() && remote_ufrag != direct_remote_ufrag_))
      return fail();
    direct_remote_ufrag_ = std::move(remote_ufrag);
    direct_remote_sdp_ = true;
    msg.type = type == "offer" ? IceWorkMsg::Offer : IceWorkMsg::Answer;
  } else if (type == "candidate") {
    if (direct_remote_end_ || (!direct_offerer_ && !direct_remote_sdp_) ||
        !j.contains("ufrag") || !j["ufrag"].is_string())
      return fail();
    msg.candidate_ufrag = j["ufrag"].get<std::string>();
    if (msg.candidate_ufrag.empty() || msg.candidate_ufrag.size() > 256 ||
        (!direct_remote_ufrag_.empty() &&
         msg.candidate_ufrag != direct_remote_ufrag_))
      return fail();
    // This protocol has one ICE generation. Candidates received before SDP
    // must agree with each other as well as with the eventual SDP credentials.
    direct_remote_ufrag_ = msg.candidate_ufrag;
    const bool end = j.contains("end_of_candidates");
    if (end) {
      if (j["end_of_candidates"] != true || j.contains("candidate"))
        return fail();
      direct_remote_end_ = true;
      msg.new_candidate = "a=end-of-candidates\r\n";
    } else {
      if (!j.contains("candidate") || !j["candidate"].is_string())
        return fail();
      msg.new_candidate = j["candidate"].get<std::string>();
      const auto normalized = NormalizeDirectCandidate(
          msg.new_candidate, connection_info_.direct_policy.remote_address);
      if (!normalized) return fail();
      if (!direct_candidates_.insert(*normalized).second) return 0;
      if (direct_candidates_.size() > 128) return fail();
    }
    msg.type = IceWorkMsg::NewCandidate;
  } else
    return fail();
  PushIceWorkMsg(msg);
  return 0;
}

int PeerConnection::CloseDirect() {
  {
    std::lock_guard lock(direct_mutex_);
    if (!direct_) return -1;
    if (!inited_) return 0;
    direct_closing_ = true;
    inited_ = false;
  }
  return Destroy();
}
}  // namespace minirtc
