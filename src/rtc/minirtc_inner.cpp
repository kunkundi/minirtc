#include "minirtc_inner.h"

#include <iostream>
#include <nlohmann/json.hpp>

#include "ice_agent.h"
#include "log.h"
#include "minirtc.h"
#include "native_video_frame.h"
#include "ws_client.h"

using nlohmann::json;

namespace {

bool IsValidAudioFrameInput(const MiniRtcAudioFrame* audio_frame) {
  return audio_frame && audio_frame->data && audio_frame->size > 0;
}

}  // namespace

MiniRtcPeerPtr* MiniRtcCreatePeer(const MiniRtcParams* params) {
  if (!params) {
    std::cerr << "Params is null" << std::endl;
    return nullptr;
  }

  if (params->log_path[0] != '\0') {
    InitLogger(params->log_path);
  } else {
    InitLogger("logs");
  }

  MiniRtcPeerPtr* peer_ptr = new MiniRtcPeerPtr{};
#ifdef USE_LIBDATACHANNEL_CONNECTION
  peer_ptr->peer_connection = new DataChannelConnection();
#else
  peer_ptr->peer_connection = new PeerConnection();
#endif
  peer_ptr->pc_params.use_cfg_file = params->use_cfg_file;
  if (params->use_cfg_file) {
    peer_ptr->pc_params.cfg_path = params->cfg_path;
  } else {
    peer_ptr->pc_params.signal_server_ip = params->signal_server_ip;
    peer_ptr->pc_params.signal_server_port = params->signal_server_port;
    peer_ptr->pc_params.hardware_acceleration = params->hardware_acceleration;
    peer_ptr->pc_params.native_video_output = params->native_video_output;
    peer_ptr->pc_params.av1_encoding = params->av1_encoding;
    peer_ptr->pc_params.turn_mode = params->turn_mode;
    peer_ptr->pc_params.enable_srtp = params->enable_srtp;
    peer_ptr->pc_params.video_content_type = params->video_content_type;
    peer_ptr->pc_params.video_quality = params->video_quality;
    peer_ptr->pc_params.video_frame_rate = params->video_frame_rate;
    peer_ptr->pc_params.video_degradation_preference =
        params->video_degradation_preference;
    peer_ptr->pc_params.direct_start_bitrate_bps = params->direct_start_bitrate_bps;
    peer_ptr->pc_params.relay_start_bitrate_bps = params->relay_start_bitrate_bps;
  }
  peer_ptr->pc_params.on_receive_video_buffer = params->on_receive_video_buffer;
  peer_ptr->pc_params.on_receive_audio_buffer = params->on_receive_audio_buffer;
  peer_ptr->pc_params.on_receive_data_buffer = params->on_receive_data_buffer;

  peer_ptr->pc_params.on_receive_video_frame = params->on_receive_video_frame;
  peer_ptr->pc_params.on_receive_encoded_video =
      params->on_receive_encoded_video;

  peer_ptr->pc_params.on_signal_status = params->on_signal_status;
  peer_ptr->pc_params.on_signal_message = params->on_signal_message;
  peer_ptr->pc_params.on_signal_exchange = params->on_signal_exchange;
  peer_ptr->pc_params.on_connection_status = params->on_connection_status;
  peer_ptr->pc_params.on_net_status_report = params->on_net_status_report;

  peer_ptr->pc_params.user_id = params->user_id;
  peer_ptr->pc_params.user_data = params->user_data;

  return peer_ptr;
}

void MiniRtcDestroyPeer(MiniRtcPeerPtr** peer_ptr) {
  (*peer_ptr)->peer_connection->Destroy();
  delete (*peer_ptr)->peer_connection;
  (*peer_ptr)->peer_connection = nullptr;
  delete *peer_ptr;
  *peer_ptr = nullptr;
}

int MiniRtcInit(MiniRtcPeerPtr* peer_ptr) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  peer_ptr->peer_connection->Init(peer_ptr->pc_params);
  return 0;
}

int MiniRtcInitDirectPeer(MiniRtcPeerPtr* peer,
                          const MiniRtcDirectOptions* options) {
  if (!peer || !peer->peer_connection || !options) return -1;
  try {
    return peer->peer_connection->InitDirect(peer->pc_params, *options);
  } catch (...) {
    return -1;
  }
}

int MiniRtcStartDirectSession(MiniRtcPeerPtr* peer, bool offerer) {
  if (!peer || !peer->peer_connection) return -1;
  try {
    return peer->peer_connection->StartDirect(offerer);
  } catch (...) {
    return -1;
  }
}

