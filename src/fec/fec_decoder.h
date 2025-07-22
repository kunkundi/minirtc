/*
 * @Author: DI JUNKUN
 * @Date: 2023-11-15
 * Copyright (c) 2023 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _FEC_DECODER_H_
#define _FEC_DECODER_H_

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif
#include "lib_common/of_openfec_api.h"
#ifdef __cplusplus
};
#endif

namespace minirtc {
class FecDecoder {
 public:
  FecDecoder();
  ~FecDecoder();

 public:
  int Init();
  int Release();
  int ResetParams(unsigned int source_symbol_num);
  uint8_t **DecodeWithNewSymbol(const char *fec_symbol,
                                unsigned int fec_symbol_id);
  int ReleaseSourcePackets(uint8_t **source_packets);

 private:
  double code_rate_ = 0.667;
  int max_size_of_packet_ = 1400;

 private:
  of_codec_id_t fec_codec_id_ = OF_CODEC_REED_SOLOMON_GF_2_M_STABLE;
  of_session_t *fec_session_ = nullptr;
  of_parameters_t *fec_params_ = nullptr;
  of_rs_2_m_parameters_t *fec_rs_params_ = nullptr;
  of_ldpc_parameters_t *fec_ldpc_params_ = nullptr;

  unsigned int num_of_received_symbols_ = 0;
  unsigned int num_of_source_packets_ = 0;
  unsigned int num_of_total_packets_ = 0;
};
}  // namespace minirtc

#endif