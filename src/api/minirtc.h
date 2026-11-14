/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-02
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _MINIRTC_H_
#define _MINIRTC_H_

/**
 * @file minirtc.h
 * @brief Public C++17 API for signaling, media, and application data transport.
 *
 * Functions have C linkage, but the types in this header require C++.
 * Start with MiniRtcParams{} and configure endpoints and callbacks. Call
 * MiniRtcCreatePeer, register streams, then MiniRtcInit. Wait for
 * MiniRtcSignalConnected before MiniRtcJoinConnection and for
 * MiniRtcConnectionConnected before sending media or data. Finish with
 * MiniRtcLeaveConnection and MiniRtcDestroyPeer.
 *
 * Types, callbacks and configuration precede exported functions. Functions are
 * grouped by lifecycle, signaling, streams, media, data, direct sessions and
 * utility operations.
 *
 * Callbacks may run on internal worker threads and may overlap. Keep them short,
 * synchronize application state, and dispatch UI work to the appropriate thread.
 * Do not let exceptions escape callbacks or synchronously destroy their peer.
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

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Connection types and routing policy
// -----------------------------------------------------------------------------

/// Opaque owned handle. Create with MiniRtcCreatePeer; release only with MiniRtcDestroyPeer.
typedef struct MiniRtcPeer MiniRtcPeerPtr;

/// Legacy payload categories; current send functions use typed frames/streams.
enum MiniRtcDataType { MiniRtcDataTypeVideo = 0, MiniRtcDataTypeAudio, MiniRtcDataTypeData };

/// Status of a remote session, independent of the WSS signaling connection.
enum MiniRtcConnectionStatus {
  /// Session negotiation or transport setup is starting.
  MiniRtcConnectionConnecting = 0,
  /// The transport reports that the session is usable.
  MiniRtcConnectionConnected,
  /// ICE candidates are being gathered.
  MiniRtcConnectionGathering,
  /// Connectivity was lost; the session may be retired.
  MiniRtcConnectionDisconnected,
  /// Negotiation or transport establishment failed.
  MiniRtcConnectionFailed,
  /// The session was closed or explicitly cleared.
  MiniRtcConnectionClosed,
  /// The signaling server rejected the target password.
  MiniRtcConnectionIncorrectPassword,
  /// The signaling server could not find the target ID.
  MiniRtcConnectionNoSuchTransmissionId,
  /// The target exists but is not available for joining.
  MiniRtcConnectionRemoteUnavailable
};

/// Signaling connection/login events; MiniRtcInit() does not wait for these events.
enum MiniRtcSignalStatus {
  /// Opening the signaling connection.
  MiniRtcSignalConnecting = 0,
  /// Signaling login completed successfully.
  MiniRtcSignalConnected,
  /// Connection or login failed.
  MiniRtcSignalFailed,
  /// Signaling connection closed.
  MiniRtcSignalClosed,
  /// Attempting to reconnect to the signaling server.
  MiniRtcSignalReconnecting,
  /// Signaling transport reported server closure.
  MiniRtcSignalServerClosed,
  /// TLS certificate verification failed.
  MiniRtcSignalTlsCertError
};

/// Observed transport path in a network report, not a requested TURN policy.
enum MiniRtcTraversalMode {
  MiniRtcTraversalP2P = 0,
  MiniRtcTraversalRelay,
  MiniRtcTraversalUnknown
};

/// ICE candidate policy; enabled relay modes still require valid TURN access.
enum MiniRtcTurnMode : uint8_t {
  /// Direct/STUN candidates only.
  MiniRtcTurnDisabled = 0,
  /// Direct candidates plus TURN/UDP and TURN/TCP fallback.
  MiniRtcTurnAutoUdpTcp,
  /// Relay-only candidates using TURN/UDP.
  MiniRtcTurnForceUdp,
  /// Relay-only candidates using TURN/TCP.
  MiniRtcTurnForceTcp,
};

/// Detailed join rejection category from signaling error_code or legacy reason.
enum class MiniRtcJoinFailureKind : uint8_t {
  PasswordRequired,
  AuthenticationFailed,
  NoSuchTransmission,
  RemoteUnavailable,
  Throttled,
  Busy,
  NotAuthenticated,
  Unknown,
};

typedef struct {
  MiniRtcJoinFailureKind kind;
  MiniRtcConnectionStatus status;
  /// Positive seconds for throttled/busy responses only.
  int retry_after;
} MiniRtcJoinFailure;

/// Parsed secure signaling endpoint. host preserves IPv6 brackets. target is
/// empty for the root path; an initial query is normalized to start with '/'.
/// port is zero when omitted. host + target fits MiniRtcParams::signal_server_ip.
typedef struct {
  char host[256];
  char target[256];
  int port;
} MiniRtcSignalServerAddress;

// -----------------------------------------------------------------------------
// Video settings and native frame types
// -----------------------------------------------------------------------------

/// Video quality preset used by adaptation/bitrate policy, not a fixed bitrate.
enum MiniRtcVideoQuality {
  MiniRtcVideoQualityLow = 0,
  MiniRtcVideoQualityMedium,
  MiniRtcVideoQualityHigh
};

/// Content hint supplied to video encoding and adaptation logic.
enum class MiniRtcVideoContentType : uint8_t {
  /// Camera/video content.
  RealtimeVideo = 0,
  /// Screen sharing and desktop content.
  ScreenContent = 1,
};

/// Preferred tradeoff when video must adapt to bandwidth or processing limits.
enum class MiniRtcVideoDegradationPreference : uint8_t {
  /// Prefer temporal smoothness over resolution.
  MaintainFrameRate = 0,
  /// Prefer spatial detail over frame rate.
  MaintainResolution = 1,
  /// Balance frame-rate and resolution adjustments.
  Balanced = 2,
};

/// Tag selecting exactly one MiniRtcNativeVideoFramePayload member.
enum MiniRtcNativeVideoFrameType : uint32_t {
  /// No native payload.
  MiniRtcNativeVideoFrameNone = 0,
  /// Host-accessible NV12 planes.
  MiniRtcNativeVideoFrameCpuNv12 = 1,
  /// CUDA device-memory NV12 planes.
  MiniRtcNativeVideoFrameCudaNv12 = 2,
  /// Apple CVPixelBufferRef payload.
  MiniRtcNativeVideoFrameCVPixelBuffer = 3,
  /// Output-only Surface buffer.
  MiniRtcNativeVideoFrameAndroidMediaCodec = 4,
};

/// NV12 planes in CPU memory. Strides are bytes per row, including padding.
typedef struct {
  /// Luma plane, height rows.
  const uint8_t* y_plane;
  /// Interleaved UV plane, height / 2 rows.
  const uint8_t* uv_plane;
  /// At least the frame width in bytes.
  uint32_t y_stride;
  /// At least the frame width in bytes.
  uint32_t uv_stride;
} MiniRtcCpuNv12Frame;

/// NV12 in CUDA memory; device addresses must not be dereferenced on the CPU.
typedef struct {
  /// CUdeviceptr-compatible luma address.
  uint64_t y_device_pointer;
  /// CUdeviceptr-compatible interleaved UV address.
  uint64_t uv_device_pointer;
  /// Device luma pitch in bytes, at least width.
  uint32_t y_stride;
  /// Device chroma pitch in bytes, at least width.
  uint32_t uv_stride;
  /// Owning CUcontext; borrowed with the frame.
  void* context;
} MiniRtcCudaNv12Frame;

/// Output-only MediaCodec buffer. render(owner, ANativeWindow*) consumes the
/// buffer once; null detaches its codec from the previous window. Return 0 on
/// success, negative on failure. release(owner) discards unrendered buffers.
/// A codec reset invalidates retained tokens; rendering such a token fails.
/// copy_to_nv12 is null for this type; it cannot be used as encoder input.
typedef struct {
  int (*render)(void* owner, void* native_window);
} MiniRtcAndroidMediaCodecFrame;

/// Access only the member named by MiniRtcNativeVideoFrame::type.
typedef union {
  MiniRtcCpuNv12Frame cpu_nv12;
  MiniRtcAndroidMediaCodecFrame android_media_codec;
  MiniRtcCudaNv12Frame cuda_nv12;
  /// NV12 CVPixelBufferRef; use Apple APIs for plane access.
  void* cv_pixel_buffer;
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
  /// Set to sizeof(MiniRtcNativeVideoFrame).
  uint32_t struct_size;
  /// Active payload tag.
  MiniRtcNativeVideoFrameType type;
  /// Positive, even pixel width.
  uint32_t width;
  /// Positive, even pixel height.
  uint32_t height;
  MiniRtcNativeVideoFramePayload payload;
  /// Non-null context owning the payload's storage/resources.
  void* owner;
  /// Acquire one reference to the owner.
  void (*retain)(void* owner);
  /// Release one reference to the owner.
  void (*release)(void* owner);
  /// Copy to packed CPU NV12 (Y then UV, no row padding). Destination must hold
  /// at least width * height * 3 / 2 bytes. Return 0 on success, negative on error.
  int (*copy_to_nv12)(void* owner, uint8_t* destination, size_t destination_size);
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
  /// Pixel width; must match a supplied native descriptor.
  uint32_t width;
  /// Pixel height; must match a supplied native descriptor.
  uint32_t height;
  /// Send: capture time from MiniRtcGetSystemTimeMicros(); zero selects send-time
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
  /// PCM samples, not already encoded Opus packets.
  const char* data;
  /// Byte count; current encoders require 960 bytes per frame.
  size_t size;
  /// Monotonic microseconds; zero uses fallback.
  uint64_t captured_timestamp;
} MiniRtcAudioFrame;

// -----------------------------------------------------------------------------
// Network statistics
// -----------------------------------------------------------------------------

/// Receive statistics from the reporting transport; counters are cumulative.
typedef struct {
  /// Bits per second over the latest reporting interval.
  uint32_t bitrate;
  /// RTP packets counted since statistics creation.
  uint32_t rtp_packet_count;
  /// Per-media loss fraction [0, 1], not a percentage.
  float loss_rate;
} MiniRtcInboundStats;

/// Send statistics from the reporting transport; not delivery acknowledgments.
typedef struct {
  /// Bits per second over the latest reporting interval.
  uint32_t bitrate;
  /// RTP packets counted since statistics creation.
  uint32_t rtp_packet_count;
} MiniRtcOutboundStats;

/**
 * @brief Per-media and aggregate transport statistics.
 *
 * Native transport reports approximately once per second. Totals sum video,
 * audio, and data counters/rates. In the current implementation,
 * total_inbound_stats.loss_rate also sums the three loss fractions: it is not
 * a weighted aggregate and may exceed 1. Availability depends on the backend;
 * an initial login report contains zero counters, unavailable RTT and MiniRtcTraversalUnknown.
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
  /// -1 without a valid sample; zero RTT remains valid.
  double rtt_ms = -1;
  // Actual local video backends, including software fallback. Empty until
  // available; distinct backends across streams are joined with " / ".
  char video_encoder[128] = {};
  char video_decoder[128] = {};
} MiniRtcNetTrafficStats;

// -----------------------------------------------------------------------------
// Logging types
// -----------------------------------------------------------------------------

/// Process-wide logging threshold. Build-time filtering can remove trace/debug.
enum MiniRtcLogLevel : int32_t {
  MiniRtcLogTrace = 0,
  MiniRtcLogDebug,
  MiniRtcLogInfo,
  MiniRtcLogWarn,
  MiniRtcLogError,
  MiniRtcLogCritical,
  MiniRtcLogOff,
};

/// Borrowed diagnostic record, valid for the duration of MiniRtcOnLog.
typedef struct {
  MiniRtcLogLevel level;
  /// Microseconds since the Unix epoch.
  int64_t timestamp_us;
  /// NUL-terminated source path, or nullptr.
  const char* source_file;
  /// Zero when the source location is unavailable.
  int source_line;
  /// Read message_size bytes, including any embedded NULs.
  const char* message;
  size_t message_size;
} MiniRtcLogRecord;

typedef void (*MiniRtcOnLog)(const MiniRtcLogRecord* record, void* user_data);

// -----------------------------------------------------------------------------
// Peer callbacks
// -----------------------------------------------------------------------------

/**
 * @brief Receive decoded PCM audio or application data for a named source.
 * @param data Borrowed bytes; may contain embedded zeros.
 * @param size Byte count; not a string terminator.
 * @param remote_peer_id Sender identifier, read using remote_peer_id_size.
 * @param remote_peer_id_size Sender ID byte count, excluding a trailing NUL.
 * @param source_id Source/stream label, read using source_id_size.
 * @param source_id_size Source label byte count, excluding a trailing NUL.
 * @param user_data The application context supplied in MiniRtcParams.
 *
 * Buffer type depends on which MiniRtcParams callback holds this function. Copy bytes
 * and identifiers before dispatching them to another thread or storing them.
 */
