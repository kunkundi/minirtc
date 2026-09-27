# MiniRTC

A lightweight, cross-platform real-time media library powering [CrossDesk](https://github.com/kunkundi/crossdesk), with codecs, P2P / TURN connectivity, network adaptation, and data transport. Applications provide capture, playback, rendering, and business protocols; MiniRTC handles the transport pipeline.

[中文](README.md) · [Public API](src/api/minirtc.h) · [CrossDesk](https://github.com/kunkundi/crossdesk) · [Signaling server](https://github.com/kunkundi/crossdesk-server)

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20iOS-brightgreen)](#platforms)
[![License: LGPL v3](https://img.shields.io/badge/license-LGPL--3.0-blue)](LICENSE)
[![GitHub issues](https://img.shields.io/github/issues/kunkundi/minirtc)](https://github.com/kunkundi/minirtc/issues)

[Features](#features) · [Platforms and codecs](#platforms) · [Build](#build) · [Integration](#integration) · [Media and memory](#frames) · [Connectivity](#network) · [Validation](#validation)

<a id="features"></a>

## Features

- **Media:** H.264 / AV1 video, Opus audio, multiple media streams, and APIs for sending to a specific peer.
- **Connectivity:** WSS signaling, ICE / STUN, TURN UDP / TCP, dynamic TURN credentials, and UPnP mapping on supported gateways.
- **Network adaptation:** SRTP, RTP / RTCP, NACK retransmission, bandwidth estimation, congestion control, and frame-rate / resolution degradation policies.
- **Data:** reliable and unreliable streams. Applications implement serialization and handling for mouse, keyboard, clipboard, file, and other messages.
- **Browser interoperability:** a libdatachannel path supports [CrossDesk Web Client](https://github.com/kunkundi/crossdesk-web-client). Matching signaling and media negotiation are required; this is not a drop-in client for arbitrary WebRTC services.

The build produces a **C++17 static library** with `extern "C"` function linkage. The public header uses C++ types and syntax and cannot be included directly as a pure C header.

<a id="platforms"></a>

## Platforms and codecs

| Platform | H.264 hardware path | H.264 software path | AV1 encode / decode | Optional native decoded output |
| --- | --- | --- | --- | --- |
| Windows x64 | NVIDIA NVENC / NVDEC; requires CUDA build | OpenH264 | SVT-AV1 / dav1d | CPU NV12 from software decoders; CUDA NV12 from NVDEC |
| Linux x86-64 | NVIDIA NVENC / NVDEC; requires CUDA build | OpenH264 | SVT-AV1 / dav1d | CPU NV12 from software decoders; CUDA NV12 from NVDEC |
| Linux arm64 | Current factories use software codecs | OpenH264 | SVT-AV1 / dav1d | CPU NV12 from software decoders |
| macOS | VideoToolbox | OpenH264 | SVT-AV1 / dav1d | `CVPixelBufferRef` from VideoToolbox |
| iOS | VideoToolbox | Not built | SVT-AV1 / dav1d | `CVPixelBufferRef` from VideoToolbox |

- Hardware use depends on build configuration, runtime `hardware_acceleration`, and device support. On desktop, the initialization factory attempts an OpenH264 fallback when hardware H.264 encoder initialization fails. iOS does not compile or link OpenH264 and requires `hardware_acceleration=true` for H.264; software H.264 requests or initialization failures return a null codec without a software fallback.
- The AV1 factories select **SVT-AV1 encoding / dav1d decoding**. libaom is excluded by default; use `xmake f --MINIRTC_ENABLE_AOM=true` to build its optional backends for development. This does not change the factories' selection. There is no VideoToolbox AV1 path on Apple platforms.
- Set `native_video_output=true` to request native frames. Apple software decoders still return CPU data; Windows / Linux software decoders can return CPU NV12 descriptors. Native output does not guarantee zero copies throughout every path.

See the [encoder factory](src/media/video/encode/video_encoder_factory.cpp) and [decoder factory](src/media/video/decode/video_decoder_factory.cpp) for selection logic.

<a id="build"></a>

## Build

### Tools and dependencies

Install Git, [Xmake](https://xmake.io/guide/quick-start.html#installation), and a C++17 toolchain: MSVC with the Windows SDK on Windows, GCC / Clang on Linux, or the Xcode toolchain on macOS. iOS builds require full Xcode and the iPhoneOS SDK.

[xmake.lua](xmake.lua) and the [thirdparty recipes](thirdparty/xmake.lua) resolve codecs, libnice, libdatachannel, SRTP, KCP, and the UPnP dependency stack. Recipes bring in build tools such as CMake, Meson, Ninja, and NASM. The first build needs access to dependency sources; build time depends on the package cache.

### Desktop platforms

```sh
git clone https://github.com/kunkundi/minirtc.git
cd minirtc
xmake f -m release --USE_CUDA=false -y
xmake b minirtc
```

Replace `release` with `debug` for a debug build. Use `-p` / `-a` to select a platform and architecture explicitly. Windows currently uses the static CRT (`MT`); keep the final application's runtime configuration compatible.

**CUDA:** this example targets Linux x86-64. Install the appropriate CUDA SDK and NVIDIA driver first, and replace the SDK path with your local path:

```sh
xmake f -p linux -a x86_64 -m release \
  --USE_CUDA=true --CUDA_DIR=/usr/local/cuda -y
xmake b minirtc
```

On Windows, use `--USE_CUDA=true` with `CUDA_PATH` or `--CUDA_DIR`. A CUDA build also requires `hardware_acceleration=true` in `Params` to request the hardware path. VideoToolbox on Apple platforms does not require CUDA.

### iOS

The current CrossDesk iOS integration targets **iOS 16.0+ on physical arm64 devices**. Run from the MiniRTC root in a macOS terminal:

```sh
xmake f -c -p iphoneos -a arm64 -m release \
  --target_minver=16.0 --USE_CUDA=false -y
xmake b minirtc
```

This builds an iOS static library, not an App or XCFramework, and does not establish simulator support. Reconfigure the target platform before returning to desktop builds. See CrossDesk's [iOS build script](https://github.com/kunkundi/crossdesk/blob/HEAD/apps/ios/scripts/build_minirtc_ios.sh) for dependency merging, linking, and SDK environment handling in an Xcode project.

### Options and artifacts

| Option | Default | Description |
| --- | --- | --- |
| `USE_CUDA` | `false` | Compile NVIDIA codec backends on supported platforms |
| `CUDA_DIR` | Empty; auto-detected | Explicit configuration takes priority, followed by `CUDA_PATH` / `CUDA_HOME` and common installation paths |
| `MINIRTC_INCLUDE_VIRTUAL_ICE_INTERFACES` | `false` | Allow ICE candidate gathering from VPN / TUN and other virtual interfaces |

```sh
xmake show -t minirtc
xmake install -o build/install minirtc
```

The first command shows the resolved target and dependencies; the second installs the library and public header under `build/install`. The archive is `libminirtc.a`, or `minirtc.lib` on Windows. Copying this archive alone is insufficient to link a complete application: matching dependency libraries, system libraries, and Apple frameworks are also needed. Use Xmake's resolved configuration as the dependency source of truth.

Useful commands: `xmake f --menu` shows options, `xmake b -vy minirtc` enables verbose build output, and `xmake b -r minirtc` rebuilds the target.

<a id="integration"></a>

## Integration

Use [src/api/minirtc.h](src/api/minirtc.h) as the API reference. The current lifecycle is:

1. Zero-initialize with `Params params{}`, then set endpoints, media options, and callbacks. `Params` has no application-level defaults; zero initialization does not enable TURN or SRTP.
2. Call `CreatePeer(&params)` and check the result. Register streams with `AddVideoStream`, `AddAudioStream`, and `AddDataStream` before negotiating a session.
3. Call `Init(peer)` to start asynchronous signaling. **A zero return value does not mean login succeeded**; wait for `SignalConnected` through `on_signal_status`.
4. The initiating peer calls `JoinConnection(peer, "remote-id@password")` to join a CrossDesk session. `user_id` is the local login identity, not the remote target ID. Identity registration and password rules are defined by the signaling service.
5. After `on_connection_status` reports `Connected`, send media or data using the registered stream names. API return values do not confirm remote delivery.
6. Call `LeaveConnection` to leave the session. Stop application capture and sending tasks before calling `if (peer) DestroyPeer(&peer);`. Destruction clears the pointer.

### Configuration example

This function builds parameters for direct configuration; it does not open a network connection. Replace the example IP. Implement the callbacks in your application and add the required media/data receive callbacks before calling `CreatePeer`.

```cpp
#include <cstdio>
#include "minirtc.h"

Params MakeParams(const char* login_identity,
                  OnSignalStatus on_signal,
                  OnConnectionStatus on_connection,
                  OnNetStatusReport on_stats,
                  void* context) {
  Params params{};
  params.use_cfg_file = false;
  std::snprintf(params.signal_server_ip, sizeof(params.signal_server_ip),
                "%s", "203.0.113.10");
  params.signal_server_port = 9099;
  params.hardware_acceleration = true;
  params.native_video_output = false;
  params.av1_encoding = false;
  params.turn_mode = TurnAutoUdpTcp;
  params.enable_srtp = true;
  params.video_content_type = VideoContentType::ScreenContent;
  params.video_quality = QualityHigh;
  params.video_frame_rate = 60;
  params.video_degradation_preference =
      VideoDegradationPreference::MaintainResolution;
  params.on_signal_status = on_signal;
  params.on_connection_status = on_connection;
  params.on_net_status_report = on_stats;
  params.user_id = login_identity;
  params.user_data = context;
  return params;
}
```

`signal_server_ip` takes a hostname or IP **without `wss://`, a port, or a path**; the library constructs the WSS URL. STUN / TURN host fields also omit URL prefixes. Ports `9099` / `3478` come from CrossDesk Server's Compose defaults, not defaults built into `Params`.

### Callbacks and threads

- **Provide valid `on_signal_status`, `on_connection_status`, and `on_net_status_report` callbacks.** Current paths invoke them directly; use matching no-op functions if you do not need their events.
- Receive video through `on_receive_video_frame`, audio through `on_receive_audio_buffer`, and application data through `on_receive_data_buffer`. The legacy `on_receive_video_buffer` field is not wired into the current video-frame dispatch path.
- `on_signal_message` receives messages not consumed by the internal signaling handler; it is not a tap for every raw signaling message.
- Callbacks can run on internal worker threads. Dispatch UI work to the application thread, copy or retain data as needed, and avoid destroying a peer synchronously inside its callback. Keep `user_data` valid until destruction completes, and keep the `user_id` string alive at least until `Init` has read it.
- Read identifiers and buffers using their supplied lengths, without relying on an extra `\0` terminator. Network-report pointers are also borrowed for the callback duration.

<a id="frames"></a>

## Media formats and memory

| Data | Current contract |
| --- | --- |
| CPU video input | `MiniRtcVideoFrame`: packed NV12, positive even dimensions, at least `width × height × 3 / 2` bytes |
| Native video | `MiniRtcNativeVideoFrame`: select the CPU NV12, CUDA NV12, or `CVPixelBuffer` payload according to `type` |
| Audio input | `MiniRtcAudioFrame`: 48 kHz, mono, 16-bit PCM, 10 ms per frame: 480 samples / 960 bytes |
| Timestamps | Capture timestamps use monotonic microseconds; use `GetSystemTimeMicros(peer)`, not Unix milliseconds |
| Unreliable data | Register with `AddDataStream(..., false)` and use `SendDataFrame` |
| Reliable data | Register with `AddDataStream(..., true)` and use `SendReliableDataFrame` |

Frame/data sending APIs have corresponding `*ToPeer` variants for a specific remote peer. `RequestVideoKeyFrame` / `RequestAllVideoKeyFrames` request keyframes from local sending encoders; they are not general remote-decoder recovery commands.

With native output, `MiniRtcVideoFrame::data` may be null even though `size` still contains the logical NV12 size. Check `native_frame` first. If packed CPU data is needed, use the descriptor's `copy_to_nv12` and check its result; do not dereference CUDA addresses as CPU pointers.

**Retaining native frames beyond a callback:** descriptor pointers are valid only during the API call or callback. Copy the descriptor and call `retain(owner)` before keeping it; call `release(owner)` after its last use. Saving the pointer or copying only `MiniRtcVideoFrame` does not retain the owner. Copy CPU buffers before asynchronous use as well. Input descriptors must set `struct_size`, dimensions, payload, owner, and callbacks correctly; see [native_video_frame.h](src/frame/native_video_frame.h) for validation.

`video_frame_rate` currently accepts 30 / 60; other values normalize to 60. Degradation preferences are `MaintainFrameRate`, `MaintainResolution`, and `Balanced`. The application still supplies captured frames at an appropriate cadence.

<a id="network"></a>

## Connectivity and configuration

### TURN, UPnP, and virtual interfaces

| `turn_mode` | Behavior |
| --- | --- |
| `TurnDisabled` | Direct / STUN candidates only |
| `TurnAutoUdpTcp` | Direct candidates plus TURN UDP / TCP fallback |
| `TurnForceUdp` | TURN UDP relay only |
| `TurnForceTcp` | TURN TCP relay only |

- With the current CrossDesk Server, login and negotiation messages carry temporary TURN credentials. MiniRTC validates `host`, `port`, `username`, `password`, and `expires_at` on receipt, then updates configuration for subsequent connections. Do not embed `COTURN_AUTH_SECRET` in the application.
- The example leaves static TURN endpoints and credentials empty and uses server-issued values. Relay connections require correctly configured Coturn, a reachable advertised address, and valid credentials. `TurnAutoUdpTcp` does not deploy a relay service.
- UPnP dependencies are always built. The native ICE path attempts gateway mapping outside forced-relay modes; success depends on network and gateway support.
- The matching libnice patches let supported native peers keep trying to upgrade a relayed connection to P2P. Forced-relay modes do not perform this upgrade; it is not an unconditional switch available for every peer.
- Several virtual-interface prefixes are excluded by default, including Docker, TUN / TAP, WireGuard, Tailscale, and ZeroTier. To gather candidates through those interfaces, reconfigure and build with `--MINIRTC_INCLUDE_VIRTUAL_ICE_INTERFACES=true`. See the [libnice recipe](thirdparty/libnice/xmake.lua) for the list and patches.

### TLS and INI configuration

Signaling uses WSS with system certificate trust support. For a self-signed server, trust its root certificate on the application device and check certificate validity and hostname / IP matching. Certificate failures are reported as `SignalTlsCertError`. See [CrossDesk Server](https://github.com/kunkundi/crossdesk-server) for deployment and ports.

With `use_cfg_file=true`, server and media settings are read from the INI file at `cfg_path`; corresponding direct `Params` fields are not used as per-field fallback values. `cfg_path` is a fixed character array, so copy the path into it. Callbacks, `user_id`, and `user_data` still come from `Params`.

[config/config.ini](config/config.ini) is a historical example containing old endpoints and static TURN credentials, not a ready-to-use current deployment configuration. INI defaults also differ from zero-initialized `Params`. Refer to [PeerConnection::Init](src/pc/peer_connection.cpp) for supported fields and parsing rules.

<a id="validation"></a>

## Troubleshooting and validation

| Symptom | What to check |
| --- | --- |
| `Init` succeeds but login does not | Observe signaling callbacks and check WSS endpoint, reachability, and certificate trust |
| Crash during login | Verify the three required callbacks and application-context lifetime |
| Login works but a session fails | Check both peers' service, remote ID / password, TURN credentials, UDP / TCP, and relay ports |
| Black video with native output | Handle `native_frame`, not only `data`; verify type, stride, owner lifetime, and renderer |
| Audio input rejected | Supply exactly 10 ms of 48 kHz mono 16-bit PCM per call |
| CUDA build or link failure | Verify platform / architecture, SDK path, driver, dependencies, and final application link settings |
| Missing UPnP / libnice symbols or extensions | Use the current recipes and patches; avoid mixing old caches or a different GLib / GUPnP stack |

The current [xmake.lua](xmake.lua) defines static-library and object targets, with no directly runnable targets registered for `tests/`. In particular, the [old PeerConnection sample](tests/peerconnection/host.cpp) uses removed APIs and should not be treated as a current integration tutorial or a runnable example.

Validate on the target platform with two-peer login, direct / forced TURN sessions, media and data transfer, and teardown/reconnection. Compiling against the public header does not replace network and media tests. See CrossDesk's [desktop integration](https://github.com/kunkundi/crossdesk/blob/HEAD/apps/desktop/src/gui/runtime/gui_runtime.cpp) and [iOS bridge](https://github.com/kunkundi/crossdesk/blob/HEAD/apps/ios/CrossDeskMobile/Bridge/CrossDeskRTCBridge.h) for application code.

## Feedback and license

[Report issues](https://github.com/kunkundi/minirtc/issues) with the source revision, target platform / architecture, Xmake configuration, codec and TURN modes, and sanitized logs.

MiniRTC's original code uses [LGPL-3.0-only](LICENSE). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party code, dependencies, and distribution notes.
