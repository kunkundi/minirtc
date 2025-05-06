#include "nvcodec_api.h"

#ifdef _WIN32
#else
#include <dlfcn.h>
#endif

#include "log.h"

TcuInit cuInit_ld = NULL;
TcuDeviceGet cuDeviceGet_ld = NULL;
TcuDeviceGetCount cuDeviceGetCount_ld = NULL;
TcuCtxCreate cuCtxCreate_ld = NULL;
TcuGetErrorName cuGetErrorName_ld = NULL;
TcuCtxPushCurrent cuCtxPushCurrent_ld = NULL;
TcuCtxPopCurrent cuCtxPopCurrent_ld = NULL;
TcuMemAlloc cuMemAlloc_ld = NULL;
TcuMemAllocPitch cuMemAllocPitch_ld = NULL;
TcuMemFree cuMemFree_ld = NULL;
TcuMemcpy2DAsync cuMemcpy2DAsync_ld = NULL;
TcuStreamSynchronize cuStreamSynchronize_ld = NULL;
TcuMemcpy2D cuMemcpy2D_ld = NULL;
TcuMemcpy2DUnaligned cuMemcpy2DUnaligned_ld = NULL;

TcuvidCtxLockCreate cuvidCtxLockCreate_ld = NULL;
TcuvidGetDecoderCaps cuvidGetDecoderCaps_ld = NULL;
TcuvidCreateDecoder cuvidCreateDecoder_ld = NULL;
TcuvidDestroyDecoder cuvidDestroyDecoder_ld = NULL;
TcuvidDecodePicture cuvidDecodePicture_ld = NULL;
TcuvidGetDecodeStatus cuvidGetDecodeStatus_ld = NULL;
TcuvidReconfigureDecoder cuvidReconfigureDecoder_ld = NULL;
TcuvidMapVideoFrame64 cuvidMapVideoFrame64_ld = NULL;
TcuvidUnmapVideoFrame64 cuvidUnmapVideoFrame64_ld = NULL;
TcuvidCtxLockDestroy cuvidCtxLockDestroy_ld = NULL;
TcuvidCreateVideoParser cuvidCreateVideoParser_ld = NULL;
TcuvidParseVideoData cuvidParseVideoData_ld = NULL;
TcuvidDestroyVideoParser cuvidDestroyVideoParser_ld = NULL;

TNvEncodeAPICreateInstance NvEncodeAPICreateInstance_ld = NULL;
TNvEncodeAPIGetMaxSupportedVersion NvEncodeAPIGetMaxSupportedVersion_ld = NULL;

static void* nvcuda_dll = NULL;
static void* nvcuvid_dll = NULL;
static void* nvEncodeAPI64_dll = NULL;

static int LoadLibraryHelper(void** library, const char* winLib,
                             const char* linuxLib) {
#ifdef _WIN32
  *library = LoadLibrary(TEXT(winLib));
#else
  *library = dlopen(linuxLib, RTLD_LAZY);
#endif
  if (*library == NULL) {
#ifdef _WIN32
    LOG_ERROR("Unable to load library {}", winLib);
#else
    LOG_ERROR("Unable to load library {}", linuxLib);
#endif
    return -1;
  }
  return 0;
}

static void FreeLibraryHelper(void** library) {
  if (*library != NULL) {
#ifdef _WIN32
    FreeLibrary(*library);
#else
    dlclose(*library);
#endif
    *library = NULL;
  }
}

static int LoadFunctionHelper(void* library, void** func,
                              const char* funcName) {
#ifdef _WIN32
  *func = GetProcAddress(library, funcName);
#else
  *func = dlsym(library, funcName);
#endif
  if (*func == NULL) {
    LOG_ERROR("Unable to find function {}", funcName);
    return -1;
  }
  return 0;
}