typedef void (*MiniRtcOnReceiveBuffer)(const char* data, size_t size, const char* remote_peer_id,
    const size_t remote_peer_id_size, const char* source_id, const size_t source_id_size,
    void* user_data);

/// Receive a borrowed decoded frame. Inspect native_frame before assuming data
/// is non-null; copy CPU pixels or copy/retain the native descriptor for later use.
/// Identifiers and user_data follow the MiniRtcOnReceiveBuffer contract.
typedef void (*MiniRtcOnReceiveVideoFrame)(const MiniRtcVideoFrame* video_frame,
    const char* remote_peer_id, const size_t remote_peer_id_size, const char* source_id,
    const size_t source_id_size, void* user_data);

/// Borrowed complete Annex B H.264 access unit before decoding. Return nonzero
/// to request a remote keyframe (callers must rate-limit requests). Other codecs
/// do not invoke this callback. Copy bytes before returning; never block here.
typedef int (*MiniRtcOnReceiveEncodedVideo)(const uint8_t* data, size_t size, int64_t timestamp_us,
    const char* remote_peer_id, size_t peer_size, const char* source_id, size_t source_size,
    void* user_data);

/// Report signaling/login state and the current local peer ID. The server may
/// assign or change that ID at login. This callback must be set before MiniRtcInit.
typedef void (*MiniRtcOnSignalStatus)(
    MiniRtcSignalStatus status, const char* peer_id, const size_t peer_id_size, void* user_data);

