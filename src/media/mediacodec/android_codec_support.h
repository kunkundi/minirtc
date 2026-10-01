/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-02
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _ANDROID_CODEC_SUPPORT_H_
#define _ANDROID_CODEC_SUPPORT_H_

#include <android/native_window.h>
#include <jni.h>

#include <string>
#include <vector>

#include "media_codec.h"

namespace minirtc {
namespace android {
// Source-only bridge for the embedding JNI library. Set from JNI_OnLoad before
// using codecs; the VM must outlive them. This is not an installed SDK API or an
// exported symbol. A missing VM leaves the software backends available.
__attribute__((visibility("hidden"))) void SetJavaVm(JavaVM* vm);
}  // namespace android

struct AndroidCodecInfo {
  std::string name;
  int color = 0;
  int avc_level = 0;
  bool low_latency = false;
};
// A zero size queries availability; a concrete size also checks video limits.
std::vector<AndroidCodecInfo> FindAndroidCodecs(VideoCodecType type,
                                                bool encoder, int width = 0,
                                                int height = 0);
class AndroidSurface {
 public:
  AndroidSurface();
  ~AndroidSurface();
  ANativeWindow* window = nullptr;

 private:
  jobject texture_ = nullptr;
  jobject surface_ = nullptr;
};
}  // namespace minirtc

#endif