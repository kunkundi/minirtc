# MiniRTC

轻量级跨平台实时音视频传输库，为 [CrossDesk](https://github.com/kunkundi/crossdesk) 提供媒体编解码、P2P / TURN 连接、网络自适应和数据传输能力。应用负责采集、播放、渲染及业务协议，MiniRTC 负责传输链路。

[English](README_EN.md) · [公开 API](src/api/minirtc.h) · [CrossDesk](https://github.com/kunkundi/crossdesk) · [信令服务端](https://github.com/kunkundi/crossdesk-server)

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS%20%7C%20iOS%20%7C%20Android%20%7C%20HarmonyOS-blue?style=flat)](#platforms)
[![License: LGPL v3](https://img.shields.io/badge/license-LGPL--3.0-blue)](LICENSE)
[![GitHub issues](https://img.shields.io/github/issues/kunkundi/minirtc)](https://github.com/kunkundi/minirtc/issues)

[核心能力](#features) · [平台与编解码](#platforms) · [构建](#build) · [接入流程](#integration) · [媒体与内存](#frames) · [连接配置](#network) · [排查与验证](#validation)

<a id="features"></a>

## 核心能力

- **媒体传输：** H.264 / AV1 视频、Opus 音频、多媒体流和面向指定 Peer 的发送接口。
- **连接与穿透：** WSS 信令、ICE / STUN、TURN UDP / TCP、动态 TURN 凭据，以及可用网关上的 UPnP 映射。
- **网络自适应：** SRTP、RTP / RTCP、NACK 重传、带宽估计、拥塞控制，以及帧率 / 分辨率降级策略。
- **数据传输：** 可靠与非可靠数据流；键鼠、剪贴板、文件等数据的序列化与业务处理由应用实现。
- **浏览器互通：** 通过 libdatachannel 路径与 [CrossDesk Web Client](https://github.com/kunkundi/crossdesk-web-client) 交互。需要匹配的信令与媒体协商，不是任意 WebRTC 服务的即插即用客户端。

构建产物是 **C++17 静态库**，公开函数采用 `extern "C"` 链接。当前头文件含 C++ 类型与语法，不能直接作为纯 C 头文件使用。

<a id="platforms"></a>

## 平台与编解码

| 平台 | 编码 | 解码 |
| --- | --- | --- |
| Windows x64 / Linux x86-64 | H.264：CUDA NVENC 或 OpenH264；AV1：SVT-AV1 | H.264：CUDA NVDEC 或 OpenH264；AV1：dav1d |
| Linux arm64 | H.264：OpenH264；AV1：SVT-AV1 | H.264：OpenH264；AV1：dav1d |
| macOS | H.264：VideoToolbox 或 OpenH264；AV1：SVT-AV1 | H.264：VideoToolbox 或 OpenH264；AV1：dav1d |
| iOS | H.264：VideoToolbox；AV1：SVT-AV1 | H.264：VideoToolbox；AV1：dav1d |
| Android arm64 | H.264 / AV1：MediaCodec，失败回退 OpenH264 / SVT-AV1 | H.264 / AV1：MediaCodec，失败回退 OpenH264 / dav1d |
| HarmonyOS arm64（开发版） | H.264：OpenH264；AV1：SVT-AV1 | H.264 / AV1 原生解码，失败回退 OpenH264 / dav1d |

硬件路径要求 `hardware_acceleration=true`，并受构建配置和设备能力影响。iOS 不构建 OpenH264，真机 H.264 依赖 VideoToolbox；模拟器解码路径可使用 VideoToolbox 软件实现，但不代表所有模拟器配置都已验证。Apple 平台 AV1 仍使用软件路径；可选 libaom 后端不替代默认 AV1 工厂。

原生输出随后端使用 CPU NV12、CUDA NV12、Apple `CVPixelBuffer` 或 Android MediaCodec 缓冲区，不代表每条路径都零拷贝。详见 [编码工厂](src/media/video/encode/video_encoder_factory.cpp)、[解码工厂](src/media/video/decode/video_decoder_factory.cpp) 和 [帧生命周期](docs/INTEGRATION.md#frames)。

<a id="build"></a>

## 快速构建

准备 Git、[Xmake](https://xmake.io/guide/quick-start.html#installation) 和 C++17 编译器。Windows 使用 MSVC 与 Windows SDK，x64 的 libyuv 依赖还使用 `clang-cl`；macOS 使用 Xcode，Linux 使用 GCC / Clang。

```sh
git clone https://github.com/kunkundi/minirtc.git
cd minirtc
xmake f -m release --USE_CUDA=false -y
xmake b minirtc
```

首次构建由 Xmake 解析和下载依赖，生成 `libminirtc.a` 或 `minirtc.lib`；最终应用还需链接匹配的依赖库与系统库。CUDA、iOS、Android、HarmonyOS、构建选项及安装方法见 [构建指南](docs/INTEGRATION.md#build)。嵌入 CrossDesk 的构建应使用对应应用的平台脚本。

<a id="integration"></a>

## 接入应用

1. 清零 `Params`，填写信令地址、本机身份、编解码选项、TURN 模式、SRTP 和回调。
2. 创建 Peer 并检查结果，在协商前注册视频、音频和数据流。
3. 调用 `Init`，等待 `SignalConnected`；返回 0 本身不表示登录成功。
4. 加入远程会话，等待 `Connected` 后再发送帧或数据。
5. 结束时停止应用侧采集和发送、退出会话，再销毁 Peer。

从 [初始化示例与回调约定](docs/INTEGRATION.md#integration) 开始，并以 [公开头文件](src/api/minirtc.h) 为接口依据。当前调用路径要求三个状态 / 报告回调有效；`tests/` 中的旧示例不能作为现行接入教程。

<a id="frames"></a>

## 媒体与内存

- CPU 视频输入为连续 **NV12**，宽高为正偶数。
- 音频输入为 **48 kHz、单声道、16-bit PCM**，每帧恰好 **10 ms / 960 字节**。
- 采集时间戳使用**单调时钟微秒**。
- 回调缓冲区是借用数据。异步使用前复制 CPU 数据；原生描述符需复制并保留所有者，使用后释放。Android MediaCodec 输出使用独立的渲染约定。

实现采集与渲染前请阅读 [媒体格式和生命周期](docs/INTEGRATION.md#frames)。

<a id="network"></a>

## 连接与自托管

使用 [CrossDesk Server](https://github.com/kunkundi/crossdesk-server) 提供 WSS 信令，Coturn 提供中继。当前会话使用服务端下发的 ICE 列表和临时 TURN 凭据；额外 STUN / TURN 节点在服务端配置。两端需要兼容的信令服务并信任其证书。

[ICE 与 TURN 模式](docs/INTEGRATION.md#network) · [TLS 与 INI 配置](docs/INTEGRATION.md#tls-与-ini-配置) · [浏览器客户端](https://github.com/kunkundi/crossdesk-web-client)

<a id="validation"></a>

## 排查与验证

[排查指南](docs/INTEGRATION.md#validation) 覆盖登录、回调、原生帧、音频、CUDA 和依赖问题。集成后需在目标平台验证登录、直连 / 中继、媒体与数据传输及退出重连；当前 Xmake 没有为旧 `tests/` 目录注册可直接运行的目标。

## 反馈与许可

[提交问题](https://github.com/kunkundi/minirtc/issues)时附上源码版本、目标平台 / 架构、Xmake 配置、编解码与 TURN 模式，以及脱敏日志。

MiniRTC 原创代码使用 [LGPL-3.0-only](LICENSE)。第三方代码、依赖和分发注意事项见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
