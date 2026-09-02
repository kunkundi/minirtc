#include "nvcodec_api.h"

#ifdef _WIN32
#else
#include <dlfcn.h>
#endif

#include <mutex>

#include "log.h"

namespace minirtc {

// Successful loads are process-lifetime. Unloading NVIDIA's driver modules
// while codecs or driver callbacks may still exist leaves cached entry points
// dangling and lets one transport invalidate another.
static bool nvcodec_dll_loaded = false;
static std::mutex nvcodec_dll_mutex;

TcuInit cuInit_ld = NULL;
TcuDeviceGet cuDeviceGet_ld = NULL;
TcuDeviceGetCount cuDeviceGetCount_ld = NULL;
TcuCtxCreate cuCtxCreate_ld = NULL;
TcuCtxDestroy cuCtxDestroy_ld = NULL;
TcuDeviceGetName cuDeviceGetName_ld = NULL;
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

#ifdef _WIN32
TcuGraphicsGLRegisterBuffer cuGraphicsGLRegisterBuffer_ld = NULL;
TcuGraphicsUnregisterResource cuGraphicsUnregisterResource_ld = NULL;
TcuGraphicsMapResources cuGraphicsMapResources_ld = NULL;
TcuGraphicsUnmapResources cuGraphicsUnmapResources_ld = NULL;
TcuGraphicsResourceGetMappedPointer cuGraphicsResourceGetMappedPointer_ld =
    NULL;
#endif

TcuvidCtxLockCreate cuvidCtxLockCreate_ld = NULL;
TcuvidGetDecoderCaps cuvidGetDecoderCaps_ld = NULL;
TcuvidCreateDecoder cuvidCreateDecoder_ld = NULL;
TcuvidDestroyDecoder cuvidDestroyDecoder_ld = NULL;
TcuvidDecodePicture cuvidDecodePicture_ld = NULL;
TcuvidGetDecodeStatus cuvidGetDecodeStatus_ld = NULL;
TcuvidReconfigureDecoder cuvidReconfigureDecoder_ld = NULL;
TcuvidMapVideoFrame cuvidMapVideoFrame_ld = NULL;
TcuvidUnmapVideoFrame cuvidUnmapVideoFrame_ld = NULL;
TcuvidCtxLockDestroy cuvidCtxLockDestroy_ld = NULL;
TcuvidCreateVideoParser cuvidCreateVideoParser_ld = NULL;
TcuvidParseVideoData cuvidParseVideoData_ld = NULL;
TcuvidDestroyVideoParser cuvidDestroyVideoParser_ld = NULL;

TNvEncodeAPICreateInstance NvEncodeAPICreateInstance_ld = NULL;
TNvEncodeAPIGetMaxSupportedVersion NvEncodeAPIGetMaxSupportedVersion_ld = NULL;

#ifdef _WIN32
static HMODULE nvcuda_dll = NULL;
static HMODULE nvcuvid_dll = NULL;
static HMODULE nvencodeapi_dll = NULL;
#else
static void* nvcuda_dll = NULL;
static void* nvcuvid_dll = NULL;
static void* nvencodeapi_dll = NULL;
#endif

static int LoadLibraryHelper(void** library, const char* winLib,
                             const char* linuxLib) {
#ifdef _WIN32
  *library = LoadLibraryA(winLib);
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
    FreeLibrary((HMODULE)*library);
#else
    dlclose(*library);
#endif
    *library = NULL;
  }
}

static void ResetNvCodecFunctions() {
  cuInit_ld = NULL;
  cuDeviceGet_ld = NULL;
  cuDeviceGetCount_ld = NULL;
  cuCtxCreate_ld = NULL;
  cuCtxDestroy_ld = NULL;
  cuDeviceGetName_ld = NULL;
  cuGetErrorName_ld = NULL;
  cuCtxPushCurrent_ld = NULL;
  cuCtxPopCurrent_ld = NULL;
  cuMemAlloc_ld = NULL;
  cuMemAllocPitch_ld = NULL;
  cuMemFree_ld = NULL;
  cuMemcpy2DAsync_ld = NULL;
  cuStreamSynchronize_ld = NULL;
  cuMemcpy2D_ld = NULL;
  cuMemcpy2DUnaligned_ld = NULL;

#ifdef _WIN32
  cuGraphicsGLRegisterBuffer_ld = NULL;
  cuGraphicsUnregisterResource_ld = NULL;
  cuGraphicsMapResources_ld = NULL;
  cuGraphicsUnmapResources_ld = NULL;
  cuGraphicsResourceGetMappedPointer_ld = NULL;
#endif

  cuvidCtxLockCreate_ld = NULL;
  cuvidGetDecoderCaps_ld = NULL;
  cuvidCreateDecoder_ld = NULL;
  cuvidDestroyDecoder_ld = NULL;
  cuvidDecodePicture_ld = NULL;
  cuvidGetDecodeStatus_ld = NULL;
  cuvidReconfigureDecoder_ld = NULL;
  cuvidMapVideoFrame_ld = NULL;
  cuvidUnmapVideoFrame_ld = NULL;
  cuvidCtxLockDestroy_ld = NULL;
  cuvidCreateVideoParser_ld = NULL;
  cuvidParseVideoData_ld = NULL;
  cuvidDestroyVideoParser_ld = NULL;

  NvEncodeAPICreateInstance_ld = NULL;
  NvEncodeAPIGetMaxSupportedVersion_ld = NULL;
}

