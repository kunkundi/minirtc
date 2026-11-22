#include "minirtc.h"
#ifdef __ANDROID__
#include "../media/mediacodec/android_codec_support.h"
#endif

int MiniRtcSetAndroidJavaVm(void* java_vm) {
#ifdef __ANDROID__
  minirtc::android::SetJavaVm(static_cast<JavaVM*>(java_vm));
  return 0;
#else
  (void)java_vm;
  return -1;
#endif
}

bool MiniRtcSupportsH264High10() {
#ifdef MINIRTC_H264_HIGH10
  return true;
#else
  return false;
#endif
}
