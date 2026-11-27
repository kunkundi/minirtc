#include "cv_pixel_buffer_frame.h"

#include <climits>
#include <cstring>

#include "native_video_frame.h"

namespace minirtc {

SdrColor PixelColor(CVPixelBufferRef buffer) {
  CFTypeRef matrix = CVBufferCopyAttachment(buffer, kCVImageBufferYCbCrMatrixKey, nullptr);
  const bool bt709 = matrix && CFEqual(matrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
  if (matrix) CFRelease(matrix);
  return {CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
          bt709};
}

static void RetainPixelBufferOwner(void* owner) {
  if (owner) {
    CVPixelBufferRetain(static_cast<CVPixelBufferRef>(owner));
  }
}

static void ReleasePixelBufferOwner(void* owner) {
  if (owner) {
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(owner));
  }
}

int CopyPixelBufferToNv12(void* owner, uint8_t* destination,
                          size_t destination_size) {
  auto pixel_buffer = static_cast<CVPixelBufferRef>(owner);
  const OSType pixel_format =
      pixel_buffer ? CVPixelBufferGetPixelFormatType(pixel_buffer) : 0;
  if (!pixel_buffer || !destination ||
      !CVPixelBufferIsPlanar(pixel_buffer) ||
      CVPixelBufferGetPlaneCount(pixel_buffer) < 2 ||
      (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
       pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
    return -1;
  }
  const size_t width = CVPixelBufferGetWidth(pixel_buffer);
  const size_t height = CVPixelBufferGetHeight(pixel_buffer);
  size_t required_size;
  if (width > INT_MAX || height > INT_MAX ||
      !GetNv12FrameSize(width, height, &required_size) || destination_size < required_size ||
      CVPixelBufferLockBaseAddress(pixel_buffer,
                                   kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
    return -1;
  }

  const auto* y_plane = static_cast<const uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  const auto* uv_plane = static_cast<const uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
  const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);
  int result = 0;
  if (!y_plane || !uv_plane || y_stride < width || uv_stride < width) {
    result = -1;
  } else {
    for (size_t row = 0; row < height; ++row) {
      memcpy(destination + row * width, y_plane + row * y_stride, width);
    }
    uint8_t* uv_destination = destination + width * height;
    for (size_t row = 0; row < height / 2U; ++row) {
      memcpy(uv_destination + row * width, uv_plane + row * uv_stride, width);
    }
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  if (result == 0) PixelColor(pixel_buffer).Normalize(destination, width, height);
  return result;
}

MiniRtcNativeVideoFrame DescribePixelBuffer(CVPixelBufferRef pixel_buffer) {
  MiniRtcNativeVideoFrame native_frame{};
  native_frame.struct_size = sizeof(native_frame);
  native_frame.type = MiniRtcNativeVideoFrameCVPixelBuffer;
  native_frame.width = static_cast<uint32_t>(CVPixelBufferGetWidth(pixel_buffer));
  native_frame.height = static_cast<uint32_t>(CVPixelBufferGetHeight(pixel_buffer));
  native_frame.payload.cv_pixel_buffer = pixel_buffer;
  native_frame.owner = pixel_buffer;
  native_frame.retain = &RetainPixelBufferOwner;
  native_frame.release = &ReleasePixelBufferOwner;
  native_frame.copy_to_nv12 = &CopyPixelBufferToNv12;
  return native_frame;
}

}  // namespace minirtc
