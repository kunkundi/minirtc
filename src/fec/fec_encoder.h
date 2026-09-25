/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-13
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _FEC_ENCODER_H_
#define _FEC_ENCODER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace minirtc {
using FecSymbols = std::vector<std::vector<uint8_t>>;

// A bounded RS(255) block. Instances are confined to their calling thread.
// All output is owned by the caller. Small live blocks use cached coefficients;
// larger blocks fall back to OpenFEC. Neither path retains caller memory.
class FecEncoder {
 public:
  static constexpr size_t kMaxSymbols = 255;
  static constexpr size_t kMaxSymbolSize = 1400;
  bool EncodeSymbols(const FecSymbols& sources, size_t repair_count,
                     FecSymbols* repairs) const;

  // Legacy offline API. Network code must use the sized, owning API above.
  int Init() { return 0; }
  int Release() { return 0; }
  uint8_t** Encode(const char* data, size_t len);
  int ReleaseFecPackets(uint8_t** packets, size_t len);
  void GetFecPacketsParams(unsigned int len, uint8_t& total, uint8_t& source,
                           unsigned int& last_size);
};
}  // namespace minirtc
#endif
