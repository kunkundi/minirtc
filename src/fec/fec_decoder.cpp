#include "fec_decoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace minirtc {

namespace {

constexpr int kFecVerbosity = 0;

}  // namespace

FecBlockDecoder::FecBlockDecoder(const FecBlockConfig& config)
    : config_(config) {
  initialized_ = InitSession();
}

FecBlockDecoder::~FecBlockDecoder() { ReleaseSession(); }

bool FecBlockDecoder::InitSession() {
  if (!config_.IsValid()) {
    return false;
  }

  fec_rs_params_ =
      static_cast<of_rs_2_m_parameters_t*>(calloc(1, sizeof(*fec_rs_params_)));
  if (!fec_rs_params_) {
    return false;
  }

  fec_rs_params_->m = 8;
  fec_params_ = reinterpret_cast<of_parameters_t*>(fec_rs_params_);
  fec_params_->nb_source_symbols = config_.source_symbol_count;
  fec_params_->nb_repair_symbols = config_.repair_symbol_count;
  fec_params_->encoding_symbol_length = config_.symbol_size;

  if (OF_STATUS_OK !=
      of_create_codec_instance(&fec_session_, fec_codec_id_, OF_DECODER,
                               kFecVerbosity)) {
    ReleaseSession();
    return false;
  }

  if (OF_STATUS_OK != of_set_fec_parameters(fec_session_, fec_params_)) {
    ReleaseSession();
    return false;
  }

  return true;
}

void FecBlockDecoder::ReleaseSession() {
  if (fec_session_) {
    of_release_codec_instance(fec_session_);
    fec_session_ = nullptr;
  }

  if (fec_rs_params_) {
    free(fec_rs_params_);
    fec_rs_params_ = nullptr;
    fec_params_ = nullptr;
  }

  initialized_ = false;
}

FecDecodeStatus FecBlockDecoder::AddSymbol(const FecSymbol& symbol) {
  if (complete_) {
    return FecDecodeStatus::kComplete;
  }

  if (!initialized_ || symbol.symbol_id >= config_.total_symbol_count() ||
      symbol.data.size() != config_.symbol_size) {
    return FecDecodeStatus::kError;
  }

  if (received_symbol_ids_.find(symbol.symbol_id) !=
      received_symbol_ids_.end()) {
    return FecDecodeStatus::kDuplicate;
  }

  auto inserted =
      received_symbols_.emplace(symbol.symbol_id, symbol.data);
  if (!inserted.second) {
    return FecDecodeStatus::kDuplicate;
  }
  received_symbol_ids_.insert(symbol.symbol_id);

  if (OF_STATUS_ERROR == of_decode_with_new_symbol(
                             fec_session_, inserted.first->second.data(),
                             symbol.symbol_id)) {
    return FecDecodeStatus::kError;
  }

  if (received_symbols_.size() >= config_.source_symbol_count &&
      of_is_decoding_complete(fec_session_)) {
    complete_ = BuildDecodedData();
    return complete_ ? FecDecodeStatus::kComplete : FecDecodeStatus::kError;
  }

  return FecDecodeStatus::kAccepted;
}

bool FecBlockDecoder::BuildDecodedData() {
  std::vector<void*> source_symbols(config_.source_symbol_count, nullptr);
  if (OF_STATUS_OK !=
      of_get_source_symbols_tab(fec_session_, source_symbols.data())) {
    return false;
  }

  decoded_data_.clear();
  decoded_data_.reserve(config_.original_size);

  for (uint16_t esi = 0; esi < config_.source_symbol_count; ++esi) {
    if (!source_symbols[esi]) {
      return false;
    }

    const size_t already_copied = decoded_data_.size();
    const size_t remaining = config_.original_size > already_copied
                                 ? config_.original_size - already_copied
                                 : 0;
    const size_t copy_size = std::min<size_t>(remaining, config_.symbol_size);
    const auto* source_data = static_cast<const uint8_t*>(source_symbols[esi]);
    decoded_data_.insert(decoded_data_.end(), source_data,
                         source_data + copy_size);

    auto received = received_symbols_.find(esi);
    const bool owned_source =
        received != received_symbols_.end() &&
        received->second.data() == source_symbols[esi];
    if (!owned_source) {
      free(source_symbols[esi]);
    }
  }

  return decoded_data_.size() == config_.original_size;
}

