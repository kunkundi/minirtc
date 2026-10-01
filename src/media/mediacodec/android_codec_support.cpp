#include "android_codec_support.h"

#include <android/api-level.h>
#include <android/native_window_jni.h>

#include <algorithm>
#include <atomic>
#include <cctype>

namespace {
std::atomic<JavaVM*> java_vm{nullptr};
struct Env {
  JavaVM* vm = java_vm.load();
  JNIEnv* e = nullptr;
  bool attached = false;
  bool frame = false;
  Env() {
    if (!vm) return;
    if (vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK)
      attached = vm->AttachCurrentThread(&e, nullptr) == JNI_OK;
    if (e) frame = e->PushLocalFrame(128) == JNI_OK;
  }
  bool Ok() {
    if (!e || !frame) return false;
    if (!e->ExceptionCheck()) return true;
    e->ExceptionClear();
    return false;
  }
  ~Env() {
    if (e && e->ExceptionCheck()) e->ExceptionClear();
    if (frame) e->PopLocalFrame(nullptr);
    if (attached) vm->DetachCurrentThread();
  }
};
std::string String(JNIEnv* e, jstring s) {
  if (!s) return {};
  const char* chars = e->GetStringUTFChars(s, nullptr);
  std::string result = chars ? chars : "";
  if (chars) e->ReleaseStringUTFChars(s, chars);
  return result;
}
bool SoftwareName(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  // This Unisoc component reports hardwareAccelerated=true but loads
  // libomx_av1dec_sw_sprd.so / libdav1d.so (Retroid Pocket 2S, Android 11).
  return name == "omx.sprd.av1.decoder" || name.find("omx.google.") == 0 ||
         name.find("c2.android.") == 0 || name.find("c2.google.") == 0 ||
         name.find(".sw.") != std::string::npos ||
         name.find(".sw_") != std::string::npos ||
         name.find(".software") != std::string::npos ||
         name.find(".ffmpeg.") != std::string::npos;
}
}  // namespace

