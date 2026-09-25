#ifndef _MINIRTC_H_
#define _MINIRTC_H_

/**
 * @file minirtc.h
 * @brief Public C++17 API for signaling, media, and application data transport.
 *
 * Functions have C linkage, but the types in this header require C++.
 * Typical lifecycle: zero-initialize Params, set callbacks and endpoints,
 * CreatePeer, register streams, Init, wait for SignalConnected, JoinConnection,
 * wait for Connected, send/receive, LeaveConnection, then DestroyPeer.
 *
 * Callbacks may run on internal worker threads and may overlap. Keep them short,
 * synchronize application state, and dispatch UI work to the appropriate thread.
 * Do not let exceptions escape callbacks or synchronously destroy their Peer.
 * Callback buffers, identifiers, frames, and statistics are borrowed for the
 * callback duration; copy data or retain a native frame before asynchronous use.
 */

#if defined(_MSC_VER)
#define MINIRTC_API __declspec(dllexport)
#elif defined(__GNUC__)
#define MINIRTC_API __attribute__((visibility("default")))
#else
#define MINIRTC_API
#endif

#include <cstddef>
#include <cstdint>

/// Legacy payload categories; current send functions use typed frames/streams.
enum DATA_TYPE { VIDEO = 0, AUDIO, DATA };

/// Status of a remote session, independent of the WSS signaling connection.
enum ConnectionStatus {
  Connecting = 0,       ///< Session negotiation or transport setup is starting.
  Connected,           ///< The transport reports that the session is usable.
  Gathering,           ///< ICE candidates are being gathered.
  Disconnected,        ///< Connectivity was lost; the session may be retired.
  Failed,              ///< Negotiation or transport establishment failed.
  Closed,              ///< The session was closed or explicitly cleared.
  IncorrectPassword,   ///< The signaling server rejected the target password.
  NoSuchTransmissionId, ///< The signaling server could not find the target ID.
  RemoteUnavailable     ///< The target exists but is not available for joining.
};

/// Signaling connection/login events; Init() does not wait for these events.
enum SignalStatus {
  SignalConnecting = 0, ///< Opening the signaling connection.
  SignalConnected,     ///< Signaling login completed successfully.
  SignalFailed,        ///< Connection or login failed.
  SignalClosed,        ///< Signaling connection closed.
  SignalReconnecting,  ///< Attempting to reconnect to the signaling server.
  SignalServerClosed,  ///< Signaling transport reported server closure.
  SignalTlsCertError   ///< TLS certificate verification failed.
};

/// Observed transport path in a network report, not a requested TURN policy.
enum TraversalMode { P2P = 0, Relay, UnknownMode };

/// Video quality preset used by adaptation/bitrate policy, not a fixed bitrate.
enum VideoQuality { QualityLow = 0, QualityMedium, QualityHigh };

/// Content hint supplied to video encoding and adaptation logic.
enum class VideoContentType : uint8_t {
  RealtimeVideo = 0, ///< Camera/video content.
  ScreenContent = 1, ///< Screen sharing and desktop content.
};


/// Preferred tradeoff when video must adapt to bandwidth or processing limits.
enum class VideoDegradationPreference : uint8_t {
  MaintainFrameRate = 0,   ///< Prefer temporal smoothness over resolution.
  MaintainResolution = 1, ///< Prefer spatial detail over frame rate.
  Balanced = 2,           ///< Balance frame-rate and resolution adjustments.
};

/// ICE candidate policy; enabled relay modes still require valid TURN access.
enum TurnMode : uint8_t {
  TurnDisabled = 0, ///< Direct/STUN candidates only.
  TurnAutoUdpTcp,   ///< Direct candidates plus TURN/UDP and TURN/TCP fallback.
  TurnForceUdp,     ///< Relay-only candidates using TURN/UDP.
  TurnForceTcp,     ///< Relay-only candidates using TURN/TCP.
};