static void CleanupFailedNvCodecLoad() {
  // A failed load is the only safe time to unload these modules: the success
  // flag has not been published, so no codec can be using their entry points.
  FreeLibraryHelper(reinterpret_cast<void**>(&nvencodeapi_dll));
  FreeLibraryHelper(reinterpret_cast<void**>(&nvcuvid_dll));
  FreeLibraryHelper(reinterpret_cast<void**>(&nvcuda_dll));
  ResetNvCodecFunctions();
  nvcodec_dll_loaded = false;
}

static int LoadFunctionHelper(void* library, void** func,
                              const char* funcName) {
#ifdef _WIN32
  *func = GetProcAddress((HMODULE)library, funcName);
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
  std::lock_guard<std::mutex> lock(nvcodec_dll_mutex);

  if (nvcodec_dll_loaded) {
    return 0;
  }

  if (LoadLibraryHelper(reinterpret_cast<void**>(&nvcuda_dll), "nvcuda.dll",
                        "libcuda.so") != 0) {
    CleanupFailedNvCodecLoad();
    return -1;
  }

  if (LoadFunctionHelper(nvcuda_dll, (void**)&cuInit_ld, "cuInit") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuDeviceGet_ld, "cuDeviceGet") !=
          0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuDeviceGetCount_ld,
                         "cuDeviceGetCount") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuCtxCreate_ld,
                         "cuCtxCreate_v2") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuCtxDestroy_ld,
                         "cuCtxDestroy_v2") != 0 ||
      LoadFunctionHelper(nvcuda_dll, (void**)&cuDeviceGetName_ld,
                         "cuDeviceGetName") != 0 ||
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
    CleanupFailedNvCodecLoad();
    return -1;
  }

  if (LoadLibraryHelper(reinterpret_cast<void**>(&nvcuvid_dll), "nvcuvid.dll",
                        "libnvcuvid.so") != 0) {
    CleanupFailedNvCodecLoad();
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
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidMapVideoFrame_ld,
                         "cuvidMapVideoFrame64") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidUnmapVideoFrame_ld,
                         "cuvidUnmapVideoFrame64") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidCtxLockDestroy_ld,
                         "cuvidCtxLockDestroy") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidCreateVideoParser_ld,
                         "cuvidCreateVideoParser") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidParseVideoData_ld,
                         "cuvidParseVideoData") != 0 ||
      LoadFunctionHelper(nvcuvid_dll, (void**)&cuvidDestroyVideoParser_ld,
                         "cuvidDestroyVideoParser") != 0) {
    CleanupFailedNvCodecLoad();
    return -1;
  }

  if (LoadLibraryHelper(reinterpret_cast<void**>(&nvencodeapi_dll),
                        "nvEncodeAPI64.dll", "libnvidia-encode.so") != 0) {
    CleanupFailedNvCodecLoad();
    return -1;
  }

  if (LoadFunctionHelper(nvencodeapi_dll, (void**)&NvEncodeAPICreateInstance_ld,
                         "NvEncodeAPICreateInstance") != 0 ||
      LoadFunctionHelper(nvencodeapi_dll,
                         (void**)&NvEncodeAPIGetMaxSupportedVersion_ld,
                         "NvEncodeAPIGetMaxSupportedVersion") != 0) {
    CleanupFailedNvCodecLoad();
    return -1;
  }

  LOG_INFO("Load NvCodec API success");

  nvcodec_dll_loaded = true;

  return 0;
}

#ifdef _WIN32
int LoadCudaGraphicsInterop() {
  if (LoadNvCodecDll() != 0) {
    return -1;
  }

  std::lock_guard<std::mutex> lock(nvcodec_dll_mutex);
  if (cuGraphicsGLRegisterBuffer_ld && cuGraphicsUnregisterResource_ld &&
      cuGraphicsMapResources_ld && cuGraphicsUnmapResources_ld &&
      cuGraphicsResourceGetMappedPointer_ld) {
    return 0;
  }

  if (LoadFunctionHelper(nvcuda_dll,
                         reinterpret_cast<void**>(
                             &cuGraphicsGLRegisterBuffer_ld),
                         "cuGraphicsGLRegisterBuffer") != 0 ||
      LoadFunctionHelper(nvcuda_dll,
                         reinterpret_cast<void**>(
                             &cuGraphicsUnregisterResource_ld),
                         "cuGraphicsUnregisterResource") != 0 ||
      LoadFunctionHelper(
          nvcuda_dll, reinterpret_cast<void**>(&cuGraphicsMapResources_ld),
          "cuGraphicsMapResources") != 0 ||
      LoadFunctionHelper(
          nvcuda_dll, reinterpret_cast<void**>(&cuGraphicsUnmapResources_ld),
          "cuGraphicsUnmapResources") != 0 ||
      LoadFunctionHelper(nvcuda_dll,
                         reinterpret_cast<void**>(
                             &cuGraphicsResourceGetMappedPointer_ld),
                         "cuGraphicsResourceGetMappedPointer_v2") != 0) {
    cuGraphicsGLRegisterBuffer_ld = NULL;
    cuGraphicsUnregisterResource_ld = NULL;
    cuGraphicsMapResources_ld = NULL;
    cuGraphicsUnmapResources_ld = NULL;
    cuGraphicsResourceGetMappedPointer_ld = NULL;
    return -1;
  }

  LOG_INFO("Load CUDA/OpenGL interop API success");
  return 0;
}
#endif
}  // namespace minirtc
