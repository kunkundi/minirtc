#include "minirtc.h"
#ifdef __ANDROID__
#include "../media/mediacodec/android_codec_support.h"
#endif

int SetMiniRtcAndroidJavaVm(void* java_vm) {
#ifdef __ANDROID__
  minirtc::android::SetJavaVm(static_cast<JavaVM*>(java_vm));
  return 0;
#else
  (void)java_vm;
  return -1;
#endif
}