/// Receive a borrowed signaling payload not consumed by MiniRTC's internal
/// handler, plus failed user_join_transmission replies (including error_code
/// and retry_after) before MiniRtcOnConnectionStatus. Optional; not a raw observer.
typedef void (*MiniRtcOnSignalMessage)(const char* message, size_t size, void* user_data);

/// Stage supplied to the application signaling adapter.
enum class MiniRtcSignalExchangeStage { PrepareLogin, IncomingMessage };

/**
 * Optional application protocol adapter, called on the signaling thread.
 * PrepareLogin receives the default login request on every WebSocket open.
 * IncomingMessage runs before internal message handling, including login replies.
 * Return 0 to keep normal processing; a positive byte count sends the JSON object
 * written into response (replacing the login request or consuming the incoming
 * message); return -1 to fail signaling authentication without a fallback.
 * Never exceed response_capacity. Buffers are borrowed for this call only.
 * With an adapter, the application owns authentication/reconnect credentials;
 * MiniRTC does not apply its built-in reconnect-token/password fallback.
 * The adapter owns protocol semantics, keys and persistence; MiniRTC transports
 * opaque JSON and continues to handle standard successful/failed login replies.
 */
typedef int (*MiniRtcOnSignalExchange)(MiniRtcSignalExchangeStage stage, const char* message,
    size_t size, char* response, size_t response_capacity, void* user_data);

/// Report state for the named remote peer. Supply a valid callback before MiniRtcInit;
/// several signaling paths call it directly, including failed join responses.
typedef void (*MiniRtcOnConnectionStatus)(MiniRtcConnectionStatus status,
    const char* remote_peer_id, const size_t remote_peer_id_size, void* user_data);

/**
 * @brief Report the local/remote peer IDs, observed path, and borrowed statistics.
 *
 * Required before MiniRtcInit. Read identifiers by their supplied lengths. The initial
 * login report uses MiniRtcTraversalUnknown and zero statistics; its peer_id may contain
 * the full ID@password login identity, so do not log that value indiscriminately.
 * Copy the statistics value before using it outside this callback.
 */
