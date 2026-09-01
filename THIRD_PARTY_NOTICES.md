# Third-party notices

MiniRTC's original source is licensed under `LGPL-3.0-only`; see `LICENSE`.
Third-party terms continue to apply to their respective code.

| Bundled code | Location | Terms |
| --- | --- | --- |
| WebRTC-derived code | `src/common`, `src/qos`, selected RTP/RTCP files, `h264_bitstream_parser.*` | `thirdparty/webrtc/LICENSE` and `thirdparty/webrtc/PATENTS` |
| inih | `src/inih` | `src/inih/LICENSE` |
| NVIDIA interface headers | `thirdparty/nvcodec/interface` | `thirdparty/nvcodec/interface/LICENSE` |
| NVIDIA SDK samples | `src/media/nvcodec` | Redistribution must be verified against the EULA supplied with the exact SDK version |

External runtime dependencies are declared in `xmake.lua` and `thirdparty/`.
MiniRTC publishes source only; packages downloaded by xmake are not copied into
this repository, so their license texts are not duplicated here.

Binary products embedding MiniRTC, including CrossDesk, must provide the
applicable dependency licenses and notices at the product release layer and
satisfy LGPL/MPL source-availability and relinking obligations.

The NVIDIA sample files explicitly refer to an associated SDK EULA, but their
SDK version is not recorded. Their source redistribution permission must be
confirmed before the repository can be treated as license-complete. Codec
patent rights also require separate product review.
