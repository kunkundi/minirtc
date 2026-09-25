#include "fec_encoder.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "openfec_session.h"
#include "rs_small.h"

extern "C" {
#include "lib_common/of_openfec_api.h"
}

namespace minirtc {
namespace {
size_t LegacyTotal(size_t k) { return k * 1000 / 667; }
size_t LegacyCount(size_t len) {
  if (!len || len > FecEncoder::kMaxSymbols * FecEncoder::kMaxSymbolSize)
    return 0;
  const size_t k =
      (len + FecEncoder::kMaxSymbolSize - 1) / FecEncoder::kMaxSymbolSize;
  return LegacyTotal(k) <= FecEncoder::kMaxSymbols ? k : 0;
}
}  // namespace

bool FecEncoder::EncodeSymbols(const FecSymbols& sources, size_t repair_count,
                               FecSymbols* repairs) const {
  if (!repairs || repairs == &sources) return false;
  const auto invalid = [&] {
    repairs->clear();
    return false;
  };
  const size_t k = sources.size();
  if (!k || k > kMaxSymbols || repair_count > kMaxSymbols - k) return invalid();
  const size_t size = sources.front().size();
  if (!size || size > kMaxSymbolSize) return invalid();
  for (const auto& source : sources)
    if (source.size() != size) return invalid();
  if (!repair_count) {
    repairs->clear();
    return true;
  }
  if (SupportsSmallRs(k, repair_count)) {
    if (!EncodeSmallRs(sources, repair_count, repairs)) return invalid();
    return true;
  }
  repairs->clear();

  std::lock_guard<std::mutex> lock(OpenFecMutex());
  of_session_t* raw = nullptr;
  if (of_create_codec_instance(&raw, OF_CODEC_REED_SOLOMON_GF_2_M_STABLE,
                               OF_ENCODER, 0) != OF_STATUS_OK) {
    if (raw) of_release_codec_instance(raw);
    return false;
  }
  std::unique_ptr<of_session_t, decltype(&of_release_codec_instance)> session(
      raw, of_release_codec_instance);
  of_rs_2_m_parameters_t params{};
  params.m = 8;
  params.nb_source_symbols = static_cast<uint32_t>(k);
  params.nb_repair_symbols = static_cast<uint32_t>(repair_count);
  params.encoding_symbol_length = static_cast<uint32_t>(size);
  if (of_set_fec_parameters(raw, reinterpret_cast<of_parameters_t*>(&params)) !=
      OF_STATUS_OK)
    return false;

  FecSymbols result(repair_count, std::vector<uint8_t>(size));
  std::vector<void*> pointers(k + repair_count);
  for (size_t i = 0; i < k; ++i)
    pointers[i] = const_cast<uint8_t*>(sources[i].data());
  for (size_t i = 0; i < repair_count; ++i) {
    pointers[k + i] = result[i].data();
    if (of_build_repair_symbol(raw, pointers.data(),
                               static_cast<uint32_t>(k + i)) != OF_STATUS_OK)
      return false;
  }
  *repairs = std::move(result);
  return true;
}

uint8_t** FecEncoder::Encode(const char* data, size_t len) {
  const size_t k = LegacyCount(len);
  if (!data || !k) return nullptr;
  const size_t n = LegacyTotal(k);
  FecSymbols sources(k, std::vector<uint8_t>(kMaxSymbolSize));
  for (size_t i = 0; i < k; ++i)
    std::memcpy(sources[i].data(), data + i * kMaxSymbolSize,
                std::min(kMaxSymbolSize, len - i * kMaxSymbolSize));
  FecSymbols repairs;
  if (!EncodeSymbols(sources, n - k, &repairs)) return nullptr;
  auto** result = static_cast<uint8_t**>(std::calloc(n, sizeof(uint8_t*)));
  if (!result) return nullptr;
  for (size_t i = 0; i < n; ++i) {
    result[i] = static_cast<uint8_t*>(std::malloc(kMaxSymbolSize));
    if (!result[i]) {
      ReleaseFecPackets(result, len);
      return nullptr;
    }
    std::memcpy(result[i], i < k ? sources[i].data() : repairs[i - k].data(),
                kMaxSymbolSize);
  }
  return result;
}

int FecEncoder::ReleaseFecPackets(uint8_t** packets, size_t len) {
  if (!packets) return 0;
  const size_t k = LegacyCount(len);
  if (!k) return -1;
  for (size_t i = 0; i < LegacyTotal(k); ++i) std::free(packets[i]);
  std::free(packets);
  return 0;
}

void FecEncoder::GetFecPacketsParams(unsigned int len, uint8_t& total,
                                     uint8_t& source, unsigned int& last_size) {
  const size_t k = LegacyCount(len);
  source = static_cast<uint8_t>(k);
  total = static_cast<uint8_t>(LegacyTotal(k));
  last_size = k ? static_cast<unsigned int>(len - (k - 1) * kMaxSymbolSize) : 0;
}
}  // namespace minirtc