typedef void (*MiniRtcOnNetStatusReport)(const char* peer_id, const size_t peer_id_size,
    MiniRtcTraversalMode mode, const MiniRtcNetTrafficStats* stats, const char* remote_peer_id,
    const size_t remote_peer_id_size, void* user_data);

// -----------------------------------------------------------------------------
// Peer configuration
// -----------------------------------------------------------------------------

/**
 * @brief Peer configuration. Start with MiniRtcParams{} and set required values.
 *
 * Zero initialization is not an application preset: it disables TURN/SRTP and
 * leaves endpoints unset. All character arrays must be NUL-terminated.
 * MiniRtcCreatePeer borrows pointers into these arrays; keep MiniRtcParams at a stable
 * address, and keep user_id alive, until MiniRtcInit has finished reading them.
 * Callbacks and user_data must remain valid until peer destruction completes.
 *
 * ICE servers and temporary credentials come only from signaling.
 * With use_cfg_file=true, the INI supplies signaling and media settings instead
 * of their direct fields below; missing INI values do not fall back to those
 * fields. Callbacks, user_id, user_data, and log_path still come from this
 * structure.
 */
typedef struct {
  /// Select INI configuration instead of direct settings.
  bool use_cfg_file;
  /// INI path; relative paths use the process working dir.
  char cfg_path[256];

  /// Signaling host with optional path, or wss/https URL. WSS uses system trust.
  /// An explicit URL port overrides signal_server_port.
  char signal_server_ip[256];
  /// WSS port; no automatic default in direct mode.
  int signal_server_port;
  /// Logger directory; an empty value uses "logs".
  char log_path[256];
  /// Request available hardware H.264 codecs; build/device support still applies.
  bool hardware_acceleration;
  /// Prefer MiniRtcVideoFrame::native_frame instead of copying decoded pixels into
  /// MiniRtcVideoFrame::data. Decoders that cannot expose a descriptor ignore this
  /// option and continue to provide CPU data.
  bool native_video_output;
  /// Request AV1 video encoding; false selects H.264.
  bool av1_encoding;
  /// Candidate policy; zero means MiniRtcTurnDisabled.
  MiniRtcTurnMode turn_mode;
  /// Enable native media SRTP; browser WebRTC uses encryption.
  bool enable_srtp;
  /// Screen sharing or real-time video hint.
  MiniRtcVideoContentType video_content_type;

  /// Adaptation/quality preset.
  MiniRtcVideoQuality video_quality;
  /// 30 or 60 fps; other values normalize to 60.
  uint32_t video_frame_rate;
  /// Adaptation priority.
  MiniRtcVideoDegradationPreference video_degradation_preference;

  /// Legacy field; current video dispatch uses on_receive_video_frame instead.
  MiniRtcOnReceiveBuffer on_receive_video_buffer;
  /// Optional decoded PCM receiver.
  MiniRtcOnReceiveBuffer on_receive_audio_buffer;
  /// Optional application-data receiver.
  MiniRtcOnReceiveBuffer on_receive_data_buffer;

  /// Optional decoded-frame receiver.
  MiniRtcOnReceiveVideoFrame on_receive_video_frame;

  /// Required; use a no-op if unused.
  MiniRtcOnSignalStatus on_signal_status;
  /// Optional signaling/rejected-join receiver.
  MiniRtcOnSignalMessage on_signal_message;
  /// Required; use a no-op if unused.
  MiniRtcOnConnectionStatus on_connection_status;
  /// Required, including during login.
  MiniRtcOnNetStatusReport on_net_status_report;

  /// Local login identity, not the target ID. CrossDesk may use ID@password;
  /// assignment/registration rules belong to the signaling service. Borrowed.
  const char* user_id;
  /// Opaque caller-owned context forwarded to all callbacks.
  void* user_data;
  /// Optional application protocol adapter.
  MiniRtcOnSignalExchange on_signal_exchange;
  /// Optional native H.264 tap.
  MiniRtcOnReceiveEncodedVideo on_receive_encoded_video;

  /// Native P2P/direct-IP congestion-control starting estimate in bps.
  /// Zero or values outside [300000, 20000000] use the default 2500000.
  /// INI mode reads [bandwidth] direct_start_bitrate_bps instead.
  int direct_start_bitrate_bps;
  /// Native TURN congestion-control starting estimate in bps.
  /// Zero or values outside [300000, 20000000] use the default 1500000.
  /// INI mode reads [bandwidth] relay_start_bitrate_bps instead.
  int relay_start_bitrate_bps;
  /// Suppress native padding probes until the first local video packet is queued.
  /// False preserves receiver-side probing. True still permits sender probing,
  /// including ALR recovery during static video, after local video starts.
  /// Applies per connection to direct and relay paths; browser WebRTC is separate.
  /// INI mode reads [bandwidth] disable_receiver_padding_probing (default false).
  bool disable_receiver_padding_probing;
} MiniRtcParams;

// -----------------------------------------------------------------------------
// IP direct configuration
// -----------------------------------------------------------------------------

/// Error reported by an authenticated IP direct session.
typedef enum MiniRtcDirectError {
  MiniRtcDirectInvalidSignal = 1,
  MiniRtcDirectSignalSendFailed,
  MiniRtcDirectDtlsHandshakeFailed,
  MiniRtcDirectRequiredCryptoUnsupported,
  MiniRtcDirectSrtpInstallFailed,
  MiniRtcDirectNetworkPolicyFailed,
  MiniRtcDirectFingerprintMissing,
  MiniRtcDirectFingerprintMismatch,
  MiniRtcDirectSrtpKeyExportFailed
} MiniRtcDirectError;

