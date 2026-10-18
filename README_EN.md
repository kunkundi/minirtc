# MiniRTC

A lightweight, cross-platform real-time media library powering [CrossDesk](https://github.com/kunkundi/crossdesk), with codecs, P2P / TURN connectivity, network adaptation, and data transport. Applications provide capture, playback, rendering, and business protocols; MiniRTC handles the transport pipeline.

[中文](README.md) · [Public API](src/api/minirtc.h) · [CrossDesk](https://github.com/kunkundi/crossdesk) · [Signaling server](https://github.com/kunkundi/crossdesk-server)

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20iOS%20%7C%20Android%20%7C%20HarmonyOS-blue?style=flat)](#platforms)
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

| Platform | Encoding | Decoding |
| --- | --- | --- |
| Windows x64 / Linux x86-64 | H.264: NVENC with CUDA, or OpenH264; AV1: SVT-AV1 | H.264: NVDEC with CUDA, or OpenH264; AV1: dav1d |
| Linux arm64 | H.264: OpenH264; AV1: SVT-AV1 | H.264: OpenH264; AV1: dav1d |
| macOS | H.264: VideoToolbox or OpenH264; AV1: SVT-AV1 | H.264: VideoToolbox or OpenH264; AV1: dav1d |
| iOS | H.264: VideoToolbox; AV1: SVT-AV1 | H.264: VideoToolbox; AV1: dav1d |
| Android arm64 | H.264 / AV1: MediaCodec with OpenH264 / SVT-AV1 fallback | H.264 / AV1: MediaCodec with OpenH264 / dav1d fallback |
| HarmonyOS arm64 (development) | H.264: OpenH264; AV1: SVT-AV1 | Native H.264 / AV1 decoding with OpenH264 / dav1d fallback |

Hardware paths require `hardware_acceleration=true`, a matching build, and device support. iOS does not build OpenH264: physical-device H.264 requires VideoToolbox; its simulator decode path can use VideoToolbox software decoding, without implying support for every simulator configuration. Apple AV1 remains software-based. libaom is optional and does not replace the default AV1 factories.

Native output varies by backend: CPU NV12, CUDA NV12, Apple `CVPixelBuffer`, or Android MediaCodec buffers. It does not imply zero-copy support on every path. See the [encoder](src/media/video/encode/video_encoder_factory.cpp), [decoder](src/media/video/decode/video_decoder_factory.cpp), and [frame ownership guide](docs/INTEGRATION_EN.md#frames).

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

1. Zero-initialize `Params`, then set the signaling endpoint, identity, codec options, TURN mode, SRTP, and callbacks.
2. Create a peer, check the result, and register video, audio, and data streams before session negotiation.
3. Call `Init` and wait for `SignalConnected`; a zero return value alone does not mean login succeeded.
4. Join the remote session and wait for `Connected` before sending frames or data.
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
