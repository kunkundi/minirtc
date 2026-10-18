# MiniRTC 构建与接入指南

[返回项目首页](../README.md) · [English](INTEGRATION_EN.md)

[构建](#build) · [接入流程](#integration) · [媒体与内存](#frames) · [连接配置](#network) · [排查与验证](#validation)

<a id="build"></a>

## 构建

### 环境与依赖

需要 Git、[Xmake](https://xmake.io/guide/quick-start.html#installation) 和 C++17 工具链：Windows 使用 MSVC 与 Windows SDK，x64 的 libyuv 依赖还使用 `clang-cl`；Linux 使用 GCC / Clang，macOS 使用 Xcode 工具链。iOS 构建需要完整 Xcode 与 iPhoneOS SDK。

依赖由 [xmake.lua](../xmake.lua) 与 [thirdparty](../thirdparty/xmake.lua) 的包配方解析，包括编解码库、libnice、libdatachannel、SRTP、KCP 和 UPnP 依赖栈。配方会引入 CMake、Meson、Ninja、NASM 等构建工具；首次构建需要访问依赖源，耗时与缓存状态有关。

### 桌面平台

```sh
git clone https://github.com/kunkundi/minirtc.git
cd minirtc
xmake f -m release --USE_CUDA=false -y
xmake b minirtc
```

将 `release` 改为 `debug` 可构建调试版本；使用 `-p` / `-a` 显式指定平台和架构。Windows 当前设置静态 CRT（`MT`），最终应用应保持运行库配置一致。

**CUDA：** 以下是 Linux x86-64 示例，需先安装匹配的 CUDA SDK 与 NVIDIA 驱动，并将路径替换为本机实际路径：

```sh
xmake f -p linux -a x86_64 -m release \
  --USE_CUDA=true --CUDA_DIR=/usr/local/cuda -y
xmake b minirtc
```

Windows 可使用 `--USE_CUDA=true` 配合 `CUDA_PATH` 或 `--CUDA_DIR` 指定 SDK。编入 CUDA 后仍需在 `Params` 中设置 `hardware_acceleration=true`；Apple 的 VideoToolbox 不需要 CUDA。

### iOS

当前 CrossDesk iOS 集成使用 **iOS 16.0+、arm64 真机**。在 macOS 终端的 MiniRTC 根目录执行：

```sh
xmake f -c -p iphoneos -a arm64 -m release \
  --target_minver=16.0 --USE_CUDA=false -y
xmake b minirtc
```

此命令生成 iOS 静态库，不会生成 App 或 XCFramework，也不代表模拟器构建已验证。切回桌面构建时应重新配置目标平台。Xcode 工程的依赖合并、链接和 SDK 环境处理可参考 CrossDesk 的 [iOS 构建脚本](https://github.com/kunkundi/crossdesk/blob/HEAD/apps/ios/scripts/build_minirtc_ios.sh)。

### Android

Android arm64 在 `hardware_acceleration=true` 时优先使用 MediaCodec 进行 H.264 / AV1 编解码；硬件不可用或失败时，编码回退到 OpenH264 / SVT-AV1，解码回退到 OpenH264 / dav1d。UPnP 关闭。SVT-AV1 使用内置的 `getauxval` CPU 检测，按运行设备选择 NEON 及扩展指令。使用 Xmake 3.1.1 和 NDK r28c，在本仓库根目录执行，`ANDROID_NDK_HOME` 指向已安装的 NDK：

```sh
xmake f -p android -a arm64-v8a -m release --ndk="$ANDROID_NDK_HOME" \
  --ndk_sdkver=26 --runtimes=c++_static --policies=package.precompiled:n -y
xmake b minirtc
```

Android 适配直接维护在本仓库内。宿主工程可直接包含本仓库的 `xmake.lua`，同时为构建产物和依赖缓存设置独立目录。创建 Peer 前，Android 宿主需将系统 TrustManager 根证书导出到应用私有 PEM 文件，并将 `SSL_CERT_FILE` 设置为该路径；MiniRTC 显式加载此文件并校验证书链及主机名。Android 与其他平台使用相同的解码任务提交逻辑，当前未限制压缩帧的解码排队长度和等待时间。

AV1 发送端设置 `Params::av1_encoding=true`，添加视频流后提交 NV12 帧。启用硬件加速时先尝试 MediaCodec，失败后回退 SVT-AV1；关闭硬件加速时直接使用 SVT-AV1。实时性能取决于实际选中的编解码器、设备、分辨率和帧率。

### HarmonyOS

仓库已包含 `harmony` 平台的 Xmake 构建路径、依赖配方，以及带软件回退的 H.264 / AV1 原生解码器；编码当前使用 OpenH264 / SVT-AV1。SDK 选择和应用集成见 CrossDesk 的 [鸿蒙构建说明](https://github.com/kunkundi/crossdesk/blob/HEAD/apps/harmony/README.md)。真机能力受 SDK、权限与硬件解码器影响，当前按开发集成使用。

### 选项与产物

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `MINIRTC_ENABLE_AOM` | `false` | 构建可选 libaom 后端，不改变工厂的默认选择 |
| `USE_CUDA` | `false` | 在支持的平台编入 NVIDIA 编解码后端 |
| `CUDA_DIR` | 空，自动检测 | 优先显式配置，再检查 `CUDA_PATH` / `CUDA_HOME` 及常见安装路径 |
| `MINIRTC_INCLUDE_VIRTUAL_ICE_INTERFACES` | `false` | 是否允许从 VPN / TUN 等虚拟接口收集 ICE 候选 |

```sh
xmake show -t minirtc
xmake install -o build/install minirtc
```

第一条命令显示当前目标与依赖信息；第二条将库和公开头文件安装到 `build/install`。库为 `libminirtc.a` 或 Windows 上的 `minirtc.lib`。仅复制该静态库并不足以链接完整应用，还需要对应架构和配置的依赖库、系统库及 Apple frameworks。完整依赖以 Xmake 解析结果为准。

常用诊断命令：`xmake f --menu` 查看选项，`xmake b -vy minirtc` 输出详细构建日志，`xmake b -r minirtc` 重建目标。

<a id="integration"></a>

## 接入流程

以 [src/api/minirtc.h](../src/api/minirtc.h) 为接口依据。当前流程是：

1. 使用 `Params params{}` 清零，填写地址、媒体选项和回调。`Params` 没有应用级默认配置，零初始化也不会自动启用 TURN 或 SRTP。
2. `CreatePeer(&params)` 创建实例；检查返回值。注册 `AddVideoStream`、`AddAudioStream` 和 `AddDataStream`，在会话协商前固定流名称。
3. 调用 `Init(peer)` 启动异步信令连接。**返回 0 不表示登录成功**；以 `on_signal_status` 收到 `SignalConnected` 为准。
4. 发起方使用 `JoinConnection(peer, "远端ID@密码")` 加入 CrossDesk 会话。`user_id` 是本地登录身份，不能用远端目标 ID 替代；具体身份注册与密码规则由信令服务决定。
5. `on_connection_status` 收到 `Connected` 后，根据已注册的流名称发送媒体或数据。API 返回值不代表远端已收到内容。
6. 退出会话调用 `LeaveConnection`；停止应用侧采集和发送任务，再用 `if (peer) DestroyPeer(&peer);` 销毁实例。销毁后指针置空。

### 初始化配置示例

以下函数构造直接配置模式的参数，不发起网络连接。替换示例 IP；回调由应用实现，在 `CreatePeer` 前补充需要的媒体和数据接收回调。

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

`signal_server_ip` 填主机名或 IP，**不带 `wss://`、端口或路径**；库会拼接 WSS 地址。STUN / TURN 主机字段也不带 URL 前缀。示例 `9099` / `3478` 来自 CrossDesk Server 的 Compose 默认值，不是 `Params` 自带的默认端口。

### 回调与线程

- **必须提供有效的 `on_signal_status`、`on_connection_status` 和 `on_net_status_report`。** 当前实现有直接调用这些回调的路径；不使用时也应提供同签名空函数。
- 视频使用 `on_receive_video_frame`，音频使用 `on_receive_audio_buffer`，应用数据使用 `on_receive_data_buffer`。旧字段 `on_receive_video_buffer` 不接入当前视频帧分发路径。
- `on_signal_message` 接收未被库内部信令处理器消费的消息；它不是全部原始信令的监听器。
- 回调可来自内部工作线程。将 UI 操作投递到应用线程，及时复制或保留需要的数据，并避免在回调内同步销毁 Peer。`user_data` 在 Peer 完全销毁前必须有效；`user_id` 指向的字符串至少保持到 `Init` 完成读取。
- 标识和缓冲区按回调给出的长度读取，不依赖额外的 `\0` 终止符。网络报告的指针也是回调期间借用的。

<a id="frames"></a>

## 媒体格式与内存

| 数据 | 当前约定 |
| --- | --- |
| CPU 视频输入 | `MiniRtcVideoFrame`：连续 NV12，正偶数宽高，至少 `width × height × 3 / 2` 字节 |
| 原生视频 | `MiniRtcNativeVideoFrame`：按 `type` 区分 CPU NV12、CUDA NV12、`CVPixelBuffer` 或 Android MediaCodec 输出 |
| 音频输入 | `MiniRtcAudioFrame`：48 kHz、单声道、16-bit PCM，每帧 10 ms，即 480 个采样 / 960 字节 |
| 时间戳 | 采集时间使用单调时钟微秒；可用 `GetSystemTimeMicros(peer)`，不要混用 Unix 毫秒 |
| 普通数据 | `AddDataStream(..., false)` 配合 `SendDataFrame` |
| 可靠数据 | `AddDataStream(..., true)` 配合 `SendReliableDataFrame` |

所有发送接口都有相应的 `*ToPeer` 形式用于指定远端。`RequestVideoKeyFrame` / `RequestAllVideoKeyFrames` 用于请求本地发送编码器产生关键帧，不是通用的远端解码恢复命令。

启用原生输出后，`MiniRtcVideoFrame::data` 可以为空，即使 `size` 仍为逻辑 NV12 大小。接收端应先检查 `native_frame`；需要连续 CPU 数据时先检查 `copy_to_nv12` 是否存在，再调用并检查返回值，不能直接解引用 CUDA 地址。Android MediaCodec 输出没有 `copy_to_nv12`，应使用 `payload.android_media_codec.render(owner, ANativeWindow*)`，该操作只消费一次输出缓冲区；此类型不能作为编码输入，编解码器重置后保留的令牌会失效。

**跨回调保留原生帧：** 描述符指针只在当前调用 / 回调中有效。先复制描述符并调用 `retain(owner)`，最后调用 `release(owner)`；仅保存指针或复制 `MiniRtcVideoFrame` 不会延长所有者生命周期。CPU 缓冲区也应在异步使用前复制。输入描述符须正确设置 `struct_size`、尺寸、载荷、所有者与回调，具体校验见 [native_video_frame.h](../src/frame/native_video_frame.h)。

`video_frame_rate` 当前支持 30 / 60，其他值归一化为 60；降级策略可选 `MaintainFrameRate`、`MaintainResolution`、`Balanced`。应用仍负责按合适的频率提交采集帧。

<a id="network"></a>

## 连接与配置

### 会话 ICE 配置

当前客户端上报 `ice_config_version: 1`，由服务端为每次连接下发带有效期的完整 `ice` 列表，替代本地 STUN / TURN 地址。额外或备用节点应配置在服务端的 `CROSSDESK_ICE_SERVERS`。显式空列表表示仅使用本地候选；强制中继模式仍要求列表中存在匹配的 TURN 节点。

协商该能力后，MiniRTC 会拒绝缺失、格式错误或过期的会话配置。兼容旧服务端时可接收未过期的旧 `turn` 凭据；两种有效配置都没有时，连接建立失败。`Params` 的静态地址不会覆盖服务端列表。实现见 [连接配置](../src/pc/peer_connection.cpp) 与 [ICE 数据结构](../src/ice/ice_server_config.h)。

### TURN、UPnP 与虚拟网卡

| `turn_mode` | 行为 |
| --- | --- |
| `TurnDisabled` | 仅直连 / STUN 候选 |
| `TurnAutoUdpTcp` | 直连候选，加 TURN UDP / TCP 回退 |
| `TurnForceUdp` | 仅 TURN UDP 中继 |
| `TurnForceTcp` | 仅 TURN TCP 中继 |

- 与当前 CrossDesk Server 配合时，登录和协商消息会携带临时 TURN 凭据。库收到时校验 `host`、`port`、`username`、`password`、`expires_at`，更新后续连接配置；无需把 `COTURN_AUTH_SECRET` 写入应用。
- 示例参数将静态 TURN 地址与用户名 / 密码留空，使用服务端下发值。需要中继时，服务端必须正确配置 Coturn、公布可达地址并签发有效凭据；`TurnAutoUdpTcp` 本身不会部署中继服务。
- 桌面 libnice 构建启用 UPnP，可在非强制中继模式尝试网关映射。iOS、Android 和 HarmonyOS 配方禁用 UPnP；实际映射能力还取决于网关。
- 匹配的 libnice 补丁允许受支持的原生对端在使用中继后继续尝试升级到 P2P；强制中继模式不执行这种升级。不要将其理解为所有对端都能无条件切回直连。
- 默认忽略若干虚拟接口前缀，包括 Docker、TUN / TAP、WireGuard、Tailscale、ZeroTier。需要通过这些接口收集候选时，以 `--MINIRTC_INCLUDE_VIRTUAL_ICE_INTERFACES=true` 重新配置并构建。列表与补丁见 [libnice 配方](../thirdparty/libnice/xmake.lua)。

### TLS 与 INI 配置

信令通过 WSS 连接，证书验证使用系统信任能力。自签服务端需要在运行应用的设备上信任根证书；排查证书有效期与主机名 / IP，证书错误通过 `SignalTlsCertError` 报告。服务部署和端口说明见 [CrossDesk Server](https://github.com/kunkundi/crossdesk-server)。

`use_cfg_file=true` 时，从 `cfg_path` 指向的 INI 读取服务器与媒体配置，直接填写的对应 `Params` 字段不会作为逐项回退值。`cfg_path` 是固定字符数组，应复制路径内容；回调、`user_id`、`user_data` 仍由 `Params` 提供。

[config/config.ini](../config/config.ini) 是历史配置示例，包含旧地址和静态 TURN 凭据，不能直接作为当前部署配置使用。INI 缺省值与 `Params{}` 的零值也不同；支持的字段与解析规则以 [PeerConnection::Init](../src/pc/peer_connection.cpp) 为准。

<a id="validation"></a>

## 排查与验证

| 现象 | 检查方向 |
| --- | --- |
| `Init` 返回成功但未登录 | 等待信令状态回调，检查 WSS 地址、服务可达性及证书 |
| 登录时崩溃 | 检查三个必需回调是否有效，以及上下文生命周期 |
| 可登录但无法建立会话 | 检查双方服务、远端 ID / 密码、TURN 凭据、UDP / TCP 与中继端口 |
| 启用原生输出后黑屏 | 不要只读取 `data`；检查帧类型、stride、所有者生命周期及渲染路径 |
| 音频帧被拒绝 | 确认是 48 kHz 单声道 16-bit PCM，且每次提交恰好 10 ms |
| CUDA 编译或链接失败 | 检查平台 / 架构、SDK 路径、驱动、依赖与最终应用的链接设置 |
| UPnP / libnice 符号或扩展缺失 | 检查是否使用当前包配方与补丁，避免混用旧缓存或另一套 GLib / GUPnP |

当前 [xmake.lua](../xmake.lua) 构建静态库及对象目标，没有为 `tests/` 注册可直接运行的测试目标。尤其 [旧 PeerConnection 示例](../tests/peerconnection/host.cpp) 仍使用已移除的接口，不应作为当前 API 教程或直接宣称能够运行。

验证集成时，至少构建目标平台并完成两端登录、直连 / 强制 TURN、媒体收发、数据收发与退出重连。公开头文件的编译检查不能替代这些网络和媒体测试。实际应用可参考 CrossDesk 的 [桌面集成](https://github.com/kunkundi/crossdesk/blob/HEAD/apps/desktop/src/gui/runtime/gui_runtime.cpp) 和 [iOS 桥接](https://github.com/kunkundi/crossdesk/blob/HEAD/apps/ios/CrossDeskMobile/Bridge/CrossDeskRTCBridge.h)。