/**
 * @brief Configuration for an authenticated IP direct session.
 *
 * Direct peers never log in, join a cloud session, or request STUN/TURN.
 * The caller authenticates its signaling channel before initialization.
 * send_signal receives native MiniRTC signaling JSON; copy/enqueue it before
 * returning. Callback contexts remain alive until
 * MiniRtcCloseDirectSession/MiniRtcDestroyPeer returns.
 */
typedef struct MiniRtcDirectOptions {
  /// Set to sizeof(MiniRtcDirectOptions).
  uint32_t struct_size;
  /// Must be 1.
  uint32_t version;
  const char* local_id;
  const char* remote_id;
  const char* session_id;
  /// Authenticated WSS local IPv4/IPv6, with a local zone for link-local.
  const char* local_address;
  /// Authenticated WSS peer IPv4/IPv6, with a local zone for link-local.
  const char* remote_address;
  bool offerer;
  /// Required. Copy/enqueue the borrowed payload and return 0 on success.
  int (*send_signal)(const char* message, size_t message_size, void* context);
  /// Required. Receives errors with the context supplied below.
  void (*on_error)(MiniRtcDirectError error, void* context);
  void* context;
} MiniRtcDirectOptions;

// -----------------------------------------------------------------------------
// Logging and platform setup
// -----------------------------------------------------------------------------

/**
 * @brief Configure process-wide logging, including before the first MiniRtcCreatePeer.
 *
 * A callback receives diagnostics on the asynchronous logging worker and owns
 * filtering/storage. It replaces the default console/rotating-file outputs.
 * A null callback selects default outputs; MiniRtcLogOff disables logging.
 * Default file output uses the directory selected by MiniRtcParams::log_path.
 *
 * Calls are serialized. Replacement waits for the previous callback to finish,
 * so its user_data may be released after this function returns successfully.
 * Queued records may be delivered to the newly selected callback. Keep callback
 * context alive until replacement completes. Callbacks must return promptly,
 * contain exceptions, and avoid MiniRTC calls or waiting for their callers.
 * Callback exceptions are contained by the library.
 *
 * @return 0 on success, -1 for an invalid level, -2 when called from a log
 * callback, or -3 when the output could not be configured. Configure before
 * MiniRtcCreatePeer to route startup diagnostics through the selected output.
 */
MINIRTC_API int MiniRtcConfigureLogging(
    MiniRtcLogLevel level, MiniRtcOnLog callback, void* user_data);

/// Supply Android's JavaVM* as an opaque pointer from JNI_OnLoad before creating
/// peers. The VM must outlive all peers/codecs. A null pointer clears the VM and
/// leaves software codecs available. Returns 0 on Android, -1 on other platforms.
MINIRTC_API int MiniRtcSetAndroidJavaVm(void* java_vm);

// -----------------------------------------------------------------------------
// Peer lifecycle and configuration
// -----------------------------------------------------------------------------

/**
 * @brief Allocate a peer and capture its configuration; does not connect.
 * @param params Zero-initialized configuration with required fields populated.
 * @return An owned handle, or nullptr if params is null.
 *
 * Initializes logging using log_path. Configuration is not validated here;
 * MiniRtcParams strings must remain valid until MiniRtcInit reads them. Release the handle
 * with MiniRtcDestroyPeer. Allocation failures are not converted to a null return.
 */
MINIRTC_API MiniRtcPeerPtr* MiniRtcCreatePeer(const MiniRtcParams* params);

/**
 * @brief Read configuration and start asynchronous signaling/login.
 * @return -1 for an invalid peer; otherwise 0 in the current wrapper, which
 * does not propagate the internal initialization result.
 *
 * Call once per created peer, with required callbacks already configured.
 * Callbacks can start during this call. Observe MiniRtcOnSignalStatus to determine
 * login success; a zero return alone does not mean signaling is connected.
 */
MINIRTC_API int MiniRtcInit(MiniRtcPeerPtr* peer_ptr);

/**
 * @brief Update codec and ICE defaults without reconnecting signaling.
 *
 * Call after MiniRtcInit. The settings are copied atomically for subsequent remote
 * connections; existing or already negotiating connections keep their snapshot.
 * Returns 0 on success, -1 for an invalid peer or TURN mode. As with other peer
 * APIs, the caller must prevent concurrent destruction of the handle.
 */
MINIRTC_API int MiniRtcUpdateConnectionSettings(MiniRtcPeerPtr* peer_ptr,
    bool hardware_acceleration, bool av1_encoding, MiniRtcTurnMode turn_mode);

/**
 * @brief Stop the peer, release its resources, and set the caller's handle null.
 * @param peer_ptr Address of a live handle returned by MiniRtcCreatePeer.
 *
 * Neither peer_ptr nor *peer_ptr may be null; this function is not idempotent.
 * Stop concurrent API calls and frame producers first. Keep callback state alive
 * until this call completes, and do not call it synchronously from a callback.
 */
MINIRTC_API void MiniRtcDestroyPeer(MiniRtcPeerPtr** peer_ptr);

// -----------------------------------------------------------------------------
// Signaling and remote sessions
// -----------------------------------------------------------------------------

/**
 * @brief Request a remote session through the signaling service.
 * @param transmission_id Non-null, NUL-terminated target identity in the
 * service's format; CrossDesk commonly uses "remote-id@password".
 * @return 0 if sent or queued, -1 for an invalid peer or unavailable signaling.
 *
 * Register streams before joining. During signaling connection/reconnection,
 * one pending join is retained; a newer request replaces it. Wait for MiniRtcConnectionConnected
 * through MiniRtcOnConnectionStatus before sending media or application data.
 */
