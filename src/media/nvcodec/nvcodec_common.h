/*
 * @Author: DI JUNKUN
 * @Date: 2024-09-10
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _NVCODEC_COMMON_H_
#define _NVCODEC_COMMON_H_

#include <cuda.h>

#include <iostream>
#include <memory>
#include <mutex>
#include <optional>

#include "NvCodecUtils.h"
#include "NvEncoderCLIOptions.h"
#include "NvEncoderCuda.h"
#include "log.h"
#include "nvcodec_api.h"

namespace minirtc {

class CudaInitializer {
 public:
  static bool Init() {
    static bool initialized = []() {
      CUresult res = cuInit_ld(0);
      if (res != CUDA_SUCCESS) {
        return false;
      }
      return true;
    }();
    return initialized;
  }
};

void ShowHelpAndExit(const char *szBadOption = NULL);

void ParseCommandLine(int argc, char *argv[], char *szInputFileName,
                      int &nWidth, int &nHeight, NV_ENC_BUFFER_FORMAT &eFormat,
                      char *szOutputFileName, NvEncoderInitParam &initParam,
                      int &iGpu, int &iCase, int &nFrame);

bool CheckIsCudaEncodeSupported();

bool CheckIsCudaDecodeSupported();

}  // namespace minirtc
#endif