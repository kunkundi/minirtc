#ifndef _MINIRTC_H_
#define _MINIRTC_H_

#if defined(_MSC_VER)
#define MINIRTC_API __declspec(dllexport)
#elif defined(__GNUC__)
#define MINIRTC_API __attribute__((visibility("default")))
#else
#define MINIRTC_API
#endif

#include <cstddef>
#include <cstdint>

enum DATA_TYPE { VIDEO = 0, AUDIO, DATA };
enum ConnectionStatus {
  Connecting = 0,
  Connected,
  Gathering,
  Disconnected,
  Failed,
  Closed,
  IncorrectPassword,
  NoSuchTransmissionId,
  RemoteUnavailable
};

enum SignalStatus {
  SignalConnecting = 0,
  SignalConnected,
  SignalFailed,
  SignalClosed,
  SignalReconnecting,
  SignalServerClosed,
  SignalTlsCertError
};

enum TraversalMode { P2P = 0, Relay, UnknownMode };

enum VideoQuality { QualityLow = 0, QualityMedium, QualityHigh };

enum class VideoContentType : uint8_t {
  RealtimeVideo = 0,
  ScreenContent = 1,
};


enum class VideoDegradationPreference : uint8_t {
  MaintainFrameRate = 0,
  MaintainResolution = 1,
  Balanced = 2,
};

enum TurnMode : uint8_t {
  TurnDisabled = 0,  // Direct/STUN candidates only.
  TurnAutoUdpTcp,    // Direct candidates plus TURN/UDP and TURN/TCP fallback.
  TurnForceUdp,      // Relay-only candidates using TURN/UDP.
  TurnForceTcp,      // Relay-only candidates using TURN/TCP.
};

enum XNativeVideoFrameType : uint32_t {
  XNativeVideoFrameNone = 0,
  XNativeVideoFrameCpuNv12 = 1,
  XNativeVideoFrameCudaNv12 = 2,
  XNativeVideoFrameCVPixelBuffer = 3,
};

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const uint8_t* y_plane;
  const uint8_t* uv_plane;
  uint32_t y_stride;
  uint32_t uv_stride;
} XCpuNv12Frame;

typedef struct {
  uint64_t y_device_pointer;
  uint64_t uv_device_pointer;
  uint32_t y_stride;
  uint32_t uv_stride;
  void* context;
} XCudaNv12Frame;

typedef union {
  XCpuNv12Frame cpu_nv12;
  XCudaNv12Frame cuda_nv12;
  void* cv_pixel_buffer;
} XNativeVideoFramePayload;

// A tagged, ref-counted video-frame descriptor. NV12 data is interpreted as
// video (limited) range by default. Exactly one payload member is active,
// selected by type. The descriptor itself is borrowed for the API call or
// callback; retain owner before keeping a copy and release it after last use.
typedef struct {
  uint32_t struct_size;
  XNativeVideoFrameType type;
  uint32_t width;
  uint32_t height;
  XNativeVideoFramePayload payload;
  void* owner;
  void (*retain)(void* owner);
  void (*release)(void* owner);
  int (*copy_to_nv12)(void* owner, uint8_t* destination,
                      size_t destination_size);
} XNativeVideoFrame;

typedef struct {
  // Packed NV12 pixels. May be null when native_frame is provided.
  const char* data;
  // Logical packed NV12 size, including for native frames.
  size_t size;
  uint32_t width;
  uint32_t height;
  uint64_t captured_timestamp;
  uint64_t received_timestamp;
  uint64_t decoded_timestamp;
  uint64_t rendered_timestamp;
  // Optional platform-neutral capture or decoded frame descriptor. The pointer
  // is valid only for the duration of the API call or receive callback; follow
  // the descriptor's retain/release contract before keeping a copy.
  const XNativeVideoFrame* native_frame;
} XVideoFrame;

typedef struct {
  uint32_t bitrate;
  uint32_t rtp_packet_count;
  float loss_rate;
} XInboundStats;

typedef struct {
  uint32_t bitrate;
  uint32_t rtp_packet_count;
} XOutboundStats;

typedef struct {
  XInboundStats video_inbound_stats;
  XOutboundStats video_outbound_stats;
  XInboundStats audio_inbound_stats;
  XOutboundStats audio_outbound_stats;
  XInboundStats data_inbound_stats;
  XOutboundStats data_outbound_stats;
  XInboundStats total_inbound_stats;
  XOutboundStats total_outbound_stats;
} XNetTrafficStats;

typedef struct Peer PeerPtr;

typedef void (*OnReceiveBuffer)(const char* data, size_t size,
                                const char* remote_peer_id,
                                const size_t remote_peer_id_size,
                                const char* source_id,
                                const size_t source_id_size, void* user_data);

typedef void (*OnReceiveVideoFrame)(const XVideoFrame* video_frame,
                                    const char* remote_peer_id,
                                    const size_t remote_peer_id_size,
                                    const char* source_id,
                                    const size_t source_id_size,
                                    void* user_data);