MINIRTC_API int MiniRtcJoinConnection(MiniRtcPeerPtr* peer_ptr, const char* transmission_id);

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
MINIRTC_API int MiniRtcLeaveConnection(MiniRtcPeerPtr* peer_ptr, const char* transmission_id);

/**
 * @brief As a session host, disconnect one remote peer and notify it via WSS.
 * @param transmission_id Non-empty, NUL-terminated hosted session identity.
 * @param remote_user_id Non-empty, NUL-terminated guest identity to disconnect.
 * @return 0 if the notification was queued, -1 for invalid arguments, an absent
 * peer, or unavailable signaling. An existing local peer is closed even when
 * signaling is unavailable. Other peers and signaling remain active.
 *
 * Requires a signaling server supporting disconnect_peer. MiniRtcConnectionClosed is emitted
 * locally; transport cleanup runs on the ICE worker.
 */
MINIRTC_API int MiniRtcDisconnectPeerConnection(
    MiniRtcPeerPtr* peer_ptr, const char* transmission_id, const char* remote_user_id);

/**
 * @brief Send an application-supplied payload over the open signaling socket.
 * @param message Non-null buffer containing a service-compatible payload.
 * @param size Positive byte count; the buffer need not be NUL-terminated.
 * @return 0 after submitting to an open socket; -1 for an invalid peer,
 * null/empty payload, or unavailable socket.
 *
 * Keep the buffer valid until return. The API does not validate JSON, wait for a
 * server response, or carry media. Handle applicable replies in MiniRtcOnSignalMessage.
 */
MINIRTC_API int MiniRtcSendSignalMessage(
    MiniRtcPeerPtr* peer_ptr, const char* message, size_t size);

/**
 * @brief Retry signaling now, skipping the exponential reconnect backoff.
 * @return 0 when signaling is authenticated or a retry was requested; -1 for
 * an invalid peer or unavailable transport. Call after MiniRtcInit. Login is replayed
 * by the existing reconnect path, so callers do not re-authenticate.
 */
MINIRTC_API int MiniRtcReconnectSignal(MiniRtcPeerPtr* peer_ptr);

// -----------------------------------------------------------------------------
// Stream registration
// -----------------------------------------------------------------------------

/**
 * @brief Register an outgoing video stream for subsequent session negotiation.
 * @param stream_id Non-null, non-empty, NUL-terminated unique stream name.
 * @return 0 on registration, -1 for an invalid peer.
 *
 * Register before connections are negotiated. Existing sessions are not
 * renegotiated, and duplicate names are not rejected. The name is copied.
 */
MINIRTC_API int MiniRtcAddVideoStream(MiniRtcPeerPtr* peer_ptr, const char* stream_id);

/// Register an outgoing audio stream. Naming, timing, ownership, and return
/// values follow MiniRtcAddVideoStream; send PCM using MiniRtcSendAudioFrame.
MINIRTC_API int MiniRtcAddAudioStream(MiniRtcPeerPtr* peer_ptr, const char* stream_id);

/**
 * @brief Register an application-data stream before session negotiation.
 * @param stream_id Non-null, non-empty, NUL-terminated unique name; copied.
 * @param reliable true for reliable transport, false for unreliable transport.
 * @return 0 on registration, -1 for an invalid peer.
 *
 * Use MiniRtcSendReliableDataFrame for a reliable stream and MiniRtcSendDataFrame otherwise.
 * Re-registering a name leaves its existing reliability setting unchanged.
 * Existing sessions are not renegotiated. Browser sessions map ordinary streams
 * to separate DataChannels whose labels and callback source IDs are stream_id.
 * Reliable channels are ordered; unreliable channels are unordered/no retries.
 * Legacy control/input aliases retain the shared control channel. Applications
 * own payload serialization, protocol versions, and processing acknowledgements.
 */
MINIRTC_API int MiniRtcAddDataStream(
    MiniRtcPeerPtr* peer_ptr, const char* stream_id, bool reliable);

// -----------------------------------------------------------------------------
// Video transmission and control
// -----------------------------------------------------------------------------

/**
 * @brief Submit a captured NV12 video frame to all current peer sessions.
 * @param video_frame Valid CPU or native frame as described by MiniRtcVideoFrame.
 * @param stream_id Non-null, NUL-terminated name registered by MiniRtcAddVideoStream.
 * @return -1 for an invalid peer or frame; otherwise 0, even if no peer exists
 * or the internal transport drops/rejects the frame.
 *
 * Keep CPU bytes and the descriptor valid until return. CPU input is copied;
 * native storage may be retained for asynchronous encoding. A successful return
 * is not an encoding or delivery acknowledgment.
 */
MINIRTC_API int MiniRtcSendVideoFrame(
    MiniRtcPeerPtr* peer_ptr, const MiniRtcVideoFrame* video_frame, const char* stream_id);

/**
 * @brief Submit video to one current peer using MiniRtcSendVideoFrame's frame contract.
 * @param remote_peer_id Non-null buffer holding the exact connection ID,
 * without an appended login/password suffix.
 * @param remote_peer_id_size ID byte count; a trailing NUL is not required and
 * must not be included in the count. Keep the ID buffer valid until return.
 * @return -1 for an invalid peer or frame; otherwise 0, including when the
 * remote ID is absent. Internal lookup/send failures are not propagated.
 *
 * stream_id follows MiniRtcSendVideoFrame. Use the ID reported with MiniRtcConnectionConnected by
 * MiniRtcOnConnectionStatus.
 */