int MiniRtcReceiveDirectSignal(MiniRtcPeerPtr* peer, const char* message,
                               size_t size) {
  if (!peer || !peer->peer_connection) return -1;
  try {
    return peer->peer_connection->ReceiveDirect(message, size);
  } catch (...) {
    peer->peer_connection->ReportDirectError(MiniRtcDirectInvalidSignal);
    return -1;
  }
}

int MiniRtcCloseDirectSession(MiniRtcPeerPtr* peer) {
  return peer && peer->peer_connection ? peer->peer_connection->CloseDirect()
                                       : -1;
}

int MiniRtcJoinConnection(MiniRtcPeerPtr* peer_ptr,
                          const char* transmission_id) {
  int ret = -1;
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  ret = peer_ptr->peer_connection->Join(transmission_id);
  // LOG_INFO("JoinConnection [{}]", transmission_id);
  return ret;
}

int MiniRtcUpdateConnectionSettings(MiniRtcPeerPtr* peer_ptr,
                                    bool hardware_acceleration,
                                    bool av1_encoding,
                                    MiniRtcTurnMode turn_mode) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    return -1;
  }
  return peer_ptr->peer_connection->UpdateConnectionSettings(
      hardware_acceleration, av1_encoding, turn_mode);
}

int MiniRtcLeaveConnection(MiniRtcPeerPtr* peer_ptr,
                           const char* transmission_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  LOG_INFO("LeaveConnection");
  peer_ptr->peer_connection->Leave(transmission_id);
  return 0;
}

int MiniRtcDisconnectPeerConnection(MiniRtcPeerPtr* peer_ptr,
                                    const char* transmission_id,
                                    const char* remote_user_id) {
  if (!peer_ptr || !peer_ptr->peer_connection || !transmission_id ||
      !*transmission_id || !remote_user_id || !*remote_user_id) {
    return -1;
  }
  return peer_ptr->peer_connection->DisconnectPeer(transmission_id,
                                                   remote_user_id);
}

int MiniRtcAddVideoStream(MiniRtcPeerPtr* peer_ptr, const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->AddVideoStream(stream_id);
}

int MiniRtcAddAudioStream(MiniRtcPeerPtr* peer_ptr, const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->AddAudioStream(stream_id);
}

int MiniRtcAddDataStream(MiniRtcPeerPtr* peer_ptr, const char* stream_id,
                         bool reliable) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->AddDataStream(stream_id, reliable);
}

int MiniRtcSendVideoFrame(MiniRtcPeerPtr* peer_ptr,
                          const MiniRtcVideoFrame* video_frame,
                          const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  if (!minirtc::IsValidVideoFrameInput(video_frame)) {
    LOG_ERROR("Invalid video frame");
    return -1;
  }

  peer_ptr->peer_connection->SendVideoFrame(video_frame, stream_id);

  return 0;
}

int MiniRtcRequestVideoKeyFrame(MiniRtcPeerPtr* peer_ptr,
                                const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  return peer_ptr->peer_connection->RequestVideoKeyFrame(stream_id);
}

int MiniRtcUpdateVideoSettings(MiniRtcPeerPtr* peer_ptr, const char* remote_id,
                               size_t remote_id_size,
                               MiniRtcVideoQuality quality, int frame_rate,
                               MiniRtcVideoDegradationPreference preference) {
  if (!peer_ptr || !peer_ptr->peer_connection || !remote_id ||
      !remote_id_size || quality < MiniRtcVideoQualityLow ||
      quality > MiniRtcVideoQualityHigh ||
      (frame_rate != 30 && frame_rate != 60) ||
      preference > MiniRtcVideoDegradationPreference::Balanced)
    return -1;
  return peer_ptr->peer_connection->UpdateVideoSettings(
      std::string(remote_id, remote_id_size), quality, frame_rate, preference);
}

int MiniRtcUpdateVideoBitrateLimit(MiniRtcPeerPtr* peer_ptr,
                                   const char* remote_id, size_t remote_id_size,
                                   int max_bitrate_bps) {
  if (!peer_ptr || !peer_ptr->peer_connection || !remote_id ||
      !remote_id_size || max_bitrate_bps < 0 || max_bitrate_bps > 100'000'000)
    return -1;
  return peer_ptr->peer_connection->UpdateVideoBitrateLimit(
      std::string(remote_id, remote_id_size), max_bitrate_bps);
}

int MiniRtcSetVideoSendPaused(MiniRtcPeerPtr* peer_ptr, const char* remote_id,
                              size_t remote_id_size, bool paused) {
  if (!peer_ptr || !peer_ptr->peer_connection || !remote_id || !remote_id_size)
    return -1;
  return peer_ptr->peer_connection->SetVideoSendPaused(
      std::string(remote_id, remote_id_size), paused);
}

