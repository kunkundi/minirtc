# MiniRTC

A lightweight, cross-platform real-time media library powering [CrossDesk](https://github.com/kunkundi/crossdesk), with codecs, P2P / TURN connectivity, network adaptation, and data transport. Applications provide capture, playback, rendering, and business protocols; MiniRTC handles the transport pipeline.

[中文](README_ZH.md) · [Public API](src/api/minirtc.h) · [CrossDesk](https://github.com/kunkundi/crossdesk) · [Signaling server](https://github.com/kunkundi/crossdesk-server)

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20iOS%20%7C%20Android%20%7C%20HarmonyOS-blue?style=flat)](#platforms)
[![License: LGPL v3](https://img.shields.io/badge/license-LGPL--3.0-blue)](LICENSE)
[![GitHub issues](https://img.shields.io/github/issues/kunkundi/minirtc)](https://github.com/kunkundi/minirtc/issues)

[Features](#features) · [Platforms and codecs](#platforms) · [Build](#build) · [Integration](#integration) · [Media and memory](#frames) · [Connectivity](#network) · [Validation](#validation)

<a id="features"></a>

## Features

- **Media:** H.264, H.265 and AV1 video, Opus audio, multiple media streams, and APIs for sending to a specific peer.
- **Connectivity:** WSS signaling, ICE / STUN, TURN UDP / TCP, dynamic TURN credentials, and UPnP mapping on supported gateways.
- **Network adaptation:** SRTP, RTP / RTCP, NACK retransmission, bandwidth estimation, congestion control, and frame-rate / resolution degradation policies.
- **Data:** reliable and unreliable streams. Applications implement serialization and handling for mouse, keyboard, clipboard, file, and other messages.
- **Browser interoperability:** a libdatachannel path supports [CrossDesk Web Client](https://github.com/kunkundi/crossdesk-web-client). Matching signaling and media negotiation are required; this is not a drop-in client for arbitrary WebRTC services.

The build produces a **C++17 static library** with `extern "C"` function linkage. The public header uses C++ types and syntax and cannot be included directly as a pure C header.

<a id="platforms"></a>

## Platforms and codecs

| Platform | Encoding | Decoding |
| --- | --- | --- |
| Windows x64 | H.264: NVENC, WMF or OpenH264; H.265: NVENC or WMF; AV1: SVT-AV1 | H.264: NVDEC, D3D11VA or OpenH264; H.265: NVDEC or WMF with D3D11VA; AV1: dav1d |
| Linux x86-64 | H.264: NVENC with CUDA, or OpenH264; H.265: NVENC; AV1: SVT-AV1 | H.264: NVDEC with CUDA, or OpenH264; H.265: NVDEC; AV1: dav1d |
| Linux arm64 | H.264: OpenH264; AV1: SVT-AV1 | H.264: OpenH264; AV1: dav1d |
| macOS | H.264: VideoToolbox or OpenH264; H.265: VideoToolbox; AV1: SVT-AV1 | H.264: VideoToolbox or OpenH264; H.265: VideoToolbox; AV1: dav1d |
| iOS | H.264: VideoToolbox; AV1: SVT-AV1 | H.264: VideoToolbox; AV1: dav1d |
| Android arm64 | H.264 / AV1: MediaCodec with OpenH264 / SVT-AV1 fallback | H.264 / AV1: MediaCodec with OpenH264 / dav1d fallback |
| HarmonyOS arm64 (development) | H.264: OpenH264; AV1: SVT-AV1 | Native H.264 / AV1 decoding with OpenH264 / dav1d fallback |

For native connections, pass `MiniRtcVideoCodec::H265` to `MiniRtcCreatePeerWithVideoCodec` to prefer HEVC Main (8-bit 4:2:0). To switch future connections to HEVC, call `MiniRtcUpdateConnectionParams` with `video_codec = MiniRtcVideoCodec::H265`, hardware acceleration enabled, and the desired TURN and desktop encoding modes. Each peer needs hardware HEVC encoding and decoding. Windows can use a hardware Media Foundation encoder and a D3D11-aware HEVC decoder such as the HEVC Video Extension, or NVENC/NVDEC in CUDA builds. macOS uses VideoToolbox and Linux uses NVENC/NVDEC. H.264 remains the default and the fallback for legacy peers.

Hardware paths require `hardware_acceleration=true`, a matching build, and device support. iOS does not build OpenH264: physical-device H.264 requires VideoToolbox; its simulator decode path can use VideoToolbox software decoding, without implying support for every simulator configuration. Apple AV1 remains software-based. libaom is optional and does not replace the default AV1 factories.

Native output varies by backend: CPU NV12, CUDA NV12, Apple `CVPixelBuffer`, or Android MediaCodec buffers. It does not imply zero-copy support on every path. See the [encoder](src/media/video/encode/video_encoder_factory.cpp), [decoder](src/media/video/decode/video_decoder_factory.cpp), and [frame ownership guide](docs/INTEGRATION_EN.md#frames).

### Windows Media Foundation encoder buffering

The optional ScreenContent `CODECAPI_AVEncCommonBufferSize` policy is independent of average bitrate, real PTS, no-B-frame encoding and packet admission. Set these variables before starting the process:

| Variable | Behavior |
| --- | --- |
| `MINIRTC_WMF_BUFFER_MS` | Defaults to 300ms for ordinary frames; IDR/recovery requests use at least 600ms. 0 leaves the property untouched, without disabling the driver's internal buffering. Accepts 100–2000ms. |
| `MINIRTC_WMF_INITIAL_BUFFER_MS` | Optional initialization experiment, 100–2000ms, including the first IDR. Missing, zero or invalid values use `max(ordinary duration, 600)`. Does not override disabled buffer control. |

The ordinary 300/600 options use the same initial recovery allowance by default. Durations are converted to bytes at the negotiated bitrate, clamped to 4096–131072, then adjusted to a known driver range. Use the second variable with fresh processes/MFTs to compare initial 300ms and 600ms, checking first frames, periodic IDRs, FIR recovery and quality.

Successful initialization readback does not establish runtime support. Dynamic writes require known, satisfactory readback; `S_OK`, `IsSupported` and unknown readback alone prove nothing. A failed tighter request gets at most one recovery attempt. Failure stops optional writes for that instance without rebuilding or losing a pending IDR. Recovery that only retains the old value also stops probing. Normal resolution rebuilds negotiate capability again.

`WMF encoder buffer state` logs separate initialization confirmation, `dynamic_capability`, the last request and current readback. `fixed_readback` retains the observed bytes; its `readback_equivalent_ms` changes with bitrate and must not continually be described as 600ms. Read-only monitoring continues periodically and after IDRs when writes stop. `readback_verified` confirms property changes only, not bitstream HRD compliance or a hard per-frame cap. Initialization broken by an optional write retains one clean MFT retry without that property.

<a id="build"></a>

## Build quickly

Install Git, [Xmake](https://xmake.io/guide/quick-start.html#installation), and a C++17 compiler. Windows uses MSVC and the Windows SDK; the libyuv dependency also uses `clang-cl` on x64. macOS uses Xcode, and Linux uses GCC / Clang.

```sh
git clone https://github.com/kunkundi/minirtc.git
cd minirtc
xmake f -m release --USE_CUDA=false -y
xmake b minirtc
```

Xmake resolves dependencies on the first build. The result is `libminirtc.a` or `minirtc.lib`; final applications also need the matching dependency and system libraries. See the [build guide](docs/INTEGRATION_EN.md#build) for CUDA, iOS, Android, HarmonyOS, build options, and installation. Existing builds embedded in CrossDesk should use that application's platform scripts.

<a id="integration"></a>

## Integrate into an application

Public functions, types, callbacks and unscoped enum values use the `MiniRtc` prefix. The header groups type/configuration definitions first, followed by lifecycle, signaling, stream, media, data, direct-session and utility functions. Consumers migrate to the new names and rebuild against the matching library.

Public functions and data types are declared in `minirtc.h`, the header installed by the SDK. Use `MiniRtcConfigureLogging` for log callbacks and thresholds. Address parsing, join-rejection parsing, and Android JavaVM configuration are also available through this header.

1. Zero-initialize `MiniRtcParams`, then set the signaling endpoint, identity, codec options, TURN mode, SRTP, and callbacks.
2. Create a peer, check the result, and register video, audio, and data streams before session negotiation.
3. Call `MiniRtcInit` and wait for `MiniRtcSignalConnected`; a zero return value alone does not mean login succeeded.
4. Join the remote session and wait for `MiniRtcConnectionConnected` before sending frames or data.
5. Stop application capture and sending, leave the session, and destroy the peer when finished.

Start with the [configuration example and callback rules](docs/INTEGRATION_EN.md#integration), then consult the [public header](src/api/minirtc.h). The three status/report callbacks are required by current call paths. Old samples under `tests/` are not a current integration tutorial.

<a id="frames"></a>

## Media and memory

- CPU video input is packed **NV12** with positive even dimensions.
- Audio input is **48 kHz, mono, 16-bit PCM**, exactly **10 ms / 960 bytes** per frame.
- Capture timestamps use **monotonic microseconds**.
- Callback buffers are borrowed. Copy CPU data before asynchronous use; copy and retain native descriptors, then release them after use. Android MediaCodec output has a separate rendering contract.

See [media formats and ownership](docs/INTEGRATION_EN.md#frames) before implementing capture or rendering.

<a id="network"></a>

## Connectivity and self-hosting

Use [CrossDesk Server](https://github.com/kunkundi/crossdesk-server) for WSS signaling and Coturn for relay. Current sessions use server-issued ICE lists and temporary TURN credentials; configure additional STUN / TURN endpoints on the server. Both peers must use compatible signaling and trust its certificate.

[ICE and TURN modes](docs/INTEGRATION_EN.md#network) · [TLS and INI configuration](docs/INTEGRATION_EN.md#tls-and-ini-configuration) · [Browser client](https://github.com/kunkundi/crossdesk-web-client)

<a id="validation"></a>

## Troubleshooting and validation

The [troubleshooting guide](docs/INTEGRATION_EN.md#validation) covers login, callbacks, native frames, audio, CUDA, and dependency problems. Validate login, direct / relay sessions, media/data transfer, and teardown on the target platform. The current Xmake file does not register runnable targets for the legacy `tests/` directory.

## Feedback and license

[Report issues](https://github.com/kunkundi/minirtc/issues) with the source revision, target platform / architecture, Xmake configuration, codec and TURN modes, and sanitized logs.

MiniRTC's original code uses [LGPL-3.0-only](LICENSE). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for third-party code, dependencies, and distribution notes.
