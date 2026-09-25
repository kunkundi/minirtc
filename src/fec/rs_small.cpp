#include "rs_small.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

#include "openfec_session.h"

#if defined(__aarch64__) && !defined(MINIRTC_FEC_FORCE_SCALAR)
#include <arm_neon.h>
#define MINIRTC_RS_NEON 1
#endif

extern "C" {
#include "lib_common/of_openfec_api.h"
}

namespace minirtc {
namespace {
// OpenFEC's m=8 code uses GF(256) with primitive polynomial 0x11d.
// Immutable after C++ thread-safe initialization; there is no per-stream table.
struct Field {
  std::array<std::array<uint8_t, 256>, 256> multiply{};
  std::array<uint8_t, 256> inverse{};
  alignas(16) std::array<std::array<uint8_t, 16>, 256> low{}, high{};
  Field() {
    for (unsigned a = 0; a < 256; ++a) {
      for (unsigned b = 0; b < 256; ++b) {
        unsigned x = a, y = b, product = 0;
        for (int bit = 0; bit < 8; ++bit) {
          if (y & 1) product ^= x;
          x <<= 1;
          if (x & 256) x ^= 0x11d;
          y >>= 1;
        }
        multiply[a][b] = uint8_t(product);
        if (product == 1) inverse[a] = uint8_t(b);
      }
      for (unsigned nibble = 0; nibble < 16; ++nibble) {
        low[a][nibble] = multiply[a][nibble];
        high[a][nibble] = multiply[a][nibble << 4];
      }
    }
  }
};
const Field& Gf() {
  static const Field field;
  return field;
}

// dst ^= coefficient * src. Unaligned loads are supported; the scalar tail
// handles short/odd lengths without reading past the authenticated packet.
void MultiplyAdd(uint8_t* dst, const uint8_t* src, size_t size,
                 uint8_t coefficient, const Field& gf) {
  if (!coefficient) return;
  size_t i = 0;
  if (coefficient == 1) {
#if defined(MINIRTC_RS_NEON)
    for (; i + 16 <= size; i += 16)
      vst1q_u8(dst + i, veorq_u8(vld1q_u8(dst + i), vld1q_u8(src + i)));
#endif
    for (; i < size; ++i) dst[i] ^= src[i];
    return;
  }
#if defined(MINIRTC_RS_NEON)
  const auto low = vld1q_u8(gf.low[coefficient].data());
  const auto high = vld1q_u8(gf.high[coefficient].data());
  const auto mask = vdupq_n_u8(15);
  for (; i + 16 <= size; i += 16) {
    const auto value = vld1q_u8(src + i);
    const auto product = veorq_u8(vqtbl1q_u8(low, vandq_u8(value, mask)),
                                  vqtbl1q_u8(high, vshrq_n_u8(value, 4)));
    vst1q_u8(dst + i, veorq_u8(vld1q_u8(dst + i), product));
  }
#endif
  const auto& table = gf.multiply[coefficient];
  // Group independent lookups and XOR a word at a time on scalar targets.
  // memcpy keeps unaligned accesses and strict aliasing valid; constructing
  // the product as bytes also works on either host byte order.
  for (; i + 8 <= size; i += 8) {
    const uint8_t products[8] = {table[src[i]],     table[src[i + 1]],
                                 table[src[i + 2]], table[src[i + 3]],
                                 table[src[i + 4]], table[src[i + 5]],
                                 table[src[i + 6]], table[src[i + 7]]};
    uint64_t value, product;
    std::memcpy(&value, dst + i, sizeof(value));
    std::memcpy(&product, products, sizeof(product));
    value ^= product;
    std::memcpy(dst + i, &value, sizeof(value));
  }
  for (; i < size; ++i) dst[i] ^= table[src[i]];
}

void Scale(uint8_t* dst, size_t size, uint8_t coefficient, const Field& gf) {
  if (coefficient == 1) return;
  size_t i = 0;
#if defined(MINIRTC_RS_NEON)
  const auto low = vld1q_u8(gf.low[coefficient].data());
  const auto high = vld1q_u8(gf.high[coefficient].data());
  const auto mask = vdupq_n_u8(15);
  for (; i + 16 <= size; i += 16) {
    const auto value = vld1q_u8(dst + i);
    vst1q_u8(dst + i, veorq_u8(vqtbl1q_u8(low, vandq_u8(value, mask)),
                               vqtbl1q_u8(high, vshrq_n_u8(value, 4))));
  }
#endif
  const auto& table = gf.multiply[coefficient];
  for (; i + 8 <= size; i += 8) {
    const uint8_t products[8] = {table[dst[i]],     table[dst[i + 1]],
                                 table[dst[i + 2]], table[dst[i + 3]],
                                 table[dst[i + 4]], table[dst[i + 5]],
                                 table[dst[i + 6]], table[dst[i + 7]]};
    std::memcpy(dst + i, products, sizeof(products));
  }
  for (; i < size; ++i) dst[i] = table[dst[i]];
}

struct Coefficients {
  std::mutex initialize;
  std::atomic<bool> valid{false};
  std::array<std::array<uint8_t, kRsSmallMaxSources>, kRsSmallMaxRepairs>
      rows{};
};

const Coefficients* Matrix(size_t k, size_t r) {
  // Fixed 64-entry cache. Neither packet lengths nor attacker-controlled block
  // IDs create cache entries. Warm calls do not enter the OpenFEC mutex.
  static Coefficients cache[kRsSmallMaxSources][kRsSmallMaxRepairs];
  auto& entry = cache[k - 1][r - 1];
  if (entry.valid.load(std::memory_order_acquire)) return &entry;
  std::lock_guard<std::mutex> initialize(entry.initialize);
  if (entry.valid.load(std::memory_order_relaxed)) return &entry;
  {
    std::lock_guard<std::mutex> lock(OpenFecMutex());
    of_session_t* raw = nullptr;
    if (of_create_codec_instance(&raw, OF_CODEC_REED_SOLOMON_GF_2_M_STABLE,
                                 OF_ENCODER, 0) != OF_STATUS_OK) {
      if (raw) of_release_codec_instance(raw);
      return nullptr;
    }
    std::unique_ptr<of_session_t, decltype(&of_release_codec_instance)> session(
        raw, of_release_codec_instance);
    of_rs_2_m_parameters_t params{};
    params.m = 8;
    params.nb_source_symbols = static_cast<uint32_t>(k);
    params.nb_repair_symbols = static_cast<uint32_t>(r);
    params.encoding_symbol_length = kRsSmallMaxSources;
    if (of_set_fec_parameters(
            raw, reinterpret_cast<of_parameters_t*>(&params)) != OF_STATUS_OK)
      return nullptr;
    // Encoding the identity basis exposes G's repair rows without relying on
    // private OpenFEC structures or guessing its Vandermonde construction.
    std::array<std::array<uint8_t, kRsSmallMaxSources>, kRsSmallMaxSources>
        basis{};
    std::array<void*, kRsSmallMaxSources + kRsSmallMaxRepairs> pointers{};
    for (size_t i = 0; i < k; ++i) {
      basis[i][i] = 1;
      pointers[i] = basis[i].data();
    }
    for (size_t i = 0; i < r; ++i) {
      pointers[k + i] = entry.rows[i].data();
      if (of_build_repair_symbol(raw, pointers.data(),
                                 static_cast<uint32_t>(k + i)) != OF_STATUS_OK)
        return nullptr;
    }
  }
  // Publish only a fully built matrix. A transient allocation/library failure
  // above leaves this entry retryable, rather than poisoning it for the
  // process.
  entry.valid.store(true, std::memory_order_release);
  return &entry;
}
}  // namespace

bool SupportsSmallRs(size_t k, size_t r) {
#if defined(MINIRTC_FEC_FORCE_OPENFEC)
  (void)k;
  (void)r;
  return false;  // Reference backend for reproducible tests/benchmarks.
#else
  return k && k <= kRsSmallMaxSources && r && r <= kRsSmallMaxRepairs;
#endif
}

bool EncodeSmallRs(const FecSymbols& sources, size_t r, FecSymbols* repairs) {
  const auto* matrix = Matrix(sources.size(), r);
  if (!matrix) return false;
  const auto& gf = Gf();
  const size_t size = sources.front().size();
  repairs->resize(r);
  for (size_t row = 0; row < r; ++row) {
    auto& output = (*repairs)[row];
    output.resize(size);
    std::memcpy(output.data(), sources[0].data(), size);
    Scale(output.data(), size, matrix->rows[row][0], gf);
    for (size_t col = 1; col < sources.size(); ++col)
      MultiplyAdd(output.data(), sources[col].data(), size,
                  matrix->rows[row][col], gf);
  }
  return true;
}

bool DecodeSmallRs(size_t k, size_t r, size_t size,
                   const std::vector<bool>& seen, FecSymbols* symbols) {
  std::array<size_t, kRsSmallMaxRepairs> missing{}, repair_ids{};
  size_t count = 0, available = 0;
  for (size_t i = 0; i < k; ++i) {
    if (seen[i]) continue;
    if (count == r) return false;
    missing[count++] = i;
  }
  if (!count) return true;
  for (size_t i = 0; i < r && available < count; ++i)
    if (seen[k + i]) repair_ids[available++] = i;
  if (available < count) return false;
  const auto* matrix = Matrix(k, r);
  if (!matrix) return false;
  const auto& gf = Gf();
  std::array<std::array<uint8_t, kRsSmallMaxRepairs>, kRsSmallMaxRepairs>
      reduced{};
  // Subtract known sources. Only the m missing source symbols are unknown,
  // so solve an m*m system (m <= 4), not a k*k system.
  for (size_t row = 0; row < count; ++row) {
    const auto& coefficients = matrix->rows[repair_ids[row]];
    auto& rhs = (*symbols)[missing[row]];
    std::memcpy(rhs.data(), (*symbols)[k + repair_ids[row]].data(), size);
    for (size_t col = 0; col < k; ++col)
      if (seen[col])
        MultiplyAdd(rhs.data(), (*symbols)[col].data(), size, coefficients[col],
                    gf);
    for (size_t col = 0; col < count; ++col)
      reduced[row][col] = coefficients[missing[col]];
  }
  for (size_t col = 0; col < count; ++col) {
    size_t pivot = col;
    while (pivot < count && !reduced[pivot][col]) ++pivot;
    if (pivot == count) return false;
    if (pivot != col) {
      std::swap(reduced[pivot], reduced[col]);
      std::swap((*symbols)[missing[pivot]], (*symbols)[missing[col]]);
    }
    const uint8_t scale = gf.inverse[reduced[col][col]];
    for (size_t j = col; j < count; ++j)
      reduced[col][j] = gf.multiply[scale][reduced[col][j]];
    Scale((*symbols)[missing[col]].data(), size, scale, gf);
    for (size_t row = 0; row < count; ++row) {
      if (row == col) continue;
      const uint8_t factor = reduced[row][col];
      if (!factor) continue;
      for (size_t j = col; j < count; ++j)
        reduced[row][j] ^= gf.multiply[factor][reduced[col][j]];
      MultiplyAdd((*symbols)[missing[row]].data(),
                  (*symbols)[missing[col]].data(), size, factor, gf);
    }
  }
  return true;
}
}  // namespace minirtc