namespace minirtc {
void android::SetJavaVm(JavaVM* vm) { java_vm.store(vm); }

std::vector<AndroidCodecInfo> FindAndroidCodecs(VideoCodecType type,
                                                bool encoder, int width,
                                                int height) {
  std::vector<AndroidCodecInfo> result;
  if (type != VideoCodecType::H264 && type != VideoCodecType::AV1)
    return result;
  Env env;
  if (!env.Ok()) return result;
  auto* e = env.e;
  auto list_class = e->FindClass("android/media/MediaCodecList");
  auto info_class = e->FindClass("android/media/MediaCodecInfo");
  auto caps_class =
      e->FindClass("android/media/MediaCodecInfo$CodecCapabilities");
  auto profile_class =
      e->FindClass("android/media/MediaCodecInfo$CodecProfileLevel");
  auto video_class =
      e->FindClass("android/media/MediaCodecInfo$VideoCapabilities");
  if (!env.Ok()) return result;
  auto ctor = e->GetMethodID(list_class, "<init>", "(I)V");
  auto infos_id = e->GetMethodID(list_class, "getCodecInfos",
                                 "()[Landroid/media/MediaCodecInfo;");
  auto name_id = e->GetMethodID(info_class, "getName", "()Ljava/lang/String;");
  auto encoder_id = e->GetMethodID(info_class, "isEncoder", "()Z");
  auto caps_id = e->GetMethodID(
      info_class, "getCapabilitiesForType",
      "(Ljava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;");
  auto types_id =
      e->GetMethodID(info_class, "getSupportedTypes", "()[Ljava/lang/String;");
  auto feature_id =
      e->GetMethodID(caps_class, "isFeatureSupported", "(Ljava/lang/String;)Z");
  auto required_id =
      e->GetMethodID(caps_class, "isFeatureRequired", "(Ljava/lang/String;)Z");
  auto video_id =
      e->GetMethodID(caps_class, "getVideoCapabilities",
                     "()Landroid/media/MediaCodecInfo$VideoCapabilities;");
  auto size_id = e->GetMethodID(video_class, "isSizeSupported", "(II)Z");
  auto width_alignment =
      e->GetMethodID(video_class, "getWidthAlignment", "()I");
  auto height_alignment =
      e->GetMethodID(video_class, "getHeightAlignment", "()I");
  auto colors_id = e->GetFieldID(caps_class, "colorFormats", "[I");
  auto profile_id = e->GetFieldID(profile_class, "profile", "I");
  auto level_id = e->GetFieldID(profile_class, "level", "I");
  auto profiles_id =
      e->GetFieldID(caps_class, "profileLevels",
                    "[Landroid/media/MediaCodecInfo$CodecProfileLevel;");
  jmethodID hw_id = nullptr, sw_id = nullptr, alias_id = nullptr;
  if (android_get_device_api_level() >= 29) {
    hw_id = e->GetMethodID(info_class, "isHardwareAccelerated", "()Z");
    sw_id = e->GetMethodID(info_class, "isSoftwareOnly", "()Z");
    alias_id = e->GetMethodID(info_class, "isAlias", "()Z");
  }
  if (!env.Ok()) return result;
  auto list = e->NewObject(list_class, ctor, 1 /* ALL_CODECS */);
  if (!list || !env.Ok()) return result;
  auto infos = static_cast<jobjectArray>(e->CallObjectMethod(list, infos_id));
  if (!infos || !env.Ok()) return result;
  const std::string mime =
      type == VideoCodecType::H264 ? "video/avc" : "video/av01";
  for (int i = 0; i < e->GetArrayLength(infos); ++i) {
    if (e->PushLocalFrame(32) != JNI_OK) break;
    [&] {
      auto info = e->GetObjectArrayElement(infos, i);
      if (bool(e->CallBooleanMethod(info, encoder_id)) != encoder) return;
      auto name =
          String(e, static_cast<jstring>(e->CallObjectMethod(info, name_id)));
      if (SoftwareName(name) || name.find(".secure") != std::string::npos)
        return;
      if (hw_id && (!e->CallBooleanMethod(info, hw_id) ||
                    e->CallBooleanMethod(info, sw_id) ||
                    e->CallBooleanMethod(info, alias_id)))
        return;
      auto types =
          static_cast<jobjectArray>(e->CallObjectMethod(info, types_id));
      if (!env.Ok() || !types) return;
      bool supported = false;
      for (int t = 0; t < e->GetArrayLength(types); ++t) {
        auto s = static_cast<jstring>(e->GetObjectArrayElement(types, t));
        supported |= String(e, s) == mime;
        e->DeleteLocalRef(s);
      }
      if (!supported) return;
      auto caps =
          e->CallObjectMethod(info, caps_id, e->NewStringUTF(mime.c_str()));
      if (!env.Ok() || !caps) return;
      for (const char* feature : {"secure-playback", "tunneled-playback"})
        if (e->CallBooleanMethod(caps, required_id, e->NewStringUTF(feature)))
          return;
      auto profiles =
          static_cast<jobjectArray>(e->GetObjectField(caps, profiles_id));
      if (type == VideoCodecType::AV1 &&
          (!profiles || !e->GetArrayLength(profiles)))
        return;
      int avc_level = 0;
      if (encoder && type == VideoCodecType::H264 && profiles) {
        for (int p = 0; p < e->GetArrayLength(profiles); ++p) {
          auto profile = e->GetObjectArrayElement(profiles, p);
          if (e->GetIntField(profile, profile_id) == 1)
            avc_level = std::max(avc_level, e->GetIntField(profile, level_id));
          e->DeleteLocalRef(profile);
        }
        if (!avc_level) return;
      }
      if (width > 0 && height > 0) {
        auto video = e->CallObjectMethod(caps, video_id);
        if (!video) return;
        if (!e->CallBooleanMethod(video, size_id, width, height)) {
          // Some OMX encoders advertise macroblock alignment even though
          // configure accepts visible dimensions and writes SPS cropping.
          const int wa = e->CallIntMethod(video, width_alignment);
          const int ha = e->CallIntMethod(video, height_alignment);
          if (!encoder || wa <= 0 || ha <= 0 || wa > 128 || ha > 128 ||
              !e->CallBooleanMethod(video, size_id,
                                    ((width + wa - 1) / wa) * wa,
                                    ((height + ha - 1) / ha) * ha))
            return;
        }
      }
      int color = 0;
      auto colors = static_cast<jintArray>(e->GetObjectField(caps, colors_id));
      if (!env.Ok() || !colors) return;
      std::vector<jint> values(e->GetArrayLength(colors));
      e->GetIntArrayRegion(colors, 0, values.size(), values.data());
      // ByteBuffer input/output has a defined layout only for these formats.
      for (int candidate : {21 /* NV12 */, 19 /* I420 */}) {
        if (std::find(values.begin(), values.end(), candidate) !=
            values.end()) {
          color = candidate;
          break;
        }
      }
      if (encoder && !color) return;
      bool low_latency = e->CallBooleanMethod(caps, feature_id,
                                              e->NewStringUTF("low-latency"));
      if (env.Ok()) result.push_back({name, color, avc_level, low_latency});
    }();
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->PopLocalFrame(nullptr);
  }
  return result;
}

AndroidSurface::AndroidSurface() {
  Env env;
  if (!env.Ok()) return;
  auto* e = env.e;
  auto tc = e->FindClass("android/graphics/SurfaceTexture");
  auto sc = e->FindClass("android/view/Surface");
  if (!env.Ok()) return;
  auto ctor = e->GetMethodID(tc, "<init>", "(Z)V");
  auto surface_ctor =
      e->GetMethodID(sc, "<init>", "(Landroid/graphics/SurfaceTexture;)V");
  if (!env.Ok()) return;
  auto texture = e->NewObject(tc, ctor, JNI_FALSE);
  if (!env.Ok() || !texture) return;
  texture_ = e->NewGlobalRef(texture);
  auto surface = e->NewObject(sc, surface_ctor, texture);
  if (!env.Ok() || !surface) return;
  surface_ = e->NewGlobalRef(surface);
  window = ANativeWindow_fromSurface(e, surface);
}
AndroidSurface::~AndroidSurface() {
  if (window) ANativeWindow_release(window);
  Env env;
  if (!env.Ok()) return;
  for (auto object : {surface_, texture_}) {
    if (!object) continue;
    auto cls = env.e->GetObjectClass(object);
    auto release = env.e->GetMethodID(cls, "release", "()V");
    if (release) env.e->CallVoidMethod(object, release);
    env.Ok();
    env.e->DeleteGlobalRef(object);
  }
}
}  // namespace minirtc
