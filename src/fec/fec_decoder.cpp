#include "fec_decoder.h"

#include <cstdlib>
#include <cstring>

#include "openfec_session.h"
#include "rs_small.h"

extern "C" {
#include "lib_common/of_openfec_api.h"
}

namespace minirtc {
struct FecDecoder::State {
  of_session_t* session = nullptr;
  size_t k = 0;
  size_t size = 0;
  size_t received = 0;
  size_t repairs = 0;
  bool small = false;
  bool complete = false;
  bool failed = false;
  FecSymbols symbols;
  std::vector<bool> seen;
  ~State() {
    if (session) {
      std::lock_guard<std::mutex> lock(OpenFecMutex());
      of_release_codec_instance(session);
    }
  }
  static void* Allocate(void* context, uint32_t size, uint32_t id) {
    auto* state = static_cast<State*>(context);
    // OpenFEC parameters and IDs have already been bounded before entry.
    return id < state->k && size == state->size ? state->symbols[id].data()
                                                : nullptr;
  }
};

FecDecoder::FecDecoder() = default;
FecDecoder::~FecDecoder() = default;
int FecDecoder::Release() {
  state_.reset();
  return 0;
}

bool FecDecoder::Reset(size_t k, size_t repairs, size_t size) {
  if (!k || k > FecEncoder::kMaxSymbols ||
      repairs > FecEncoder::kMaxSymbols - k || !size ||
      size > FecEncoder::kMaxSymbolSize) {
    Release();
    return false;
  }
  const bool small = SupportsSmallRs(k, repairs);
  // Reuse bounded owned buffers between live blocks. A fallback OpenFEC
  // session is never reused: its receive state belongs to exactly one block.
  std::unique_ptr<State> state;
  if ((small || !repairs) && state_ && !state_->session) {
    state = std::move(state_);
  } else {
    Release();
    state = std::make_unique<State>();
  }
  state->k = k;
  state->size = size;
  state->repairs = repairs;
  state->small = small;
  state->received = 0;
  state->complete = state->failed = false;
  state->symbols.resize(k + repairs);
  for (auto& symbol : state->symbols) symbol.assign(size, 0);
  state->seen.assign(k + repairs, false);
  if (repairs && !state->small) {
    std::lock_guard<std::mutex> lock(OpenFecMutex());
    if (of_create_codec_instance(&state->session,
                                 OF_CODEC_REED_SOLOMON_GF_2_M_STABLE,
                                 OF_DECODER, 0) != OF_STATUS_OK)
      return false;
    of_rs_2_m_parameters_t params{};
    params.m = 8;
    params.nb_source_symbols = static_cast<uint32_t>(k);
    params.nb_repair_symbols = static_cast<uint32_t>(repairs);
    params.encoding_symbol_length = static_cast<uint32_t>(size);
    if (of_set_fec_parameters(state->session,
                              reinterpret_cast<of_parameters_t*>(&params)) !=
            OF_STATUS_OK ||
        of_set_callback_functions(state->session, State::Allocate, nullptr,
                                  state.get()) != OF_STATUS_OK)
      return false;
  }
  state_ = std::move(state);
  return true;
}

FecDecoder::Result FecDecoder::AddSymbol(size_t id, const uint8_t* data,
                                         size_t size) {
  if (!state_ || !data || id >= state_->symbols.size() || size != state_->size)
    return Result::kInvalid;
  auto& s = *state_;
  if (s.failed) return Result::kError;
  if (s.seen[id])
    return std::memcmp(s.symbols[id].data(), data, size) == 0
               ? Result::kDuplicate
               : Result::kInvalid;
  if (s.complete) return Result::kComplete;
  std::memcpy(s.symbols[id].data(), data, size);
  s.seen[id] = true;
  ++s.received;
  if (s.session) {
    std::lock_guard<std::mutex> lock(OpenFecMutex());
    const auto status = of_decode_with_new_symbol(
        s.session, s.symbols[id].data(), static_cast<uint32_t>(id));
    if (status != OF_STATUS_OK && status != OF_STATUS_FAILURE) {
      s.failed = true;
      return Result::kError;
    }
    s.complete = of_is_decoding_complete(s.session);
  } else if (s.small && s.received >= s.k) {
    s.complete = DecodeSmallRs(s.k, s.repairs, s.size, s.seen, &s.symbols);
    if (!s.complete) {
      s.failed = true;
      return Result::kError;
    }
  } else if (!s.small) {
    s.complete = s.received == s.k;
  }
  return s.complete ? Result::kComplete : Result::kAccepted;
}

bool FecDecoder::Complete() const { return state_ && state_->complete; }
const FecSymbols& FecDecoder::Symbols() const {
  static const FecSymbols empty;
  return state_ ? state_->symbols : empty;
}
int FecDecoder::ResetParams(unsigned int k) {
  if (!k || k > FecEncoder::kMaxSymbols) {
    Release();
    return -1;
  }
  return Reset(k, k * 1000 / 667 - k, 1400) ? 0 : -1;
}
uint8_t** FecDecoder::DecodeWithNewSymbol(const char* symbol, unsigned int id) {
  if (AddSymbol(id, reinterpret_cast<const uint8_t*>(symbol), 1400) !=
      Result::kComplete)
    return nullptr;
  auto** packets =
      static_cast<uint8_t**>(std::calloc(state_->k, sizeof(uint8_t*)));
  if (!packets) return nullptr;
  for (size_t i = 0; i < state_->k; ++i) packets[i] = state_->symbols[i].data();
  return packets;
}
int FecDecoder::ReleaseSourcePackets(uint8_t** packets) {
  std::free(packets);
  return 0;
}
}  // namespace minirtc
