# MiniRTC

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-brightgreen.svg)]()
[![License: LGPL v3](https://img.shields.io/badge/License-LGPL%20v3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)  
[![GitHub issues](https://img.shields.io/github/issues/kunkundi/minirtc.svg)]()
[![GitHub stars](https://img.shields.io/github/stars/kunkundi/minirtc.svg?style=social)]()
[![GitHub forks](https://img.shields.io/github/forks/kunkundi/minirtc.svg?style=social)]()

[ [中文](README_CN.md) / English ]

**Lightweight cross-platform real-time audio and video transmission library** designed for P2P communication. MiniRTC provides complete audio/video encoding/decoding, secure transmission, and network optimization. It is already stably used in the open-source remote desktop project [CrossDesk](https://github.com/kunkundi/crossdesk.git).  

---

## Key Features

- **Cross-platform support**: Windows, Linux, macOS  
- **P2P audio/video transmission**: Direct peer-to-peer connection for low latency and high real-time performance  
- **Video codec support**:
  - **AV1** software encoding/decoding  
  - **H.264** hardware-accelerated encoding/decoding  
    - Windows / Linux: **NVIDIA Video Codec SDK (NVENC/NVDEC)**  
    - macOS: **Video Toolbox**  
- **Audio codec support**: **Opus** encoding/decoding for high-quality, low-latency audio  
- **Secure transmission**: Supports **SRTP (RFC 3711)** to protect audio/video streams  
- **Network traversal**: NAT traversal based on **RFC 5245 (ICE)** for direct P2P connections in complex network environments  
- **QoS support**: Reuses WebRTC core modules for packet loss recovery, bandwidth management, and jitter compensation  
- **Lightweight design**: Small core library, easy to integrate into various projects

## How to build

Requirements:
- [xmake](https://xmake.io/#/guide/installation)
- [cmake](https://cmake.org/download/)

Following packages need to be installed on Linux:

```
sudo apt-get install -y software-properties-common git curl unzip build-essential libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxcb-randr0-dev libxcb-xtest0-dev libxcb-xinerama0-dev libxcb-shape0-dev libxcb-xkb-dev libxcb-xfixes0-dev libxv-dev libxtst-dev libasound2-dev libsndio-dev libxcb-shm0-dev libasound2-dev libpulse-dev
```

Build:
```
git clone https://github.com/kunkundi/crossdesk.git

cd crossdesk

git submodule init 

git submodule update

xmake b crossdesk
```

## Development Without CUDA Environment

For developers who do not have a **CUDA environment** installed, a preconfigured [Ubuntu 22.04 Docker image](https://hub.docker.com/r/crossdesk/ubuntu22.04) is provided.  
This image comes with all required build dependencies and allows you to build the project directly inside the container without any additional setup.

After entering the container, download the project and run:
```
export CUDA_PATH=/usr/local/cuda
export XMAKE_GLOBALDIR=/data

xmake b --root crossdesk
```

## About Xmake
#### Installing Xmake

You can install Xmake using one of the following methods:

Using curl:
```
curl -fsSL https://xmake.io/shget.text | bash
```
Using wget:
```
wget https://xmake.io/shget.text -O - | bash
```
Using powershell:
```
irm https://xmake.io/psget.text | iex
```

#### Build Options
```
# Switch build mode
xmake f -m debug/release

# Optional build parameters
-r : Rebuild the target
-v : Show detailed build logs
-y : Automatically confirm prompts

# Example
xmake b -vy minirtc
```
For more information, please refer to the [official Xmake documentation](https://xmake.io/guide/quick-start.html) .