typedef void (*OnSignalStatus)(SignalStatus status, const char* peer_id,
                               const size_t peer_id_size, void* user_data);

typedef void (*OnSignalMessage)(const char* message, size_t size,
                                void* user_data);

typedef void (*OnConnectionStatus)(ConnectionStatus status,
                                   const char* remote_peer_id,
                                   const size_t remote_peer_id_size,
                                   void* user_data);

typedef void (*OnNetStatusReport)(const char* peer_id,
                                  const size_t peer_id_size, TraversalMode mode,
                                  const XNetTrafficStats* stats,
                                  const char* remote_peer_id,
                                  const size_t remote_peer_id_size,
                                  void* user_data);

typedef struct {
  bool use_cfg_file;
  char cfg_path[256];

  char signal_server_ip[256];
  int signal_server_port;
  char stun_server_ip[256];
  int stun_server_port;
  char turn_server_ip[256];
  int turn_server_port;
  char turn_server_username[256];
  char turn_server_password[256];
  char log_path[256];
  bool hardware_acceleration;
  // Prefer XVideoFrame::native_frame instead of copying decoded pixels into
  // XVideoFrame::data. Decoders that cannot expose a descriptor ignore this
  // option and continue to provide CPU data.
  bool native_video_output;
  bool av1_encoding;
  TurnMode turn_mode;
  bool enable_srtp;
  VideoContentType video_content_type;

  VideoQuality video_quality;
  uint32_t video_frame_rate;
  VideoDegradationPreference video_degradation_preference;

  OnReceiveBuffer on_receive_video_buffer;
  OnReceiveBuffer on_receive_audio_buffer;
  OnReceiveBuffer on_receive_data_buffer;

  OnReceiveVideoFrame on_receive_video_frame;

  OnSignalStatus on_signal_status;
  OnSignalMessage on_signal_message;
  OnConnectionStatus on_connection_status;
  OnNetStatusReport on_net_status_report;

  const char* user_id;
  void* user_data;
} Params;

// Create a Peer instance
MINIRTC_API PeerPtr* CreatePeer(const Params* params);

// Destroy a Peer instance
MINIRTC_API void DestroyPeer(PeerPtr** peer_ptr);

// Initialize a Peer instance
MINIRTC_API int Init(PeerPtr* peer_ptr);

// Join a connection
MINIRTC_API int JoinConnection(PeerPtr* peer_ptr, const char* transmission_id);

// Leave a connection
MINIRTC_API int LeaveConnection(PeerPtr* peer_ptr, const char* transmission_id);

// Add media/data streams
MINIRTC_API int AddVideoStream(PeerPtr* peer_ptr, const char* stream_id);

MINIRTC_API int AddAudioStream(PeerPtr* peer_ptr, const char* stream_id);

MINIRTC_API int AddDataStream(PeerPtr* peer_ptr, const char* stream_id,
                              bool reliable);

// Send media/data frames to all peers
MINIRTC_API int SendVideoFrame(PeerPtr* peer_ptr,
                               const XVideoFrame* video_frame,
                               const char* stream_id);

MINIRTC_API int RequestVideoKeyFrame(PeerPtr* peer_ptr,
                                     const char* stream_id);

MINIRTC_API int RequestAllVideoKeyFrames(PeerPtr* peer_ptr);

MINIRTC_API int SendAudioFrame(PeerPtr* peer_ptr, const char* data, size_t size,
                               const char* stream_id);

MINIRTC_API int SendDataFrame(PeerPtr* peer_ptr, const char* data, size_t size,
                              const char* stream_id);

MINIRTC_API int SendReliableDataFrame(PeerPtr* peer_ptr, const char* data,
                                      size_t size, const char* stream_id);

// Send media/data frames to peer
MINIRTC_API int SendVideoFrameToPeer(PeerPtr* peer_ptr,
                                     const XVideoFrame* video_frame,
                                     const char* stream_id,
                                     const char* remote_peer_id,
                                     size_t remote_peer_id_size);

MINIRTC_API int SendAudioFrameToPeer(PeerPtr* peer_ptr, const char* data,
                                     size_t size, const char* stream_id,
                                     const char* remote_peer_id,
                                     size_t remote_peer_id_size);

MINIRTC_API int SendDataFrameToPeer(PeerPtr* peer_ptr, const char* data,
                                    size_t size, const char* stream_id,
                                    const char* remote_peer_id,
                                    size_t remote_peer_id_size);

MINIRTC_API int SendReliableDataFrameToPeer(PeerPtr* peer_ptr, const char* data,
                                            size_t size, const char* stream_id,
                                            const char* remote_peer_id,
                                            size_t remote_peer_id_size);

// Send signal message
MINIRTC_API int SendSignalMessage(PeerPtr* peer_ptr, const char* message,
                                  size_t size);

// Get system time in microseconds
MINIRTC_API int64_t GetSystemTimeMicros(PeerPtr* peer_ptr);

#ifdef __cplusplus
}
#endif

#endif
