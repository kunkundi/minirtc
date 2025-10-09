# MiniRTC

[English](README.md) / [中文](README_CN.md)

**轻量级跨平台实时音视频传输库**，专为 P2P 通信设计，低延迟、高性能、安全可靠。MiniRTC 提供完整的音视频编解码、加密传输和网络优化方案，并已在开源远程桌面项目 [CrossDesk](https://github.com/kunkundi/crossdesk.git) 中稳定应用。  

[License: MIT](LICENSE) | [Platform: Windows | Linux | macOS] | [Codec: AV1 | H.264 | Opus]  

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
