#include "nvcodec_common.h"

void ShowHelpAndExit(const char *szBadOption) {
  std::ostringstream oss;
  bool bThrowError = false;
  if (szBadOption) {
    oss << "Error parsing \"" << szBadOption << "\"" << std::endl;
    bThrowError = true;
  }
  oss << "Options:" << std::endl
      << "-i           Input file path" << std::endl
      << "-o           Output file path" << std::endl
      << "-s           Input resolution in this form: WxH" << std::endl
      << "-if          Input format: iyuv nv12" << std::endl
      << "-gpu         Ordinal of GPU to use" << std::endl
      << "-case        0: Encode frames with dynamic bitrate change"
      << std::endl
      << "             1: Encode frames with dynamic resolution change"
      << std::endl;
  oss << NvEncoderInitParam("", nullptr, true).GetHelpMessage() << std::endl;
  if (bThrowError) {
    throw std::invalid_argument(oss.str());
  } else {
    std::cout << oss.str();
    exit(0);
  }
}

void ParseCommandLine(int argc, char *argv[], char *szInputFileName,
                      int &nWidth, int &nHeight, NV_ENC_BUFFER_FORMAT &eFormat,
                      char *szOutputFileName, NvEncoderInitParam &initParam,
                      int &iGpu, int &iCase, int &nFrame) {
  std::ostringstream oss;
  int i;
  for (i = 1; i < argc; i++) {
    if (!_stricmp(argv[i], "-h")) {
      ShowHelpAndExit();
    }
    if (!_stricmp(argv[i], "-i")) {
      if (++i == argc) {
        ShowHelpAndExit("-i");
      }
      sprintf(szInputFileName, "%s", argv[i]);
      continue;
    }
    if (!_stricmp(argv[i], "-o")) {
      if (++i == argc) {
        ShowHelpAndExit("-o");
      }
      sprintf(szOutputFileName, "%s", argv[i]);
      continue;
    }
    if (!_stricmp(argv[i], "-s")) {
      if (++i == argc || 2 != sscanf(argv[i], "%dx%d", &nWidth, &nHeight)) {
        ShowHelpAndExit("-s");
      }
      continue;
    }

    std::vector<std::string> vszFileFormatName = {"iyuv", "nv12"};

    NV_ENC_BUFFER_FORMAT aFormat[] = {
        NV_ENC_BUFFER_FORMAT_IYUV,
        NV_ENC_BUFFER_FORMAT_NV12,
    };

    if (!_stricmp(argv[i], "-if")) {
      if (++i == argc) {
        ShowHelpAndExit("-if");
      }
      auto it = std::find(vszFileFormatName.begin(), vszFileFormatName.end(),
                          argv[i]);
      if (it == vszFileFormatName.end()) {
        ShowHelpAndExit("-if");
      }
      eFormat = aFormat[it - vszFileFormatName.begin()];
      continue;
    }
    if (!_stricmp(argv[i], "-gpu")) {
      if (++i == argc) {
        ShowHelpAndExit("-gpu");
      }
      iGpu = atoi(argv[i]);
      continue;
    }
    if (!_stricmp(argv[i], "-case")) {
      if (++i == argc) {
        ShowHelpAndExit("-case");
      }
      iCase = atoi(argv[i]);
      continue;
    }
    if (!_stricmp(argv[i], "-frame")) {
      if (++i == argc) {
        ShowHelpAndExit("-frame");
      }
      nFrame = atoi(argv[i]);
      continue;
    }
    // Regard as encoder parameter
    if (argv[i][0] != '-') {
      ShowHelpAndExit(argv[i]);
    }
    oss << argv[i] << " ";
    while (i + 1 < argc && argv[i + 1][0] != '-') {
      oss << argv[++i] << " ";
    }
  }
  initParam = NvEncoderInitParam(oss.str().c_str(), nullptr, true);
}

bool CheckIsCudaEncodeSupported() {
  static std::optional<bool> is_hardware_acceleration_supported;
  static std::mutex mtx_encoder;

  std::lock_guard<std::mutex> lock(mtx_encoder);
  if (is_hardware_acceleration_supported.has_value()) {
    return is_hardware_acceleration_supported.value();
  }

  if (!CudaInitializer::Init()) {
    LOG_WARN(
        "System not support hardware accelerated encode, use default software "
        "encoder");
    is_hardware_acceleration_supported = false;
    return false;
  }

  NV_ENCODE_API_FUNCTION_LIST functionList = {NV_ENCODE_API_FUNCTION_LIST_VER};
  NVENCSTATUS nvEncStatus = NvEncodeAPICreateInstance(&functionList);
  if (nvEncStatus != NV_ENC_SUCCESS) {
    is_hardware_acceleration_supported = false;
    return false;
  }

  is_hardware_acceleration_supported = true;
  return true;
}

bool CheckIsCudaDecodeSupported() {
  static std::optional<bool> cached_result;
  static std::mutex mtx_decoder;

  std::lock_guard<std::mutex> lock(mtx_decoder);
  if (cached_result.has_value()) {
    return cached_result.value();
  }

  CUresult cuResult;

  if (!CudaInitializer::Init()) {
    LOG_WARN(
        "System not support hardware accelerated decode, use default software "
        "decoder");
    cached_result = false;
    return false;
  }

  int deviceCount = 0;
  cuResult = cuDeviceGetCount(&deviceCount);
  if (cuResult != CUDA_SUCCESS || deviceCount == 0) {
    LOG_ERROR("No CUDA devices found");
    cached_result = false;
    return false;
  }

  CUdevice cuDevice;
  cuResult = cuDeviceGet(&cuDevice, 0);
  if (cuResult != CUDA_SUCCESS) {
    LOG_ERROR("cuDeviceGet failed");
    cached_result = false;
    return false;
  }

  CUcontext context;
  cuResult = cuCtxCreate(&context, 0, cuDevice);
  if (cuResult != CUDA_SUCCESS) {
    LOG_ERROR("cuCtxCreate failed");
    cached_result = false;
    return false;
  }

  return true;
}