/// Tag selecting exactly one MiniRtcNativeVideoFramePayload member.
enum MiniRtcNativeVideoFrameType : uint32_t {
  MiniRtcNativeVideoFrameNone = 0,          ///< No native payload.
  MiniRtcNativeVideoFrameCpuNv12 = 1,       ///< Host-accessible NV12 planes.
  MiniRtcNativeVideoFrameCudaNv12 = 2,      ///< CUDA device-memory NV12 planes.
  MiniRtcNativeVideoFrameCVPixelBuffer = 3, ///< Apple CVPixelBufferRef payload.
};

#ifdef __cplusplus
extern "C" {
#endif

/// NV12 planes in CPU memory. Strides are bytes per row, including padding.
typedef struct {
  const uint8_t* y_plane;  ///< Luma plane, height rows.
  const uint8_t* uv_plane; ///< Interleaved UV plane, height / 2 rows.
  uint32_t y_stride;      ///< At least the frame width in bytes.
  uint32_t uv_stride;     ///< At least the frame width in bytes.
} MiniRtcCpuNv12Frame;

/// NV12 in CUDA memory; device addresses must not be dereferenced on the CPU.
typedef struct {
  uint64_t y_device_pointer;  ///< CUdeviceptr-compatible luma address.
  uint64_t uv_device_pointer; ///< CUdeviceptr-compatible interleaved UV address.
  uint32_t y_stride;          ///< Device luma pitch in bytes, at least width.
  uint32_t uv_stride;         ///< Device chroma pitch in bytes, at least width.
  void* context;             ///< Owning CUcontext; borrowed with the frame.
} MiniRtcCudaNv12Frame;

/// Access only the member named by MiniRtcNativeVideoFrame::type.
typedef union {
  MiniRtcCpuNv12Frame cpu_nv12;
  MiniRtcCudaNv12Frame cuda_nv12;
  void* cv_pixel_buffer; ///< NV12 CVPixelBufferRef; use Apple APIs for plane access.
} MiniRtcNativeVideoFramePayload;

/**
 * @brief Tagged, reference-counted native video storage.
 *
 * NV12 is interpreted as video (limited) range by default. A descriptor is
 * borrowed for an API call/callback. To keep it, copy the descriptor and call
 * retain(owner) before that call returns; call release(owner) after last use.
 * Keeping only the descriptor pointer does not extend its lifetime.
 *
 * Storage must remain valid and unchanged while retained. Owner callbacks must
 * support use from the library's worker threads. Sending native frames may
 * retain storage beyond the send call; sending does not transfer the caller's
 * existing reference. Input descriptors require non-null owner, retain,
 * release, and copy_to_nv12 fields.
 */
typedef struct {
  uint32_t struct_size; ///< Set to sizeof(MiniRtcNativeVideoFrame).
  MiniRtcNativeVideoFrameType type; ///< Active payload tag.
  uint32_t width;  ///< Positive, even pixel width.
  uint32_t height; ///< Positive, even pixel height.
  MiniRtcNativeVideoFramePayload payload;
  void* owner; ///< Non-null context owning the payload's storage/resources.
  void (*retain)(void* owner);  ///< Acquire one reference to the owner.
  void (*release)(void* owner); ///< Release one reference to the owner.
  /// Copy to packed CPU NV12 (Y then UV, no row padding). Destination must hold
  /// at least width * height * 3 / 2 bytes. Return 0 on success, negative on error.
  int (*copy_to_nv12)(void* owner, uint8_t* destination,
                      size_t destination_size);
} MiniRtcNativeVideoFrame;

/**
 * @brief Captured input or decoded output video frame; no storage ownership.
 *
 * CPU input needs positive even dimensions and at least width * height * 3 / 2
 * packed NV12 bytes. A valid native descriptor may be supplied instead. All
 * timestamps are microseconds, not Unix time; zero may mean unavailable.
 */
typedef struct {
  /// Packed NV12 pixels. May be null when native_frame is provided. Copy borrowed
  /// callback data before retaining it for later use.
  const char* data;
  /// Logical packed NV12 size, including for native frames.
  size_t size;
  uint32_t width;  ///< Pixel width; must match a supplied native descriptor.
  uint32_t height; ///< Pixel height; must match a supplied native descriptor.
  /// Send: capture time from GetSystemTimeMicros(); zero selects send-time
  /// fallback. Native receive: RTP/RTCP-derived estimate in the local monotonic
  /// clock, corrected using RTT/2 (symmetric-path assumption). Zero until clock
  /// calibration is ready or when it expires. Not an absolute remote time.
  uint64_t captured_timestamp;
  /// Local receive/reassembly time, when supplied by the receive path.
  uint64_t received_timestamp;
  /// Local decoder completion time, when supplied by the decoder.
  uint64_t decoded_timestamp;
  /// Reserved for application render timing; current receive code leaves zero.
  uint64_t rendered_timestamp;
  /// Optional platform-neutral capture or decoded frame descriptor. The pointer
  /// is valid only for the duration of the API call or receive callback; follow
  /// the descriptor's retain/release contract before keeping a copy.
  const MiniRtcNativeVideoFrame* native_frame;
} MiniRtcVideoFrame;

/// Captured 10 ms of 48 kHz mono signed 16-bit PCM (480 samples / 960 bytes).
/// Keep the borrowed input buffer valid until the send call returns.
typedef struct {
  const char* data; ///< PCM samples, not already encoded Opus packets.
  size_t size; ///< Byte count; current encoders require 960 bytes per frame.
  uint64_t captured_timestamp; ///< Monotonic microseconds; zero uses fallback.
} MiniRtcAudioFrame;

/// Receive statistics from the reporting transport; counters are cumulative.
typedef struct {
  uint32_t bitrate; ///< Bits per second over the latest reporting interval.
  uint32_t rtp_packet_count; ///< RTP packets counted since statistics creation.
  float loss_rate; ///< Per-media loss fraction [0, 1], not a percentage.
} MiniRtcInboundStats;

/// Send statistics from the reporting transport; not delivery acknowledgments.
typedef struct {
  uint32_t bitrate; ///< Bits per second over the latest reporting interval.
  uint32_t rtp_packet_count; ///< RTP packets counted since statistics creation.
} MiniRtcOutboundStats;

/**
 * @brief Per-media and aggregate transport statistics.
 *
 * Native transport reports approximately once per second. Totals sum video,
 * audio, and data counters/rates. In the current implementation,
 * total_inbound_stats.loss_rate also sums the three loss fractions: it is not
 * a weighted aggregate and may exceed 1. Availability depends on the backend;
 * an initial login report contains zero counters, unavailable RTT and UnknownMode.
 */
typedef struct MiniRtcNetTrafficStats {
  MiniRtcInboundStats video_inbound_stats;
  MiniRtcOutboundStats video_outbound_stats;
  MiniRtcInboundStats audio_inbound_stats;
  MiniRtcOutboundStats audio_outbound_stats;
  MiniRtcInboundStats data_inbound_stats;
  MiniRtcOutboundStats data_outbound_stats;
  MiniRtcInboundStats total_inbound_stats;
  MiniRtcOutboundStats total_outbound_stats;
  bool srtp_active;
  double rtt_ms = -1; ///< -1 without a valid sample; zero RTT remains valid.
} MiniRtcNetTrafficStats;

/// Opaque owned handle. Create with CreatePeer; release only with DestroyPeer.
typedef struct Peer PeerPtr;

/**
 * @brief Receive decoded PCM audio or application data for a named source.
 * @param data Borrowed bytes; may contain embedded zeros.
 * @param size Byte count; not a string terminator.
 * @param remote_peer_id Sender identifier, read using remote_peer_id_size.
 * @param remote_peer_id_size Sender ID byte count, excluding a trailing NUL.
 * @param source_id Source/stream label, read using source_id_size.
 * @param source_id_size Source label byte count, excluding a trailing NUL.
 * @param user_data The application context supplied in Params.
 *
 * Buffer type depends on which Params callback holds this function. Copy bytes
 * and identifiers before dispatching them to another thread or storing them.
 */
typedef void (*OnReceiveBuffer)(const char* data, size_t size,
                                const char* remote_peer_id,
                                const size_t remote_peer_id_size,
                                const char* source_id,
                                const size_t source_id_size, void* user_data);

/// Receive a borrowed decoded frame. Inspect native_frame before assuming data
/// is non-null; copy CPU pixels or copy/retain the native descriptor for later use.
/// Identifiers and user_data follow the OnReceiveBuffer contract.
typedef void (*OnReceiveVideoFrame)(const MiniRtcVideoFrame* video_frame,
                                    const char* remote_peer_id,
                                    const size_t remote_peer_id_size,
                                    const char* source_id,
                                    const size_t source_id_size,
                                    void* user_data);

/// Report signaling/login state and the current local peer ID. The server may
/// assign or change that ID at login. This callback must be set before Init.
typedef void (*OnSignalStatus)(SignalStatus status, const char* peer_id,
                               const size_t peer_id_size, void* user_data);

/// Receive a borrowed signaling payload not consumed by MiniRTC's internal
/// handler. Optional; not an observer of every raw signaling message.
typedef void (*OnSignalMessage)(const char* message, size_t size,
                                void* user_data);

/// Report state for the named remote peer. Supply a valid callback before Init;
/// several signaling paths call it directly, including failed join responses.
typedef void (*OnConnectionStatus)(ConnectionStatus status,
                                   const char* remote_peer_id,
                                   const size_t remote_peer_id_size,
                                   void* user_data);

/**
 * @brief Report the local/remote peer IDs, observed path, and borrowed statistics.
 *
 * Required before Init. Read identifiers by their supplied lengths. The initial
 * login report uses UnknownMode and zero statistics; its peer_id may contain
 * the full ID@password login identity, so do not log that value indiscriminately.
 * Copy the statistics value before using it outside this callback.
 */
typedef void (*OnNetStatusReport)(const char* peer_id,
                                  const size_t peer_id_size, TraversalMode mode,
                                  const MiniRtcNetTrafficStats* stats,
                                  const char* remote_peer_id,
                                  const size_t remote_peer_id_size,
                                  void* user_data);

/**
 * @brief Peer configuration. Start with Params{} and set required values.
 *
 * Zero initialization is not an application preset: it disables TURN/SRTP and
 * leaves endpoints unset. All character arrays must be NUL-terminated.
 * CreatePeer borrows pointers into these arrays; keep Params at a stable
 * address, and keep user_id alive, until Init has finished reading them.
 * Callbacks and user_data must remain valid until peer destruction completes.
 *
 * ICE servers and temporary credentials come only from signaling.
 * With use_cfg_file=true, the INI supplies signaling and media settings instead
 * of their direct fields below; missing INI values do not fall back to those
 * fields. Callbacks, user_id, user_data, and log_path still come from this
 * structure.
 */
typedef struct {
  bool use_cfg_file; ///< Select INI configuration instead of direct settings.
  char cfg_path[256]; ///< INI path; relative paths use the process working dir.

  /// Signaling host only: no wss:// prefix, port, or path. WSS uses system trust.
  char signal_server_ip[256];
  int signal_server_port;  ///< WSS port; no automatic default in direct mode.
  char log_path[256]; ///< Logger directory; an empty value uses "logs".
  /// Request available hardware H.264 codecs; build/device support still applies.
  bool hardware_acceleration;
  /// Prefer MiniRtcVideoFrame::native_frame instead of copying decoded pixels into
  /// MiniRtcVideoFrame::data. Decoders that cannot expose a descriptor ignore this
  /// option and continue to provide CPU data.
  bool native_video_output;
  bool av1_encoding; ///< Request AV1 video encoding; false selects H.264.
  TurnMode turn_mode; ///< Candidate policy; zero means TurnDisabled.
  bool enable_srtp; ///< Enable native media SRTP; browser WebRTC uses encryption.
  VideoContentType video_content_type; ///< Screen sharing or real-time video hint.

  VideoQuality video_quality; ///< Adaptation/quality preset.
  uint32_t video_frame_rate; ///< 30 or 60 fps; other values normalize to 60.
  VideoDegradationPreference video_degradation_preference; ///< Adaptation priority.

  /// Legacy field; current video dispatch uses on_receive_video_frame instead.
  OnReceiveBuffer on_receive_video_buffer;
  OnReceiveBuffer on_receive_audio_buffer; ///< Optional decoded PCM receiver.
  OnReceiveBuffer on_receive_data_buffer; ///< Optional application-data receiver.

  OnReceiveVideoFrame on_receive_video_frame; ///< Optional decoded-frame receiver.

  OnSignalStatus on_signal_status; ///< Required; use a no-op if unused.
  OnSignalMessage on_signal_message; ///< Optional unhandled-signaling receiver.
  OnConnectionStatus on_connection_status; ///< Required; use a no-op if unused.
  OnNetStatusReport on_net_status_report; ///< Required, including during login.

  /// Local login identity, not the target ID. CrossDesk may use ID@password;
  /// assignment/registration rules belong to the signaling service. Borrowed.
  const char* user_id;
  void* user_data; ///< Opaque caller-owned context forwarded to all callbacks.
} Params;

/**
 * @brief Allocate a peer and capture its configuration; does not connect.
 * @param params Zero-initialized configuration with required fields populated.
 * @return An owned handle, or nullptr if params is null.
 *
 * Initializes logging using log_path. Configuration is not validated here;
 * Params strings must remain valid until Init reads them. Release the handle
 * with DestroyPeer. Allocation failures are not converted to a null return.
 */
MINIRTC_API PeerPtr* CreatePeer(const Params* params);

/**
 * @brief Stop the peer, release its resources, and set the caller's handle null.
 * @param peer_ptr Address of a live handle returned by CreatePeer.
 *
 * Neither peer_ptr nor *peer_ptr may be null; this function is not idempotent.
 * Stop concurrent API calls and frame producers first. Keep callback state alive
 * until this call completes, and do not call it synchronously from a callback.
 */
MINIRTC_API void DestroyPeer(PeerPtr** peer_ptr);

/**
 * @brief Read configuration and start asynchronous signaling/login.
 * @return -1 for an invalid peer; otherwise 0 in the current wrapper, which
 * does not propagate the internal initialization result.
 *
 * Call once per created peer, with required callbacks already configured.
 * Callbacks can start during this call. Observe OnSignalStatus to determine
 * login success; a zero return alone does not mean signaling is connected.
 */
MINIRTC_API int Init(PeerPtr* peer_ptr);

/**
 * @brief Request a remote session through the signaling service.
 * @param transmission_id Non-null, NUL-terminated target identity in the
 * service's format; CrossDesk commonly uses "remote-id@password".
 * @return 0 if sent or queued, -1 for an invalid peer or unavailable signaling.
 *
 * Register streams before joining. During signaling connection/reconnection,
 * one pending join is retained; a newer request replaces it. Wait for Connected
 * through OnConnectionStatus before sending media or application data.
 */
MINIRTC_API int JoinConnection(PeerPtr* peer_ptr, const char* transmission_id);

/**
 * @brief Request session leave or cancel a pending join.
 * @param transmission_id Non-null, NUL-terminated session identity understood
 * by the signaling service.
 * @return -1 for an invalid peer; otherwise 0. Internal leave errors are not
 * propagated, and zero does not confirm that the server processed the request.
 *
 * The internal leave path clears current peer sessions when it can proceed;
 * this is not a selective per-remote disconnect API. Signaling remains alive.
 */
MINIRTC_API int LeaveConnection(PeerPtr* peer_ptr, const char* transmission_id);

/**
 * @brief Register an outgoing video stream for subsequent session negotiation.
 * @param stream_id Non-null, non-empty, NUL-terminated unique stream name.
 * @return 0 on registration, -1 for an invalid peer.
 *
 * Register before connections are negotiated. Existing sessions are not
 * renegotiated, and duplicate names are not rejected. The name is copied.
 */
MINIRTC_API int AddVideoStream(PeerPtr* peer_ptr, const char* stream_id);

/// Register an outgoing audio stream. Naming, timing, ownership, and return
/// values follow AddVideoStream; send PCM using SendAudioFrame.
MINIRTC_API int AddAudioStream(PeerPtr* peer_ptr, const char* stream_id);

/**
 * @brief Register an application-data stream before session negotiation.
 * @param stream_id Non-null, non-empty, NUL-terminated unique name; copied.
 * @param reliable true for reliable transport, false for unreliable transport.
 * @return 0 on registration, -1 for an invalid peer.
 *
 * Use SendReliableDataFrame for a reliable stream and SendDataFrame otherwise.
 * Re-registering a name leaves its existing reliability setting unchanged.
 * Existing sessions are not renegotiated.
 */
MINIRTC_API int AddDataStream(PeerPtr* peer_ptr, const char* stream_id,
                              bool reliable);

/**
 * @brief Submit a captured NV12 video frame to all current peer sessions.
 * @param video_frame Valid CPU or native frame as described by MiniRtcVideoFrame.
 * @param stream_id Non-null, NUL-terminated name registered by AddVideoStream.
 * @return -1 for an invalid peer or frame; otherwise 0, even if no peer exists
 * or the internal transport drops/rejects the frame.
 *
 * Keep CPU bytes and the descriptor valid until return. CPU input is copied;
 * native storage may be retained for asynchronous encoding. A successful return
 * is not an encoding or delivery acknowledgment.
 */
MINIRTC_API int SendVideoFrame(PeerPtr* peer_ptr,
                               const MiniRtcVideoFrame* video_frame,
                               const char* stream_id);

/**
 * @brief Request a key frame from local outgoing video encoders.
 * @param stream_id NUL-terminated video stream name; null/empty selects all.
 * @return 0 if at least one peer connection accepts the request, otherwise -1
 * (including an invalid peer or no current connections).
 *
 * Continue supplying captured frames. Acceptance does not confirm stream
 * existence or key-frame delivery, and does not request a remote encoder.
 */
MINIRTC_API int RequestVideoKeyFrame(PeerPtr* peer_ptr,
                                     const char* stream_id);

/** Apply live outgoing video settings to one connected peer without
 * reconnecting. Returns 0 when applied, -1 for invalid settings, an unknown
 * peer, an unsupported transport, or encoder initialization failure. Continue
 * supplying frames (up to 60 fps). Waits for the encode queue; never call from
 * an encoder or encoded-frame callback. Safe to call from the received-data
 * callback.
 */
MINIRTC_API int UpdateVideoSettings(PeerPtr* peer_ptr, const char* remote_id,
                                    size_t remote_id_size, VideoQuality quality,
                                    int frame_rate,
                                    VideoDegradationPreference preference);

/// Request local key frames for all outgoing video streams on current peers.
/// Return values and asynchronous behavior follow RequestVideoKeyFrame.
MINIRTC_API int RequestAllVideoKeyFrames(PeerPtr* peer_ptr);

/**
 * @brief Submit 10 ms of PCM audio to all current peer sessions.
 * @param audio_frame 48 kHz mono signed 16-bit PCM; exactly 960 bytes.
 * @param stream_id Non-null, NUL-terminated name registered by AddAudioStream.
 * @return -1 for an invalid peer or null/empty audio input; otherwise the
 * internal dispatch result. Zero does not confirm encoding or delivery and
 * can also be returned when there are no peer sessions.
 *
 * Keep the frame and its borrowed PCM buffer valid until return. The wrapper
 * only checks non-empty input; the caller must supply the required PCM format.
 */
MINIRTC_API int SendAudioFrame(PeerPtr* peer_ptr,
                               const MiniRtcAudioFrame* audio_frame,
                               const char* stream_id);

/**
 * @brief Submit application bytes to all peers on an unreliable data stream.
 * @param data Non-null buffer, valid until return; embedded zero bytes are valid.
 * @param size Number of payload bytes; must be greater than zero.
 * @param stream_id Non-null, NUL-terminated name registered with reliable=false.
 * @return -1 for an invalid peer or null/empty data; otherwise 0. Internal send
 * failures are not propagated, and zero does not confirm remote delivery.
 */
MINIRTC_API int SendDataFrame(PeerPtr* peer_ptr, const char* data, size_t size,
                              const char* stream_id);

/// Submit bytes to all peers on a stream registered with reliable=true.
/// Buffer lifetime, validation, and return values follow SendDataFrame.
/// Reliability is a transport property; zero is not a delivery acknowledgment.
MINIRTC_API int SendReliableDataFrame(PeerPtr* peer_ptr, const char* data,
                                      size_t size, const char* stream_id);

/**
 * @brief Submit video to one current peer using SendVideoFrame's frame contract.
 * @param remote_peer_id Non-null buffer holding the exact connection ID,
 * without an appended login/password suffix.
 * @param remote_peer_id_size ID byte count; a trailing NUL is not required and
 * must not be included in the count. Keep the ID buffer valid until return.
 * @return -1 for an invalid peer or frame; otherwise 0, including when the
 * remote ID is absent. Internal lookup/send failures are not propagated.
 *
 * stream_id follows SendVideoFrame. Use the ID reported with Connected by
 * OnConnectionStatus.
 */
MINIRTC_API int SendVideoFrameToPeer(PeerPtr* peer_ptr,
                                     const MiniRtcVideoFrame* video_frame,
                                     const char* stream_id,
                                     const char* remote_peer_id,
                                     size_t remote_peer_id_size);

/**
 * @brief Submit PCM to one current peer using SendAudioFrame's input contract.
 *
 * Remote ID buffer/length rules follow SendVideoFrameToPeer.
 * @return -1 for an invalid peer, null/empty audio input, or absent remote ID;
 * otherwise the internal dispatch result. Zero is not a delivery acknowledgment
 * and does not confirm that the encoder accepted the PCM format.
 */
MINIRTC_API int SendAudioFrameToPeer(PeerPtr* peer_ptr,
                                     const MiniRtcAudioFrame* audio_frame,
                                     const char* stream_id,
                                     const char* remote_peer_id,
                                     size_t remote_peer_id_size);

/**
 * @brief Submit application bytes to one peer on an unreliable data stream.
 *
 * Data/stream rules follow SendDataFrame; remote ID rules follow
 * SendVideoFrameToPeer. This wrapper does not validate the data pointer or size:
 * the caller must supply a non-null buffer and a positive byte count.
 * @return -1 for an invalid peer; otherwise 0, even when the remote ID is absent
 * or internal sending fails. This is not a delivery acknowledgment.
 */
MINIRTC_API int SendDataFrameToPeer(PeerPtr* peer_ptr, const char* data,
                                    size_t size, const char* stream_id,
                                    const char* remote_peer_id,
                                    size_t remote_peer_id_size);

/// Submit application bytes to one peer on a stream registered as reliable.
/// Buffer/ID lifetime, caller validation duties, and return values follow
/// SendDataFrameToPeer; zero does not acknowledge delivery.
MINIRTC_API int SendReliableDataFrameToPeer(PeerPtr* peer_ptr, const char* data,
                                            size_t size, const char* stream_id,
                                            const char* remote_peer_id,
                                            size_t remote_peer_id_size);

/**
 * @brief Send an application-supplied payload over the open signaling socket.
 * @param message Non-null buffer containing a service-compatible payload.
 * @param size Positive byte count; the buffer need not be NUL-terminated.
 * @return 0 after submitting to an open socket; -1 for an invalid peer,
 * null/empty payload, or unavailable socket.
 *
 * Keep the buffer valid until return. The API does not validate JSON, wait for a
 * server response, or carry media. Handle applicable replies in OnSignalMessage.
 */
MINIRTC_API int SendSignalMessage(PeerPtr* peer_ptr, const char* message,
                                  size_t size);

/**
 * @brief Read the peer's monotonic clock in microseconds for frame timestamps.
 * @return Current clock value, 0 before clock initialization, or -1 for an
 * invalid peer. Call after Init; this is not a Unix/wall-clock timestamp.
 */
MINIRTC_API int64_t GetSystemTimeMicros(PeerPtr* peer_ptr);

#ifdef __cplusplus
}
#endif

#endif
