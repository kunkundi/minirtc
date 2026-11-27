/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-28
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "video_encoder_factory.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <chrono>

#include "wmf_video_encoder.h"
#include "d3d11_capture_feedback.h"

namespace minirtc {
namespace {
using Microsoft::WRL::ComPtr;

bool IsIntelDevice(ID3D11Device* device) {
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC desc{};
  return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
         SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc)) &&
         desc.VendorId == 0x8086;
}

// Negotiation precedes capture. Bind to the actual texture's device on the
// encode queue, including after a display switch. Statistics can read the
// published encoder concurrently without extending a transport lock.
class WindowsCaptureEncoder final : public MediaCodec {
 public:
  WindowsCaptureEncoder(std::shared_ptr<SystemClock> clock, VideoCodecType codec)
      : clock_(std::move(clock)), codec_(codec) {}
  ~WindowsCaptureEncoder() override { ReleaseFeedback(); }

  int Init(const MediaCodecConfig& config) override {
    auto initial = VideoEncoderFactory::CreateWindowsDefaultEncoder(clock_, config, codec_);
    if (!initial) return -1;
    ReleaseFeedback();
    observed_device_.Reset();
    using_intel_ = false;
    retry_default_after_ = {};
    config_ = config;
    input_fps_ = config.max_frame_rate;
    std::atomic_store(&active_, std::shared_ptr<MediaCodec>(std::move(initial)));
    return 0;
  }

  std::pair<int, int> PrepareEncodeResolutionForFrame(
      const RawFrame& raw, int w, int h) override {
    SelectSource(raw, w, h);
    return PrepareEncodeResolution(w, h);
  }

  std::pair<int, int> PrepareEncodeResolution(int w, int h) override {
    auto active = Active();
    if (active) {
      const auto size = active->PrepareEncodeResolution(w, h);
      if (size.first || !using_intel_) return size;
    }
    if (RestoreDefault(w, h)) return Active()->PrepareEncodeResolution(w, h);
    return {};
  }

  int Encode(const RawFrame& raw,
             std::function<int(const EncodedFrame&)> callback) override {
    SelectSource(raw, raw.Width(), raw.Height());
    auto active = Active();
    if (!active) {
      if (!RestoreDefault(raw.Width(), raw.Height())) return -1;
      active = Active();
    }
    if (using_intel_) {
      if (EncodeWith(*active, raw, callback) == 0) return 0;
      if (feedback_) feedback_->RequestCpuFrames();
      if (!RestoreDefault(raw.Width(), raw.Height())) return -1;
      active = Active();
      // Preserve the negotiated codec. If the fallback requires scaling,
      // let the transport scale its next frame with the new limits.
      if (active->PrepareEncodeResolution(raw.Width(), raw.Height()) !=
          std::make_pair(int(raw.Width()), int(raw.Height()))) return -1;
    }
    return EncodeWith(*active, raw, callback);
  }

  int ForceIdr() override {
    auto active = Active();
    return active ? active->ForceIdr() : -1;
  }
  int SetTargetBitrate(int bitrate) override {
    auto active = Active();
    if (bitrate > 0) config_.init_bitrate = ClampEncoderTargetBitrate(bitrate, config_.max_bitrate);
    return active ? active->SetTargetBitrate(bitrate) : -1;
  }
  int SetTargetFrameRate(int fps) override {
    if (fps > 0 && fps <= 240) {
      config_.max_frame_rate = fps;
      input_fps_ = fps;
    }
    auto active = Active();
    return active ? active->SetTargetFrameRate(fps) : -1;
  }
  int SetInputFrameRate(int fps) override {
    if (fps > 0 && fps <= 240) input_fps_ = fps;
    auto active = Active();
    return active ? active->SetInputFrameRate(fps) : -1;
  }
  bool SupportsInputFrameRateUpdates() const override {
    auto active = Active();
    return active && active->SupportsInputFrameRateUpdates();
  }
  bool SupportsNativeFrameInput(MiniRtcNativeVideoFrameType type) const override {
    auto active = Active();
    return active && active->SupportsNativeFrameInput(type);
  }
  VideoResolutionLimits GetEncoderResolutionLimits() const override {
    auto active = Active();
    return active ? active->GetEncoderResolutionLimits() : VideoResolutionLimits{};
  }
  int GetResolution(int* w, int* h) const override {
    auto active = Active();
    return active ? active->GetResolution(w, h) : -1;
  }
  std::string GetEncoderName() const override {
    auto active = Active();
    return active ? active->GetEncoderName() : "Windows hardware encoder";
  }
  bool GetLastEncodeStageTimingUs(int64_t* upload, int64_t* submit) const override {
    auto active = Active();
    return active && active->GetLastEncodeStageTimingUs(upload, submit);
  }

