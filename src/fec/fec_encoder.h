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

#ifdef __cplusplus
extern "C" {
#endif
#include "lib_common/of_openfec_api.h"
#ifdef __cplusplus
};
#endif

namespace minirtc {

struct FecBlockConfig {
  uint16_t source_symbol_count = 0;
  uint16_t repair_symbol_count = 0;
  uint16_t symbol_size = 0;
  uint32_t original_size = 0;

  uint16_t total_symbol_count() const {
    return static_cast<uint16_t>(source_symbol_count + repair_symbol_count);
  }

  bool IsValid() const {
    return source_symbol_count > 0 && repair_symbol_count > 0 &&
           symbol_size > 0 && original_size > 0 &&
           total_symbol_count() <= 255;
  }
};

struct FecSymbol {
  uint16_t symbol_id = 0;
  bool is_repair = false;
  std::vector<uint8_t> data;
};

class FecBlockEncoder {
 public:
  explicit FecBlockEncoder(const FecBlockConfig& config);
  ~FecBlockEncoder() = default;

  std::vector<FecSymbol> Encode(const std::vector<uint8_t>& payload);
  std::vector<FecSymbol> Encode(const uint8_t* payload, size_t payload_size);

  const FecBlockConfig& config() const { return config_; }

  static uint16_t RepairSymbolsForRatio(uint16_t source_symbol_count,
                                        double repair_ratio);

 private:
  FecBlockConfig config_;
};

class FecEncoder {
 public:
  FecEncoder();
  ~FecEncoder();

 public:
  int Init();
  int Release();
  uint8_t **Encode(const char *data, size_t len);
  int ReleaseFecPackets(uint8_t **fec_packets, size_t len);
  void GetFecPacketsParams(unsigned int source_length,
                           uint8_t &num_of_total_packets,
                           uint8_t &num_of_source_packets,
                           unsigned int &last_packet_size);

 private:
  double code_rate_ = 0.667;
  int max_size_of_packet_ = 1400;

 private:
  of_codec_id_t fec_codec_id_ = OF_CODEC_REED_SOLOMON_GF_2_M_STABLE;
  of_session_t *fec_session_ = nullptr;
  of_parameters_t *fec_params_ = nullptr;
  of_rs_2_m_parameters_t *fec_rs_params_ = nullptr;
  of_ldpc_parameters_t *fec_ldpc_params_ = nullptr;
};
}  // namespace minirtc

#endif
