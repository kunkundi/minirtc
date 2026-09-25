/*
 * @Author: DI JUNKUN
 * @Date: 2025-09-25
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _RS_SMALL_H_
#define _RS_SMALL_H_

#include "fec_encoder.h"

namespace minirtc {
// Bounded fast path for the live RTP configuration. Larger offline blocks keep
// using OpenFEC. The repair coefficients are obtained through its public API,
// so rs-v1 retains the exact same systematic code and symbol IDs.
constexpr size_t kRsSmallMaxSources = 16;
constexpr size_t kRsSmallMaxRepairs = 4;
bool SupportsSmallRs(size_t k, size_t r);
// Internal entry points: sizes/IDs/ownership have been validated by the owning
// FecEncoder/FecDecoder wrapper. No caller memory is retained in the cache.
bool EncodeSmallRs(const FecSymbols& sources, size_t r, FecSymbols* repairs);
bool DecodeSmallRs(size_t k, size_t r, size_t size,
                   const std::vector<bool>& seen, FecSymbols* symbols);
}  // namespace minirtc
#endif