int LoadNvCodecDll() {
  if (LoadLibraryHelper(&nvcuda_dll, "nvcuda.dll", "libcuda.so") != 0) {
    FreeLibraryHelper(&nvcuda_dll);
    return -1;
  }

  if (LoadFunctionHelper(nvcuda_dll, (void**)&cuInit_ld, "cuInit") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuDeviceGet_ld, "cuDeviceGet") !=
          0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuDeviceGetCount_ld,
                         "cuDeviceGetCount") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuCtxCreate_ld,
                         "cuCtxCreate_v2") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuGetErrorName_ld,
                         "cuGetErrorName") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuCtxPushCurrent_ld,
                         "cuCtxPushCurrent_v2") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuCtxPopCurrent_ld,
                         "cuCtxPopCurrent_v2") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuMemAlloc_ld, "cuMemAlloc_v2") !=
          0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuMemAllocPitch_ld,
                         "cuMemAllocPitch_v2") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuMemFree_ld, "cuMemFree_v2") !=
          0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuMemcpy2DAsync_ld,
                         "cuMemcpy2DAsync_v2") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuStreamSynchronize_ld,
                         "cuStreamSynchronize") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuMemcpy2D_ld, "cuMemcpy2D_v2") !=
          0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuMemcpy2DUnaligned_ld,
                         "cuMemcpy2DUnaligned_v2") != 0) {
    FreeLibraryHelper(&nvcuda_dll);
  }

  if (LoadLibraryHelper(&nvcuvid_dll, "nvcuvid.dll", "libnvcuvid.so") != 0) {
    FreeLibraryHelper(&nvcuvid_dll);
    return -1;
  }

  if (LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidCtxLockCreate_ld,
                         "cuvidCtxLockCreate") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidGetDecoderCaps_ld,
                         "cuvidGetDecoderCaps") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidCreateDecoder_ld,
                         "cuvidCreateDecoder") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidDestroyDecoder_ld,
                         "cuvidDestroyDecoder") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidDecodePicture_ld,
                         "cuvidDecodePicture") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidGetDecodeStatus_ld,
                         "cuvidGetDecodeStatus") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidReconfigureDecoder_ld,
                         "cuvidReconfigureDecoder") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidMapVideoFrame64_ld,
                         "cuvidMapVideoFrame64") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidUnmapVideoFrame64_ld,
                         "cuvidUnmapVideoFrame64") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidCtxLockDestroy_ld,
                         "cuvidCtxLockDestroy") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidCreateVideoParser_ld,
                         "cuvidCreateVideoParser") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidParseVideoData_ld,
                         "cuvidParseVideoData") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidDestroyVideoParser_ld,
                         "cuvidDestroyVideoParser") != 0) {
    FreeLibraryHelper(&nvcuvid_dll);
    return -1;
  }

#ifdef _WIN32
  nvEncodeAPI64_dll = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
  if (nvEncodeAPI64_dll == NULL) {
    LOG_ERROR("Unable to load library nvEncodeAPI64.dll");
    return -1;
  }

  if (LoadFunctionHelper(nvEncodeAPI64_dll,
                         (void**)&NvEncodeAPICreateInstance_ld,
                         "NvEncodeAPICreateInstance") != 0 ||
      LoadFunctionHelper(nvEncodeAPI64_dll,
                         (void**)&NvEncodeAPIGetMaxSupportedVersion_ld,
                         "NvEncodeAPIGetMaxSupportedVersion") != 0) {
    FreeLibraryHelper(&nvEncodeAPI64_dll);
    return -1;
  }
#endif

  LOG_INFO("Load NvCodec API success");

  return 0;
}

int ReleaseNvCodecDll() {
  if (nvcuda_dll != NULL) {
    FreeLibraryHelper(&nvcuda_dll);
  }

  if (nvcuvid_dll != NULL) {
    FreeLibraryHelper(&nvcuvid_dll);
  }

  if (nvEncodeAPI64_dll != NULL) {
    FreeLibraryHelper(&nvEncodeAPI64_dll);
  }

  LOG_INFO("Release NvCodec API success");

  return 0;
}