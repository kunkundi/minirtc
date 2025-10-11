# MiniRTC

[中文](README_CN.md) / [English](README.md)

**Lightweight cross-platform real-time audio and video transmission library** designed for P2P communication. MiniRTC provides complete audio/video encoding/decoding, secure transmission, and network optimization. It is already stably used in the open-source remote desktop project [CrossDesk](https://github.com/kunkundi/crossdesk.git).  

[License: LGPL-3.0](LICENSE) | [Platform: Windows | Linux | macOS] | [Codec: AV1 | H.264 | Opus]  

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