int MiniRtcSetAudioSendPaused(MiniRtcPeerPtr* peer_ptr, const char* remote_id,
                              size_t remote_id_size, bool paused) {
  if (!peer_ptr || !peer_ptr->peer_connection || !remote_id || !remote_id_size)
    return -1;
  return peer_ptr->peer_connection->SetAudioSendPaused(
      std::string(remote_id, remote_id_size), paused);
}

int MiniRtcUpdateVideoResolution(MiniRtcPeerPtr* peer_ptr,
                                 const char* remote_id, size_t remote_id_size,
                                 int width, int height) {
  if (!peer_ptr || !peer_ptr->peer_connection || !remote_id ||
      !remote_id_size ||
      !((width == 0 && height == 0) ||
        (width >= 2 && height >= 2 && width <= 16384 && height <= 16384)))
    return -1;
  return peer_ptr->peer_connection->UpdateVideoResolution(
      std::string(remote_id, remote_id_size), width, height);
}

int MiniRtcRequestAllVideoKeyFrames(MiniRtcPeerPtr* peer_ptr) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  return peer_ptr->peer_connection->RequestAllVideoKeyFrames();
}

int MiniRtcSendAudioFrame(MiniRtcPeerPtr* peer_ptr,
                          const MiniRtcAudioFrame* audio_frame,
                          const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  if (!IsValidAudioFrameInput(audio_frame)) {
    LOG_ERROR("Invalid audio frame");
    return -1;
  }

  return peer_ptr->peer_connection->SendAudioFrame(audio_frame, stream_id);
}

int MiniRtcSendDataFrame(MiniRtcPeerPtr* peer_ptr, const char* data,
                         size_t size, const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  if (!data || size <= 0) {
    LOG_ERROR("Invaild data");
    return -1;
  }

  peer_ptr->peer_connection->SendDataFrame(data, size, stream_id);

  return 0;
}

int MiniRtcSendReliableDataFrame(MiniRtcPeerPtr* peer_ptr, const char* data,
                                 size_t size, const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  if (!data || size <= 0) {
    LOG_ERROR("Invaild data");
    return -1;
  }

  peer_ptr->peer_connection->SendReliableDataFrame(data, size, stream_id);

  return 0;
}

int MiniRtcSendVideoFrameToPeer(MiniRtcPeerPtr* peer_ptr,
                                const MiniRtcVideoFrame* video_frame,
                                const char* stream_id,
                                const char* remote_peer_id,
                                size_t remote_peer_id_size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  if (!minirtc::IsValidVideoFrameInput(video_frame)) {
    LOG_ERROR("Invalid video frame");
    return -1;
  }

  peer_ptr->peer_connection->SendVideoFrameToPeer(
      video_frame, stream_id, remote_peer_id, remote_peer_id_size);

  return 0;
}

int MiniRtcSendAudioFrameToPeer(MiniRtcPeerPtr* peer_ptr,
                                const MiniRtcAudioFrame* audio_frame,
                                const char* stream_id,
                                const char* remote_peer_id,
                                size_t remote_peer_id_size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  if (!IsValidAudioFrameInput(audio_frame)) {
    LOG_ERROR("Invalid audio frame");
    return -1;
  }

  return peer_ptr->peer_connection->SendAudioFrameToPeer(
      audio_frame, stream_id, remote_peer_id, remote_peer_id_size);
}

int MiniRtcSendDataFrameToPeer(MiniRtcPeerPtr* peer_ptr, const char* data,
                               size_t size, const char* stream_id,
                               const char* remote_peer_id,
                               size_t remote_peer_id_size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  peer_ptr->peer_connection->SendDataFrameToPeer(
      data, size, stream_id, remote_peer_id, remote_peer_id_size);

  return 0;
}

int MiniRtcSendReliableDataFrameToPeer(MiniRtcPeerPtr* peer_ptr,
                                       const char* data, size_t size,
                                       const char* stream_id,
                                       const char* remote_peer_id,
                                       size_t remote_peer_id_size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  return peer_ptr->peer_connection->SendReliableDataFrameToPeer(
      data, size, stream_id, remote_peer_id, remote_peer_id_size);
}

int64_t MiniRtcGetSystemTimeMicros(MiniRtcPeerPtr* peer_ptr) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->GetSystemTimeMicros();
}

int MiniRtcSendSignalMessage(MiniRtcPeerPtr* peer_ptr, const char* message,
                             size_t size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->SendSignalMessage(message, size);
}

int MiniRtcReconnectSignal(MiniRtcPeerPtr* peer_ptr) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->ForceSignalReconnect();
}