MINIRTC_API int MiniRtcSendVideoFrameToPeer(MiniRtcPeerPtr* peer_ptr,
    const MiniRtcVideoFrame* video_frame, const char* stream_id, const char* remote_peer_id,
    size_t remote_peer_id_size);

/** Apply live outgoing video settings to one connected peer without
 * reconnecting. Returns 0 when applied, -1 for invalid settings, an unknown
 * peer, an unsupported transport, or encoder initialization failure. Continue
 * supplying frames (up to 60 fps). Waits for the encode queue; never call from
 * an encoder or encoded-frame callback. Safe to call from the received-data
 * callback.
 */
MINIRTC_API int MiniRtcUpdateVideoSettings(MiniRtcPeerPtr* peer_ptr, const char* remote_id,
    size_t remote_id_size, MiniRtcVideoQuality quality, int frame_rate,
    MiniRtcVideoDegradationPreference preference);

/** Set the combined video bandwidth budget for one peer, including FEC/RTX.
 * 0 removes the limit; otherwise accepts [1, 100000000] bits per second.
 * Congestion control may allocate less. This is a sustained video target,
 * not a packet shaper; audio and data are independent. Returns 0 when queued,
 * -1 for invalid input, an unknown peer or an unsupported transport.
 */
MINIRTC_API int MiniRtcUpdateVideoBitrateLimit(
    MiniRtcPeerPtr* peer_ptr, const char* remote_id, size_t remote_id_size, int max_bitrate_bps);

/** Bound outgoing video for one peer to a physical-pixel viewport. Preserves
 * aspect ratio and never upscales; quality/network adaptation may send less.
 * width/height must both be 0 (restore source size), or both in [2, 16384].
 * Applied independently to each display. Requests fresh keyframes for this
 * peer, including when the bound is unchanged. Does not change the desktop mode.
 * Returns 0 on success, -1 for invalid input, unknown peer or unsupported
 * transport. Safe from the received-data callback.
 */
MINIRTC_API int MiniRtcUpdateVideoResolution(
    MiniRtcPeerPtr* peer_ptr, const char* remote_id, size_t remote_id_size, int width, int height);

/** Pause/resume all outgoing video channels for one peer. New frames and stale
 * encoder output are discarded while paused; queued video, RTX/FEC, probing
 * and padding are stopped on the pacer queue. Resume requests fresh keyframes.
 * Keeps the connection, capture source, audio/data and video settings intact.
 * Returns 0 when accepted (pacer cleanup is asynchronous), -1 for an invalid
 * peer/ID or unsupported transport. Idempotent; safe from received-data callbacks.
 * Packets already handed to the network cannot be recalled.
 */
MINIRTC_API int MiniRtcSetVideoSendPaused(
    MiniRtcPeerPtr* peer_ptr, const char* remote_id, size_t remote_id_size, bool paused);

/**
 * @brief Request a key frame from local outgoing video encoders.
 * @param stream_id NUL-terminated video stream name; null/empty selects all.
 * @return 0 if at least one peer connection accepts the request, otherwise -1
 * (including an invalid peer or no current connections).
 *
 * Continue supplying captured frames. Acceptance does not confirm stream
 * existence or key-frame delivery, and does not request a remote encoder.
 */
MINIRTC_API int MiniRtcRequestVideoKeyFrame(MiniRtcPeerPtr* peer_ptr, const char* stream_id);

/// Request local key frames for all outgoing video streams on current peers.
/// Return values and asynchronous behavior follow MiniRtcRequestVideoKeyFrame.
MINIRTC_API int MiniRtcRequestAllVideoKeyFrames(MiniRtcPeerPtr* peer_ptr);

// -----------------------------------------------------------------------------
// Audio transmission and control
// -----------------------------------------------------------------------------

/**
 * @brief Submit 10 ms of PCM audio to all current peer sessions.
 * @param audio_frame 48 kHz mono signed 16-bit PCM; exactly 960 bytes.
 * @param stream_id Non-null, NUL-terminated name registered by MiniRtcAddAudioStream.
 * @return -1 for an invalid peer or null/empty audio input; otherwise the
 * internal dispatch result. Zero does not confirm encoding or delivery and
 * can also be returned when there are no peer sessions.
 *
 * Keep the frame and its borrowed PCM buffer valid until return. The wrapper
 * only checks non-empty input; the caller must supply the required PCM format.
 */
MINIRTC_API int MiniRtcSendAudioFrame(
    MiniRtcPeerPtr* peer_ptr, const MiniRtcAudioFrame* audio_frame, const char* stream_id);

/**
 * @brief Submit PCM to one current peer using MiniRtcSendAudioFrame's input contract.
 *
 * Remote ID buffer/length rules follow MiniRtcSendVideoFrameToPeer.
 * @return -1 for an invalid peer, null/empty audio input, or absent remote ID;
 * otherwise the internal dispatch result. Zero is not a delivery acknowledgment
 * and does not confirm that the encoder accepted the PCM format.
 */
MINIRTC_API int MiniRtcSendAudioFrameToPeer(MiniRtcPeerPtr* peer_ptr,
    const MiniRtcAudioFrame* audio_frame, const char* stream_id, const char* remote_peer_id,
    size_t remote_peer_id_size);

