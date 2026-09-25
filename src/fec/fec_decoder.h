/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-15
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _FEC_DECODER_H_
#define _FEC_DECODER_H_

#include <memory>

#include "fec_encoder.h"

namespace minirtc {
class FecDecoder {
 public:
  enum class Result { kAccepted, kDuplicate, kComplete, kInvalid, kError };
  FecDecoder();
  ~FecDecoder();
  FecDecoder(const FecDecoder&) = delete;
  FecDecoder& operator=(const FecDecoder&) = delete;

  bool Reset(size_t source_count, size_t repair_count, size_t symbol_size);
  Result AddSymbol(size_t id, const uint8_t* data, size_t size);
  bool Complete() const;
  // Valid until Reset/Release/destruction. Includes the source slots [0, k).
  const FecSymbols& Symbols() const;

  int Init() { return Release(); }
  int Release();
  int ResetParams(unsigned int source_count);
  // Legacy buffers must contain 1400 readable bytes. Returned table borrows
  // this decoder's owned symbols; free only the table before resetting.
  uint8_t** DecodeWithNewSymbol(const char* symbol, unsigned int id);
  int ReleaseSourcePackets(uint8_t** packets);

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace minirtc
#endif