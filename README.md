# MiniRTC

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20iOS-brightgreen.svg)]()
[![License: LGPL v3](https://img.shields.io/badge/License-LGPL%20v3-blue.svg)](https://www.gnu.org/licenses/lgpl-3.0)
[![GitHub issues](https://img.shields.io/github/issues/kunkundi/minirtc.svg)]()
[![GitHub stars](https://img.shields.io/github/stars/kunkundi/minirtc.svg?style=social)]()
[![GitHub forks](https://img.shields.io/github/forks/kunkundi/minirtc.svg?style=social)]()

[ [English](README_EN.md) / 中文 ]

MiniRTC 是一个**轻量级跨平台的实时音视频传输库**。它提供音视频编解码、ICE/STUN/TURN、SRTP、拥塞控制以及可靠与非可靠数据通道，并已用于开源远程桌面项目 [CrossDesk](https://github.com/kunkundi/crossdesk)。

## 核心特性

- 支持 Windows、Linux、macOS 和 iOS。
- 支持 P2P 直连以及 TURN/UDP、TURN/TCP 中继回退。
- 支持 H.264 和 AV1 视频、Opus 音频。
- 支持 SRTP、NACK、带宽估计、拥塞控制与网络状态统计。
- 支持可靠与非可靠数据流，可承载控制、键鼠、剪贴板和文件数据。

## 平台与编解码能力

| 平台 | H.264 硬件路径 | H.264 软件路径 | AV1 编码 / 解码 | 原生解码输出 |
| --- | --- | --- | --- | --- |
| Windows | NVIDIA NVENC/NVDEC（启用 CUDA 时） | OpenH264 | SVT-AV1 / dav1d | 暂不提供 |
| Linux x86-64 | NVIDIA NVENC/NVDEC（启用 CUDA 时） | OpenH264 | SVT-AV1 / dav1d | 暂不提供 |
| macOS | VideoToolbox | OpenH264 | SVT-AV1 / dav1d | `CVPixelBufferRef` |
| iOS | VideoToolbox | OpenH264 | SVT-AV1 / dav1d | `CVPixelBufferRef` |

当前 AV1 运行时路径使用 SVT-AV1 编码和 dav1d 解码。libaom 也会随默认构建编入，但不作为工厂的默认 AV1 实现。Apple 平台暂不使用 VideoToolbox 进行 AV1 编解码。

## 构建

### 环境要求

- [Xmake](https://xmake.io/#/guide/installation)
- CMake
- 支持 C++17 的编译工具链
- 构建 iOS 时需要完整安装 Xcode 和 iPhoneOS SDK

Xmake 会解析并构建 MiniRTC 的第三方依赖。首次构建需要访问依赖源，耗时会比增量构建更长。

### 桌面平台

```sh
git clone https://github.com/kunkundi/minirtc.git
cd minirtc

xmake f -m release -y
xmake b minirtc
```

调试构建可将 `release` 改为 `debug`。CUDA 默认关闭；需要 NVIDIA 硬件编解码时使用：

```sh
xmake f -m release --USE_CUDA=true --CUDA_DIR=/usr/local/cuda -y
xmake b minirtc
```

没有 CUDA 环境的 Linux 开发者也可以使用预配置的 [Ubuntu 22.04 Docker 镜像](https://hub.docker.com/r/crossdesk/ubuntu22.04)。

### iOS 构建

当前已验证的配置为 iOS 16.0 及以上、arm64 真机：

```sh
xmake f -c -p iphoneos -a arm64 -m release \
  --target_minver=16.0 --USE_CUDA=false -y
xmake b minirtc
```

## 常用 Xmake 选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `USE_CUDA` | `false` | 在支持的平台启用 NVIDIA 编解码后端 |
| `CUDA_DIR` | 自动检测 | 指定 CUDA SDK 路径 |

常用命令：

```sh
# 显示可配置选项
xmake f --menu

# 显示详细日志并自动确认依赖安装
xmake b -vy minirtc

# 重新构建
xmake b -r minirtc
```

更多信息请参阅 [Xmake 官方文档](https://xmake.io/guide/quick-start.html)。