FecDecoder::FecDecoder() {}

FecDecoder::~FecDecoder() {}

int FecDecoder::Init() {
  fec_codec_id_ = OF_CODEC_REED_SOLOMON_GF_2_M_STABLE;

  fec_rs_params_ = (of_rs_2_m_parameters_t *)calloc(1, sizeof(*fec_rs_params_));
  if (nullptr == fec_rs_params_) {
    LOG_ERROR("Create FEC decoder params failed");
    return -1;
  }

  fec_rs_params_->m = 8;
  fec_params_ = (of_parameters_t *)fec_rs_params_;

  if (OF_STATUS_OK !=
      of_create_codec_instance(&fec_session_, fec_codec_id_, OF_DECODER, 2)) {
    LOG_ERROR("Create FEC decoder instance failed");
    return -1;
  }

  return 0;
}

int FecDecoder::Release() {
  if (!fec_session_) {
    LOG_ERROR("Invalid FEC decoder instance");
    return -1;
  }

  {
    if (OF_STATUS_OK != of_release_codec_instance(fec_session_)) {
      LOG_ERROR("Release FEC decoder instance failed");
      return -1;
    }
  }

  if (fec_rs_params_) {
    free(fec_rs_params_);
  }

  return 0;
}

int FecDecoder::ResetParams(unsigned int source_symbol_num) {
  if (!fec_session_) {
    LOG_ERROR("Invalid FEC decoder instance");
    return -1;
  }

  num_of_received_symbols_ = 0;
  num_of_source_packets_ = source_symbol_num;
  num_of_total_packets_ =
      (unsigned int)floor((double)source_symbol_num / (double)code_rate_);

  LOG_ERROR("Set s[{}] r[{}]", num_of_source_packets_,
            num_of_total_packets_ - source_symbol_num);

  fec_params_->nb_source_symbols = source_symbol_num;
  fec_params_->nb_repair_symbols = num_of_total_packets_ - source_symbol_num;
  fec_params_->encoding_symbol_length = max_size_of_packet_;

  if (OF_STATUS_OK != of_set_fec_parameters(fec_session_, fec_params_)) {
    LOG_ERROR("Set FEC params failed for codec_id {}", (int)fec_codec_id_);
    return -1;
  }

  return 0;
}

uint8_t **FecDecoder::DecodeWithNewSymbol(const char *fec_symbol,
                                          unsigned int fec_symbol_id) {
  if (!fec_session_) {
    LOG_ERROR("Invalid FEC decoder instance");
    return nullptr;
  }

  num_of_received_symbols_++;
  if (OF_STATUS_ERROR == of_decode_with_new_symbol(
                             fec_session_, (char *)fec_symbol, fec_symbol_id)) {
    LOG_ERROR("Decode wit new symbol failed");
    return nullptr;
  }

  if ((num_of_received_symbols_ >= fec_params_->nb_source_symbols) &&
      (true == of_is_decoding_complete(fec_session_))) {
    uint8_t **source_packets =
        (uint8_t **)calloc(num_of_total_packets_, sizeof(uint8_t *));
    if (!source_packets) {
      LOG_ERROR("Calloc failed for source_packets with size [{}])",
                num_of_total_packets_);
    }

    if (OF_STATUS_OK !=
        of_get_source_symbols_tab(fec_session_, (void **)source_packets)) {
      LOG_ERROR("Get source symbols failed");
      return nullptr;
    }

    return source_packets;
  }

  return nullptr;
}

int FecDecoder::ReleaseSourcePackets(uint8_t **source_packets) {
  if (nullptr == source_packets) {
    LOG_ERROR(
        "Release source packets failed, due to source_packets is nullptr");
    return -1;
  }

  // for (unsigned int index = 0; index < num_of_source_packets_; index++) {
  //   if (source_packets[index]) {
  //     LOG_ERROR("Free [{}]", index);
  //     free(source_packets[index]);
  //   }
  // }
  free(source_packets);

  return 0;
}
}
