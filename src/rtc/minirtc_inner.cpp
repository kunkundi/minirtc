#include "minirtc_inner.h"

#include <iostream>
#include <nlohmann/json.hpp>

#include "ice_agent.h"
#include "log.h"
#include "minirtc.h"
#include "ws_client.h"

using nlohmann::json;

PeerPtr* CreatePeer(const Params* params) {
  if (!params) {
    std::cerr << "Params is null" << std::endl;
    return nullptr;
  }

  if (params->log_path) {
    InitLogger(params->log_path);
  } else {
    InitLogger("logs");
  }

  PeerPtr* peer_ptr = new PeerPtr;
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
    peer_ptr->pc_params.stun_server_ip = params->stun_server_ip;
    peer_ptr->pc_params.stun_server_port = params->stun_server_port;
    peer_ptr->pc_params.turn_server_ip = params->turn_server_ip;
    peer_ptr->pc_params.turn_server_port = params->turn_server_port;
    peer_ptr->pc_params.turn_server_username = params->turn_server_username;
    peer_ptr->pc_params.turn_server_password = params->turn_server_password;
    peer_ptr->pc_params.hardware_acceleration = params->hardware_acceleration;
    peer_ptr->pc_params.av1_encoding = params->av1_encoding;
    peer_ptr->pc_params.enable_turn = params->enable_turn;
    peer_ptr->pc_params.enable_srtp = params->enable_srtp;
    peer_ptr->pc_params.video_quality = params->video_quality;
  }
  peer_ptr->pc_params.on_receive_video_buffer = params->on_receive_video_buffer;
  peer_ptr->pc_params.on_receive_audio_buffer = params->on_receive_audio_buffer;
  peer_ptr->pc_params.on_receive_data_buffer = params->on_receive_data_buffer;

  peer_ptr->pc_params.on_receive_video_frame = params->on_receive_video_frame;

  peer_ptr->pc_params.on_signal_status = params->on_signal_status;
  peer_ptr->pc_params.on_signal_message = params->on_signal_message;
  peer_ptr->pc_params.on_connection_status = params->on_connection_status;
  peer_ptr->pc_params.on_net_status_report = params->on_net_status_report;

  peer_ptr->pc_params.user_id = params->user_id;
  peer_ptr->pc_params.user_data = params->user_data;

  return peer_ptr;
}

void DestroyPeer(PeerPtr** peer_ptr) {
  (*peer_ptr)->peer_connection->Destroy();
  delete (*peer_ptr)->peer_connection;
  (*peer_ptr)->peer_connection = nullptr;
  delete *peer_ptr;
  *peer_ptr = nullptr;
}

int Init(PeerPtr* peer_ptr) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  peer_ptr->peer_connection->Init(peer_ptr->pc_params);
  return 0;
}

int JoinConnection(PeerPtr* peer_ptr, const char* transmission_id) {
  int ret = -1;
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  ret = peer_ptr->peer_connection->Join(transmission_id);
  // LOG_INFO("JoinConnection [{}]", transmission_id);
  return ret;
}

int LeaveConnection(PeerPtr* peer_ptr, const char* transmission_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  LOG_INFO("LeaveConnection");
  peer_ptr->peer_connection->Leave(transmission_id);
  return 0;
}

int AddVideoStream(PeerPtr* peer_ptr, const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->AddVideoStream(stream_id);
}

int AddAudioStream(PeerPtr* peer_ptr, const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->AddAudioStream(stream_id);
}

int AddDataStream(PeerPtr* peer_ptr, const char* stream_id, bool reliable) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->AddDataStream(stream_id, reliable);
}

int SendVideoFrame(PeerPtr* peer_ptr, const XVideoFrame* video_frame,
                   const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  if (!video_frame) {
    LOG_ERROR("Invaild video frame");
    return -1;
  } else if (!video_frame->data || video_frame->size <= 0) {
    LOG_ERROR("Invaild video frame");
    return -1;
  }

  peer_ptr->peer_connection->SendVideoFrame(video_frame, stream_id);

  return 0;
}

int RequestVideoKeyFrame(PeerPtr* peer_ptr, const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  return peer_ptr->peer_connection->RequestVideoKeyFrame(stream_id);
}

int RequestAllVideoKeyFrames(PeerPtr* peer_ptr) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  return peer_ptr->peer_connection->RequestAllVideoKeyFrames();
}

int SendAudioFrame(PeerPtr* peer_ptr, const char* data, size_t size,
                   const char* stream_id) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  if (!data || size <= 0) {
    LOG_ERROR("Invaild video frame");
    return -1;
  }

  peer_ptr->peer_connection->SendAudioFrame(data, size, stream_id);

  return 0;
}

int SendDataFrame(PeerPtr* peer_ptr, const char* data, size_t size,
                  const char* stream_id) {
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

int SendReliableDataFrame(PeerPtr* peer_ptr, const char* data, size_t size,
                          const char* stream_id) {
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

int SendVideoFrameToPeer(PeerPtr* peer_ptr, const XVideoFrame* video_frame,
                         const char* stream_id, const char* remote_peer_id,
                         size_t remote_peer_id_size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  peer_ptr->peer_connection->SendVideoFrameToPeer(
      video_frame, stream_id, remote_peer_id, remote_peer_id_size);

  return 0;
}

int SendAudioFrameToPeer(PeerPtr* peer_ptr, const char* data, size_t size,
                         const char* stream_id, const char* remote_peer_id,
                         size_t remote_peer_id_size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  peer_ptr->peer_connection->SendAudioFrameToPeer(
      data, size, stream_id, remote_peer_id, remote_peer_id_size);

  return 0;
}

int SendDataFrameToPeer(PeerPtr* peer_ptr, const char* data, size_t size,
                        const char* stream_id, const char* remote_peer_id,
                        size_t remote_peer_id_size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  peer_ptr->peer_connection->SendDataFrameToPeer(
      data, size, stream_id, remote_peer_id, remote_peer_id_size);

  return 0;
}

int SendReliableDataFrameToPeer(PeerPtr* peer_ptr, const char* data,
                                size_t size, const char* stream_id,
                                const char* remote_peer_id,
                                size_t remote_peer_id_size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }

  peer_ptr->peer_connection->SendReliableDataFrameToPeer(
      data, size, stream_id, remote_peer_id, remote_peer_id_size);

  return 0;
}

int64_t GetSystemTimeMicros(PeerPtr* peer_ptr) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->GetSystemTimeMicros();
}

int SendSignalMessage(PeerPtr* peer_ptr, const char* message, size_t size) {
  if (!peer_ptr || !peer_ptr->peer_connection) {
    LOG_ERROR("Peer connection not created");
    return -1;
  }
  return peer_ptr->peer_connection->SendSignalMessage(message, size);
}
