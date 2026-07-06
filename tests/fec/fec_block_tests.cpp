#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "fec_decoder.h"
#include "fec_encoder.h"

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
  }
}

std::vector<uint8_t> MakePayload(size_t size) {
  std::vector<uint8_t> payload(size);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>((i * 17 + 3) & 0xFF);
  }
  return payload;
}

void RecoversMissingSourceSymbolAndTrimsFinalPadding() {
  std::vector<uint8_t> payload = MakePayload(37);

  minirtc::FecBlockConfig config;
  config.source_symbol_count = 3;
  config.repair_symbol_count = 2;
  config.symbol_size = 16;
  config.original_size = static_cast<uint32_t>(payload.size());

  minirtc::FecBlockEncoder encoder(config);
  std::vector<minirtc::FecSymbol> symbols = encoder.Encode(payload);

  Expect(symbols.size() == 5, "encoder should emit source + repair symbols");
  Expect(symbols[0].data.size() == 16, "symbols should be padded to symbol size");

  minirtc::FecBlockDecoder decoder(config);
  for (const auto& symbol : symbols) {
    if (symbol.symbol_id == 1) {
      continue;
    }
    decoder.AddSymbol(symbol);
  }

  Expect(decoder.IsComplete(), "decoder should complete with one source loss");
  Expect(decoder.DecodedData() == payload,
         "decoded data should exactly match original payload");
}

void IgnoresDuplicateSymbols() {
  std::vector<uint8_t> payload = MakePayload(32);

  minirtc::FecBlockConfig config;
  config.source_symbol_count = 2;
  config.repair_symbol_count = 1;
  config.symbol_size = 16;
  config.original_size = static_cast<uint32_t>(payload.size());

  minirtc::FecBlockEncoder encoder(config);
  std::vector<minirtc::FecSymbol> symbols = encoder.Encode(payload);

  minirtc::FecBlockDecoder decoder(config);
  Expect(decoder.AddSymbol(symbols[0]) == minirtc::FecDecodeStatus::kAccepted,
         "first source symbol should be accepted");
  Expect(decoder.AddSymbol(symbols[0]) == minirtc::FecDecodeStatus::kDuplicate,
         "duplicate source symbol should be reported");
}

}  // namespace

int main() {
  RecoversMissingSourceSymbolAndTrimsFinalPadding();
  IgnoresDuplicateSymbols();
  std::cout << "fec_block_tests passed" << std::endl;
  return 0;
}
