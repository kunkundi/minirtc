# MiniRTC

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)]()
[![License: LGPL v3](https://img.shields.io/badge/License-LGPL%20v3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)  
[![GitHub issues](https://img.shields.io/github/issues/kunkundi/minirtc.svg)]()
[![GitHub stars](https://img.shields.io/github/stars/kunkundi/minirtc.svg?style=social)]()
[![GitHub forks](https://img.shields.io/github/forks/kunkundi/minirtc.svg?style=social)]()

[English](README_EN.md) / [中文](README.md)

**轻量级跨平台实时音视频传输库**，专为 P2P 通信设计，低延迟、高性能、安全可靠。MiniRTC 提供完整的音视频编解码、加密传输和网络优化方案，并已在开源远程桌面项目 [CrossDesk](https://github.com/kunkundi/crossdesk.git) 中稳定应用。  

---

## 核心特性

- **跨平台支持**：Windows、Linux、macOS 等主流平台  
- **P2P 音视频传输**：点对点直连，降低延迟，提高实时性  
- **多视频编码支持**：
  - **AV1** 软件编解码  
  - **H.264** 硬件加速编码/解码  
    - Windows / Linux: **NVIDIA Video Codec SDK (NVENC/NVDEC)**  
    - macOS: **Video Toolbox**  
- **音频编码支持**：**Opus** 编解码，高质量低延迟  
- **安全加密**：支持 **SRTP 协议 (RFC 3711)**，保障音视频传输安全  
- **网络透传**：基于 **RFC 5245 (ICE)** 的 NAT 穿透，适应复杂网络环境，实现直接连接  
- **QoS 保证**：复用 WebRTC 核心模块，实现丢包恢复、带宽管理与网络抖动补偿  
- **轻量化设计**：核心库体积小，易于集成到各类项目  

## 如何编译

依赖：
- [xmake](https://xmake.io/#/guide/installation)
- [cmake](https://cmake.org/download/)

Linux环境下需安装以下包：

```
sudo apt-get install -y software-properties-common git curl unzip build-essential libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxcb-randr0-dev libxcb-xtest0-dev libxcb-xinerama0-dev libxcb-shape0-dev libxcb-xkb-dev libxcb-xfixes0-dev libxv-dev libxtst-dev libasound2-dev libsndio-dev libxcb-shm0-dev libasound2-dev libpulse-dev
```
编译
```
git clone https://github.com/kunkundi/minirtc.git

cd minirtc

xmake b minirtc
```
#### 无 CUDA 环境下的开发支持

对于未安装 **CUDA 环境** 的Linux开发者，这里提供了预配置的 [Ubuntu 22.04 Docker 镜像](https://hub.docker.com/r/crossdesk/ubuntu22.04)。  
该镜像内置必要的构建依赖，可在容器中开箱即用，无需额外配置即可直接编译项目。

进入容器，下载工程后执行：
```
export CUDA_PATH=/usr/local/cuda
export XMAKE_GLOBALDIR=/data

xmake b --root minirtc
```

## 关于 Xmake

#### 安装 Xmake
使用 curl：
```
curl -fsSL https://xmake.io/shget.text | bash
```
使用 wget：
```
wget https://xmake.io/shget.text -O - | bash
```
使用 powershell：
```
irm https://xmake.io/psget.text | iex
```

#### 编译选项
```
# 切换编译模式
xmake f -m debug/release

# 可选编译参数
-r ：重新构建目标
-v ：显示详细的构建日志
-y ：自动确认提示

# 示例
xmake b -vy minirtc
```
更多使用方法可参考 [Xmake官方文档](https://xmake.io/guide/quick-start.html) 。