 private:
  std::shared_ptr<MediaCodec> Active() const { return std::atomic_load(&active_); }

  void ReleaseFeedback() {
    if (feedback_) feedback_->EncoderStopped();
    feedback_.Reset();
  }

  static int EncodeWith(MediaCodec& encoder, const RawFrame& raw,
                        const std::function<int(const EncodedFrame&)>& callback) {
    const auto* native = raw.NativeFrame();
    if (native && !encoder.SupportsNativeFrameInput(native->type)) {
      RawFrame cpu(raw);
      if (!cpu.MaterializeNativeFrame()) return -1;
      return encoder.Encode(cpu, callback);
    }
    return encoder.Encode(raw, callback);
  }

  void SelectSource(const RawFrame& raw, int w, int h) {
    const auto* frame = raw.NativeFrame();
    if (w < 2 || h < 2) return;
    if (!frame || frame->type != MiniRtcNativeVideoFrameD3D11Nv12) {
      // Keep the MFT for short cursor-composition gaps. A sustained switch to
      // WGC/GDI or a CPU-only output should return to normal backend selection.
      if (using_intel_ && Clock::now() - last_texture_ >= std::chrono::seconds(1)) {
        observed_device_.Reset();
        RestoreDefault(w, h);
      }
      return;
    }
    last_texture_ = Clock::now();
    auto* texture = static_cast<ID3D11Texture2D*>(frame->payload.d3d11_nv12.texture);
    ComPtr<ID3D11Device> device;
    texture->GetDevice(&device);
    if (device.Get() == observed_device_.Get()) return;
    observed_device_ = device;  // Retry a failed probe only on a new capture device.
    if (IsIntelDevice(device.Get())) {
      auto feedback = D3D11CaptureFeedback::Find(device.Get());
      auto candidate = std::make_shared<WmfVideoEncoder>(clock_, codec_, device.Get());
      auto config = config_;
      config.init_width = w;
      config.init_height = h;
      if (candidate->Init(config) == 0) {
        candidate->SetInputFrameRate(input_fps_);
        ReleaseFeedback();
        feedback_ = std::move(feedback);
        if (feedback_) feedback_->EncoderStarted();
        std::atomic_store(&active_, std::shared_ptr<MediaCodec>(std::move(candidate)));
        using_intel_ = true;
        LOG_INFO("Using capture-adapter Intel Quick Sync encoder");
        return;
      }
      if (feedback) feedback->RequestCpuFrames();
      LOG_WARN("Capture-adapter Quick Sync unavailable; using the default Windows encoder");
    }
    if (using_intel_) RestoreDefault(w, h);
  }

  bool RestoreDefault(int w, int h) {
    ReleaseFeedback();
    using_intel_ = false;
    if (!Active() && Clock::now() < retry_default_after_) return false;
    auto config = config_;
    config.init_width = w;
    config.init_height = h;
    auto next = VideoEncoderFactory::CreateWindowsDefaultEncoder(clock_, config, codec_);
    if (next) {
      next->SetInputFrameRate(input_fps_);
      next->ForceIdr();
      LOG_INFO("Restoring Windows encoder [{}]", next->GetEncoderName());
    } else {
      retry_default_after_ = Clock::now() + std::chrono::seconds(1);
    }
    std::atomic_store(&active_, std::shared_ptr<MediaCodec>(std::move(next)));
    return Active() != nullptr;
  }

  const std::shared_ptr<SystemClock> clock_;
  MediaCodecConfig config_;
  const VideoCodecType codec_;
  std::shared_ptr<MediaCodec> active_;
  ComPtr<ID3D11Device> observed_device_;
  ComPtr<D3D11CaptureFeedback> feedback_;
  int input_fps_ = 0;
  bool using_intel_ = false;
  using Clock = std::chrono::steady_clock;
  Clock::time_point last_texture_{}, retry_default_after_{};
};
}  // namespace

std::unique_ptr<MediaCodec> VideoEncoderFactory::CreateWindowsCaptureEncoder(
    std::shared_ptr<SystemClock> clock, const MediaCodecConfig& config, VideoCodecType codec) {
  return TryInitEncoder(std::make_unique<WindowsCaptureEncoder>(std::move(clock), codec), config);
}
}  // namespace minirtc
