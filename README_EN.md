# MiniRTC

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20iOS-brightgreen.svg)]()
[![License: LGPL v3](https://img.shields.io/badge/License-LGPL%20v3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)
[![GitHub issues](https://img.shields.io/github/issues/kunkundi/minirtc.svg)]()
[![GitHub stars](https://img.shields.io/github/stars/kunkundi/minirtc.svg?style=social)]()
[![GitHub forks](https://img.shields.io/github/forks/kunkundi/minirtc.svg?style=social)]()

[ [中文](README.md) / English ]

MiniRTC is a **lightweight, cross-platform real-time audio and video transmission library**. It provides audio/video codecs, ICE/STUN/TURN, SRTP, congestion control, and reliable or unreliable data channels, and is used by the open-source remote desktop project [CrossDesk](https://github.com/kunkundi/crossdesk).

## Key features

- Supports Windows, Linux, macOS, and iOS.
- Supports direct P2P connectivity with TURN/UDP and TURN/TCP relay fallback.
- Supports H.264 and AV1 video plus Opus audio.
- Provides SRTP, NACK, bandwidth estimation, congestion control, and network statistics.
- Provides reliable and unreliable data streams for control, mouse, keyboard, clipboard, and file traffic.

## Platform and codec support

| Platform | H.264 hardware path | H.264 software path | AV1 encode / decode | Native decoded output |
| --- | --- | --- | --- | --- |
| Windows | NVIDIA NVENC/NVDEC when CUDA is enabled | OpenH264 | SVT-AV1 / dav1d | Not available |
| Linux x86-64 | NVIDIA NVENC/NVDEC when CUDA is enabled | OpenH264 | SVT-AV1 / dav1d | Not available |
| macOS | VideoToolbox | OpenH264 | SVT-AV1 / dav1d | `CVPixelBufferRef` |
| iOS | VideoToolbox | OpenH264 | SVT-AV1 / dav1d | `CVPixelBufferRef` |

The active AV1 runtime path currently uses SVT-AV1 for encoding and dav1d for decoding. libaom is also included in the default build, but is not selected by the codec factories. VideoToolbox AV1 encoding and decoding are not currently used on Apple platforms.

## Building

### Requirements

- [Xmake](https://xmake.io/#/guide/installation)
- CMake
- A C++17 toolchain
- A complete Xcode and iPhoneOS SDK installation for iOS builds

Xmake resolves and builds MiniRTC's third-party dependencies. The first build requires access to the dependency sources and takes longer than incremental builds.

### Desktop platforms

```sh
git clone https://github.com/kunkundi/minirtc.git
cd minirtc

xmake f -m release -y
xmake b minirtc
```

Replace `release` with `debug` for a debug build. CUDA is disabled by default. Enable NVIDIA hardware codecs with:

```sh
xmake f -m release --USE_CUDA=true --CUDA_DIR=/usr/local/cuda -y
xmake b minirtc
```

Linux developers without a CUDA environment can also use the preconfigured [Ubuntu 22.04 Docker image](https://hub.docker.com/r/crossdesk/ubuntu22.04).

### iOS builds

The currently verified configuration is an arm64 device running iOS 16.0 or later:

```sh
xmake f -c -p iphoneos -a arm64 -m release \
  --target_minver=16.0 --USE_CUDA=false -y
xmake b minirtc
```

## Common Xmake options

| Option | Default | Description |
| --- | --- | --- |
| `USE_CUDA` | `false` | Enables NVIDIA codec backends on supported platforms |
| `CUDA_DIR` | Auto-detected | Overrides the CUDA SDK path |

Useful commands:

```sh
# Display configurable options
xmake f --menu

# Show verbose logs and automatically confirm dependency installation
xmake b -vy minirtc

# Rebuild the target
xmake b -r minirtc
```

See the [Xmake quick-start documentation](https://xmake.io/guide/quick-start.html) for more information.
