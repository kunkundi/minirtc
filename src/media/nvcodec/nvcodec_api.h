/*
 * @Author: DI JUNKUN
 * @Date: 2024-08-12
 * Copyright (c) 2024 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _NVCODEC_API_H_
#define _NVCODEC_API_H_

#if __WIN32__
#include <Windows.h>
#endif

#include <iostream>

#include "cuda.h"
#include "cuviddec.h"
#include "nvEncodeAPI.h"
#include "nvcuvid.h"

namespace minirtc {

// nvcuda.dll
typedef CUresult (*TcuInit)(unsigned int Flags);

typedef CUresult (*TcuDeviceGet)(CUdevice *device, int ordinal);

typedef CUresult (*TcuDeviceGetCount)(int *count);

typedef CUresult (*TcuCtxCreate)(CUcontext *pctx, unsigned int flags,
                                 CUdevice dev);

typedef CUresult (*TcuCtxDestroy)(CUcontext ctx);

typedef CUresult (*TcuDeviceGetName)(char *name, int len, CUdevice dev);

typedef CUresult (*TcuGetErrorName)(CUresult error, const char **pStr);

typedef CUresult (*TcuCtxPushCurrent)(CUcontext ctx);

typedef CUresult (*TcuCtxPopCurrent)(CUcontext *pctx);

typedef CUresult (*TcuMemAlloc)(CUdeviceptr *dptr, size_t bytesize);

typedef CUresult (*TcuMemAllocPitch)(CUdeviceptr *dptr, size_t *pPitch,
                                     size_t WidthInBytes, size_t Height,
                                     unsigned int ElementSizeBytes);

typedef CUresult (*TcuMemFree)(CUdeviceptr dptr);

typedef CUresult (*TcuMemcpy2DAsync)(const CUDA_MEMCPY2D *pCopy,
                                     CUstream hStream);

typedef CUresult (*TcuStreamSynchronize)(CUstream hStream);

typedef CUresult (*TcuMemcpy2D)(const CUDA_MEMCPY2D *pCopy);

typedef CUresult (*TcuMemcpy2DUnaligned)(const CUDA_MEMCPY2D *pCopy);

extern TcuInit cuInit_ld;
extern TcuDeviceGet cuDeviceGet_ld;
extern TcuDeviceGetCount cuDeviceGetCount_ld;
extern TcuCtxCreate cuCtxCreate_ld;
extern TcuCtxDestroy cuCtxDestroy_ld;
extern TcuDeviceGetName cuDeviceGetName_ld;
extern TcuGetErrorName cuGetErrorName_ld;
extern TcuCtxPushCurrent cuCtxPushCurrent_ld;
extern TcuCtxPopCurrent cuCtxPopCurrent_ld;
extern TcuMemAlloc cuMemAlloc_ld;
extern TcuMemAllocPitch cuMemAllocPitch_ld;
extern TcuMemFree cuMemFree_ld;
extern TcuMemcpy2DAsync cuMemcpy2DAsync_ld;
extern TcuStreamSynchronize cuStreamSynchronize_ld;
extern TcuMemcpy2D cuMemcpy2D_ld;
extern TcuMemcpy2DUnaligned cuMemcpy2DUnaligned_ld;

#if defined(_WIN32)
typedef CUresult(CUDAAPI *TcuGraphicsGLRegisterBuffer)(
    CUgraphicsResource* resource, unsigned int buffer, unsigned int flags);
typedef CUresult(CUDAAPI *TcuGraphicsUnregisterResource)(
    CUgraphicsResource resource);
typedef CUresult(CUDAAPI *TcuGraphicsMapResources)(
    unsigned int count, CUgraphicsResource* resources, CUstream stream);
typedef CUresult(CUDAAPI *TcuGraphicsUnmapResources)(
    unsigned int count, CUgraphicsResource* resources, CUstream stream);
typedef CUresult(CUDAAPI *TcuGraphicsResourceGetMappedPointer)(
    CUdeviceptr* device_pointer, size_t* size, CUgraphicsResource resource);

extern TcuGraphicsGLRegisterBuffer cuGraphicsGLRegisterBuffer_ld;
extern TcuGraphicsUnregisterResource cuGraphicsUnregisterResource_ld;
extern TcuGraphicsMapResources cuGraphicsMapResources_ld;
extern TcuGraphicsUnmapResources cuGraphicsUnmapResources_ld;
extern TcuGraphicsResourceGetMappedPointer
    cuGraphicsResourceGetMappedPointer_ld;

// Loads the optional CUDA/OpenGL interop entry points from the already
// process-lifetime NVIDIA driver module.
int LoadCudaGraphicsInterop();
#endif

// nvcuvid.dll
typedef CUresult (*TcuvidCtxLockCreate)(CUvideoctxlock *pLock, CUcontext ctx);
typedef CUresult (*TcuvidGetDecoderCaps)(CUVIDDECODECAPS *pdc);
typedef CUresult (*TcuvidCreateDecoder)(CUvideodecoder *phDecoder,
                                        CUVIDDECODECREATEINFO *pdci);
typedef CUresult (*TcuvidDestroyDecoder)(CUvideodecoder hDecoder);
typedef CUresult (*TcuvidDecodePicture)(CUvideodecoder hDecoder,
                                        CUVIDPICPARAMS *pPicParams);
typedef CUresult (*TcuvidGetDecodeStatus)(CUvideodecoder hDecoder, int nPicIdx,
                                          CUVIDGETDECODESTATUS *pDecodeStatus);
typedef CUresult (*TcuvidReconfigureDecoder)(
    CUvideodecoder hDecoder, CUVIDRECONFIGUREDECODERINFO *pDecReconfigParams);
typedef CUresult (*TcuvidMapVideoFrame)(CUvideodecoder hDecoder, int nPicIdx,
                                        unsigned long long *pDevPtr,
                                        unsigned int *pPitch,
                                        CUVIDPROCPARAMS *pVPP);
typedef CUresult (*TcuvidUnmapVideoFrame)(CUvideodecoder hDecoder,
                                          unsigned long long DevPtr);
typedef CUresult (*TcuvidCtxLockDestroy)(CUvideoctxlock lck);
typedef CUresult (*TcuvidCreateVideoParser)(CUvideoparser *pObj,
                                            CUVIDPARSERPARAMS *pParams);
typedef CUresult (*TcuvidParseVideoData)(CUvideoparser obj,
                                         CUVIDSOURCEDATAPACKET *pPacket);
typedef CUresult (*TcuvidDestroyVideoParser)(CUvideoparser obj);

extern TcuvidCtxLockCreate cuvidCtxLockCreate_ld;
extern TcuvidGetDecoderCaps cuvidGetDecoderCaps_ld;
extern TcuvidCreateDecoder cuvidCreateDecoder_ld;
extern TcuvidDestroyDecoder cuvidDestroyDecoder_ld;
extern TcuvidDecodePicture cuvidDecodePicture_ld;
extern TcuvidGetDecodeStatus cuvidGetDecodeStatus_ld;
extern TcuvidReconfigureDecoder cuvidReconfigureDecoder_ld;
extern TcuvidMapVideoFrame cuvidMapVideoFrame_ld;
extern TcuvidUnmapVideoFrame cuvidUnmapVideoFrame_ld;
extern TcuvidCtxLockDestroy cuvidCtxLockDestroy_ld;
extern TcuvidCreateVideoParser cuvidCreateVideoParser_ld;
extern TcuvidParseVideoData cuvidParseVideoData_ld;
extern TcuvidDestroyVideoParser cuvidDestroyVideoParser_ld;

// nvEncodeAPI64.dll
typedef NVENCSTATUS (*TNvEncodeAPICreateInstance)(
    NV_ENCODE_API_FUNCTION_LIST *functionList);
typedef NVENCSTATUS (*TNvEncodeAPIGetMaxSupportedVersion)(uint32_t *version);

extern TNvEncodeAPICreateInstance NvEncodeAPICreateInstance_ld;
extern TNvEncodeAPIGetMaxSupportedVersion NvEncodeAPIGetMaxSupportedVersion_ld;

int LoadNvCodecDll();

}  // namespace minirtc
#endif