/** Pause/resume all outgoing audio channels for one peer. Paused PCM input and
 * queued audio packets are discarded; resuming sends only fresh audio. Keeps
 * capture, the connection and video/data intact. Returns 0 when applied, -1 for
 * an invalid peer/ID or unsupported transport. Idempotent; safe from received-data
 * callbacks. Packets already handed to the network cannot be recalled.
 */
MINIRTC_API int MiniRtcSetAudioSendPaused(
    MiniRtcPeerPtr* peer_ptr, const char* remote_id, size_t remote_id_size, bool paused);

// -----------------------------------------------------------------------------
// Application data transmission
// -----------------------------------------------------------------------------

/**
 * @brief Submit application bytes to all peers on an unreliable data stream.
 * @param data Non-null buffer, valid until return; embedded zero bytes are valid.
 * @param size Number of payload bytes; must be greater than zero.
 * @param stream_id Non-null, NUL-terminated name registered with reliable=false.
 * @return -1 for an invalid peer or null/empty data; otherwise 0. Internal send
 * failures are not propagated, and zero does not confirm remote delivery.
 */
MINIRTC_API int MiniRtcSendDataFrame(
    MiniRtcPeerPtr* peer_ptr, const char* data, size_t size, const char* stream_id);

/**
 * @brief Submit application bytes to one peer on an unreliable data stream.
 *
 * Data/stream rules follow MiniRtcSendDataFrame; remote ID rules follow
 * MiniRtcSendVideoFrameToPeer. This wrapper does not validate the data pointer or size:
 * the caller must supply a non-null buffer and a positive byte count.
 * @return -1 for an invalid peer; otherwise 0, even when the remote ID is absent
 * or internal sending fails. This is not a delivery acknowledgment.
 */
MINIRTC_API int MiniRtcSendDataFrameToPeer(MiniRtcPeerPtr* peer_ptr, const char* data, size_t size,
    const char* stream_id, const char* remote_peer_id, size_t remote_peer_id_size);

/// Submit bytes to all peers on a stream registered with reliable=true.
/// Buffer lifetime, validation, and return values follow MiniRtcSendDataFrame.
/// Reliability is a transport property; zero is not a delivery acknowledgment.
MINIRTC_API int MiniRtcSendReliableDataFrame(
    MiniRtcPeerPtr* peer_ptr, const char* data, size_t size, const char* stream_id);

/// Submit application bytes to one peer on a stream registered as reliable.
/// Buffer/ID lifetime and caller validation duties follow MiniRtcSendDataFrameToPeer.
/// Returns -1 for an invalid peer or absent remote ID, otherwise the transport
/// send result. Zero means accepted for sending, not acknowledged delivery.
MINIRTC_API int MiniRtcSendReliableDataFrameToPeer(MiniRtcPeerPtr* peer_ptr, const char* data,
    size_t size, const char* stream_id, const char* remote_peer_id, size_t remote_peer_id_size);

// -----------------------------------------------------------------------------
// IP direct sessions
// -----------------------------------------------------------------------------

/// Initialize an authenticated direct peer using the supplied options.
MINIRTC_API int MiniRtcInitDirectPeer(MiniRtcPeerPtr* peer, const MiniRtcDirectOptions* options);

/// Start offer/answer negotiation for a configured direct peer.
MINIRTC_API int MiniRtcStartDirectSession(MiniRtcPeerPtr* peer, bool offerer);

/// Deliver a payload received over the authenticated direct signaling channel.
MINIRTC_API int MiniRtcReceiveDirectSignal(
    MiniRtcPeerPtr* peer, const char* message, size_t message_size);

/// Close the peer's direct session.
MINIRTC_API int MiniRtcCloseDirectSession(MiniRtcPeerPtr* peer);

// -----------------------------------------------------------------------------
// Address, rejection and timing helpers
// -----------------------------------------------------------------------------

/// Parse a host, host/path, or wss/https URL, trimming outer whitespace. On
/// success write a fully initialized result and return 0; invalid input or
/// output pointers return -1. Input is borrowed for the call and read by size.
MINIRTC_API int MiniRtcParseSignalServerAddress(
    const char* input, size_t input_size, MiniRtcSignalServerAddress* output);

/// Validate a numeric unicast direct-peer address and write canonical text with
/// a numeric IPv6 scope. Link-local IPv6 requires a local zone. output_capacity
/// includes the trailing NUL; 129 bytes accommodates every accepted address.
/// Return 0 on success, -1 for invalid input or insufficient output capacity.
MINIRTC_API int MiniRtcNormalizeDirectAddress(
    const char* input, size_t input_size, char* output, size_t output_capacity);

/// Return 1 for a compatible pair of valid direct-peer addresses, 0 otherwise.
/// Families must match; IPv6 link-local scope and classification must match.
MINIRTC_API int MiniRtcIsDirectAddressPair(
    const char* local, size_t local_size, const char* remote, size_t remote_size);

/// Parse a borrowed JSON join rejection. Recognized error_code values take
/// precedence over legacy reason strings. Invalid/missing data returns an
/// Unknown/MiniRtcConnectionFailed result with zero retry_after. Exceptions are contained.
MINIRTC_API MiniRtcJoinFailure MiniRtcParseJoinFailure(const char* message, size_t message_size);

/**
 * @brief Read the peer's monotonic clock in microseconds for frame timestamps.
 * @return Current clock value, 0 before clock initialization, or -1 for an
 * invalid peer. Call after MiniRtcInit; this is not a Unix/wall-clock timestamp.
 */
MINIRTC_API int64_t MiniRtcGetSystemTimeMicros(MiniRtcPeerPtr* peer_ptr);

#ifdef __cplusplus
}
#endif

#endif
