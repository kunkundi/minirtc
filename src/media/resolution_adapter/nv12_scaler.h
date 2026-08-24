#ifndef MINIRTC_NV12_SCALER_H_
#define MINIRTC_NV12_SCALER_H_

#include <cstdint>
#include <vector>

#include "libyuv/scale.h"

namespace minirtc {

// Scales NV12 by separating the interleaved chroma plane before filtering.
// libyuv::NV12Scale filters packed UV samples with a small downward rounding
// bias. Keeping U and V planar preserves neutral chroma (U=V=128).
int ScaleNv12ViaI420(const uint8_t* src_y, int src_stride_y,
                     const uint8_t* src_uv, int src_stride_uv, int src_width,
                     int src_height, uint8_t* dst_y, int dst_stride_y,
                     uint8_t* dst_uv, int dst_stride_uv, int dst_width,
                     int dst_height, libyuv::FilterMode filtering,
                     std::vector<uint8_t>* scratch_buffer);

}  // namespace minirtc

#endif  // MINIRTC_NV12_SCALER_H_
