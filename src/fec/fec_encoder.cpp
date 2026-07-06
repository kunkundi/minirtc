#include "fec_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "log.h"

namespace minirtc {

namespace {

constexpr of_codec_id_t kFecCodecId = OF_CODEC_REED_SOLOMON_GF_2_M_STABLE;
constexpr int kFecVerbosity = 0;

of_parameters_t* CreateRsParams(const FecBlockConfig& config) {
  auto* rs_params =
      static_cast<of_rs_2_m_parameters_t*>(
          calloc(1, sizeof(of_rs_2_m_parameters_t)));
  if (!rs_params) {
    return nullptr;
  }

  rs_params->m = 8;
  auto* params = reinterpret_cast<of_parameters_t*>(rs_params);
  params->nb_source_symbols = config.source_symbol_count;
  params->nb_repair_symbols = config.repair_symbol_count;
  params->encoding_symbol_length = config.symbol_size;
  return params;
}

void ReleaseRsParams(of_parameters_t* params) {
  if (params) {
    free(params);
  }
}

}  // namespace

FecBlockEncoder::FecBlockEncoder(const FecBlockConfig& config)
    : config_(config) {
  if (!config_.IsValid()) {
    throw std::invalid_argument("invalid FEC block config");
  }
}

uint16_t FecBlockEncoder::RepairSymbolsForRatio(uint16_t source_symbol_count,
                                                double repair_ratio) {
  if (source_symbol_count == 0 || repair_ratio <= 0.0) {
    return 0;
  }
  const auto repair_count = static_cast<uint16_t>(
      std::ceil(static_cast<double>(source_symbol_count) * repair_ratio));
  return std::max<uint16_t>(1, repair_count);
}

std::vector<FecSymbol> FecBlockEncoder::Encode(
    const std::vector<uint8_t>& payload) {
  return Encode(payload.data(), payload.size());
}

std::vector<FecSymbol> FecBlockEncoder::Encode(const uint8_t* payload,
                                               size_t payload_size) {
  if (!payload || payload_size != config_.original_size) {
    throw std::invalid_argument("payload size does not match FEC config");
  }

  of_session_t* session = nullptr;
  of_parameters_t* params = CreateRsParams(config_);
  if (!params) {
    throw std::runtime_error("failed to allocate FEC parameters");
  }

  if (OF_STATUS_OK !=
      of_create_codec_instance(&session, kFecCodecId, OF_ENCODER,
                               kFecVerbosity)) {
    ReleaseRsParams(params);
    throw std::runtime_error("failed to create FEC encoder");
  }

  if (OF_STATUS_OK != of_set_fec_parameters(session, params)) {
    of_release_codec_instance(session);
    ReleaseRsParams(params);
    throw std::runtime_error("failed to set FEC parameters");
  }

  std::vector<std::vector<uint8_t>> raw_symbols(config_.total_symbol_count());
  std::vector<void*> symbol_ptrs(config_.total_symbol_count(), nullptr);

  for (uint16_t esi = 0; esi < config_.source_symbol_count; ++esi) {
    raw_symbols[esi].assign(config_.symbol_size, 0);
    const size_t offset = static_cast<size_t>(esi) * config_.symbol_size;
    const size_t remaining = payload_size > offset ? payload_size - offset : 0;
    const size_t copy_size =
        std::min<size_t>(remaining, config_.symbol_size);
    if (copy_size > 0) {
      memcpy(raw_symbols[esi].data(), payload + offset, copy_size);
    }
    symbol_ptrs[esi] = raw_symbols[esi].data();
  }

  for (uint16_t esi = config_.source_symbol_count;
       esi < config_.total_symbol_count(); ++esi) {
    raw_symbols[esi].assign(config_.symbol_size, 0);
    symbol_ptrs[esi] = raw_symbols[esi].data();
    if (OF_STATUS_OK != of_build_repair_symbol(session, symbol_ptrs.data(),
                                               esi)) {
      of_release_codec_instance(session);
      ReleaseRsParams(params);
      throw std::runtime_error("failed to build FEC repair symbol");
    }
  }

  std::vector<FecSymbol> symbols;
  symbols.reserve(config_.total_symbol_count());
  for (uint16_t esi = 0; esi < config_.total_symbol_count(); ++esi) {
    FecSymbol symbol;
    symbol.symbol_id = esi;
    symbol.is_repair = esi >= config_.source_symbol_count;
    symbol.data = std::move(raw_symbols[esi]);
    symbols.push_back(std::move(symbol));
  }

  of_release_codec_instance(session);
  ReleaseRsParams(params);
  return symbols;
}

FecEncoder::FecEncoder() {}

FecEncoder::~FecEncoder() {}

int FecEncoder::Init() {
  fec_codec_id_ = OF_CODEC_REED_SOLOMON_GF_2_M_STABLE;

  fec_rs_params_ = (of_rs_2_m_parameters_t *)calloc(1, sizeof(*fec_rs_params_));
  if (nullptr == fec_rs_params_) {
    LOG_ERROR("Create FEC codec params failed");
    return -1;
  }

  fec_rs_params_->m = 8;
  fec_params_ = (of_parameters_t *)fec_rs_params_;

  if (OF_STATUS_OK !=
      of_create_codec_instance(&fec_session_, fec_codec_id_, OF_ENCODER, 2)) {
    LOG_ERROR("Create FEC codec instance failed");
    return -1;
  }

  return 0;
}

int FecEncoder::Release() {
  if (!fec_session_) {
    LOG_ERROR("Invalid FEC codec instance");
    return -1;
  }

  {
    if (OF_STATUS_OK != of_release_codec_instance(fec_session_)) {
      LOG_ERROR("Release FEC codec instance failed");
      return -1;
    }
  }

  if (fec_rs_params_) {
    free(fec_rs_params_);
  }

  return 0;
}

uint8_t **FecEncoder::Encode(const char *data, size_t len) {
  uint8_t **fec_packets = nullptr;

  unsigned int last_packet_size = len % max_size_of_packet_;
  uint8_t num_of_source_packets =
      (uint8_t)(len / max_size_of_packet_) + (last_packet_size ? 1 : 0);
  uint8_t num_of_total_packets =
      (uint8_t)floor((double)num_of_source_packets / code_rate_);

  fec_params_->nb_source_symbols = num_of_source_packets;
  fec_params_->nb_repair_symbols = num_of_total_packets - num_of_source_packets;

  fec_params_->encoding_symbol_length = max_size_of_packet_;

  if (OF_STATUS_OK != of_set_fec_parameters(fec_session_, fec_params_)) {
    LOG_ERROR("Set FEC params failed for codec_id {}", (int)fec_codec_id_);
    return nullptr;
  }

  fec_packets = (uint8_t **)calloc(num_of_total_packets, sizeof(uint8_t *));

  if (nullptr == fec_packets) {
    LOG_ERROR("Calloc failed for fec_packets with size [{}])",
              num_of_total_packets);
    return nullptr;
  }

  for (int esi = 0; esi < num_of_source_packets; esi++) {
    if (esi != (num_of_source_packets - 1)) {
      fec_packets[esi] =
          (uint8_t *)calloc(max_size_of_packet_, sizeof(uint8_t));
      if (nullptr == fec_packets[esi]) {
        LOG_ERROR("Calloc failed for fec_packets[{}] with size [{}])", esi,
                  max_size_of_packet_);
        ReleaseFecPackets(fec_packets, len);
        return nullptr;
      }
      memcpy(fec_packets[esi], data + esi * max_size_of_packet_,
             max_size_of_packet_);
    } else {
      fec_packets[esi] =
          (uint8_t *)calloc(max_size_of_packet_, sizeof(uint8_t));
      if (nullptr == fec_packets[esi]) {
        LOG_ERROR("Calloc failed for fec_packets[{}] with size [{}])", esi,
                  last_packet_size);
        ReleaseFecPackets(fec_packets, len);
        return nullptr;
      }
      memcpy(fec_packets[esi], data + esi * max_size_of_packet_,
             last_packet_size);
    }
  }

  for (unsigned int esi = num_of_source_packets; esi < num_of_total_packets;
       esi++) {
    fec_packets[esi] = (uint8_t *)calloc(max_size_of_packet_, sizeof(uint8_t));
    if (nullptr == fec_packets[esi]) {
      LOG_ERROR("Calloc failed for fec_packets[{}] with size [{}])", esi,
                max_size_of_packet_);
      ReleaseFecPackets(fec_packets, len);
      return nullptr;
    }
    if (OF_STATUS_OK !=
        of_build_repair_symbol(fec_session_, (void **)fec_packets, esi)) {
      LOG_ERROR("Build repair symbols failed for esi [{}]", esi);
      ReleaseFecPackets(fec_packets, len);
      return nullptr;
    }
  }

  return fec_packets;
}

int FecEncoder::ReleaseFecPackets(uint8_t **fec_packets, size_t len) {
  if (nullptr == fec_packets) {
    LOG_ERROR("Release Fec packets failed, due to fec_packets is nullptr");
    return -1;
  }
  unsigned int last_packet_size = len % max_size_of_packet_;
  uint8_t num_of_source_packets =
      (uint8_t)(len / max_size_of_packet_) + (last_packet_size ? 1 : 0);
  uint8_t num_of_total_packets =
      (uint8_t)floor((double)num_of_source_packets / code_rate_);

  for (int esi = 0; esi < num_of_total_packets; esi++) {
    if (fec_packets[esi]) {
      free(fec_packets[esi]);
    }
  }
  free(fec_packets);

  return 0;
}

void FecEncoder::GetFecPacketsParams(unsigned int source_length,
                                     uint8_t &num_of_total_packets,
                                     uint8_t &num_of_source_packets,
                                     unsigned int &last_packet_size) {
  last_packet_size = source_length % max_size_of_packet_;
  num_of_source_packets = (uint8_t)(source_length / max_size_of_packet_) +
                          (last_packet_size ? 1 : 0);
  num_of_total_packets =
      (uint8_t)floor((double)num_of_source_packets / code_rate_);
}
}
