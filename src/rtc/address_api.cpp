#include "minirtc.h"
#include "direct_address.h"
#include "signal_server_address.h"

#include <cstring>

int ParseMiniRtcSignalServerAddress(const char* input, size_t input_size,
                                   MiniRtcSignalServerAddress* output) {
  if (!input || !output) return -1;
  try {
    const auto parsed = minirtc::ParseSignalServerAddress({input, input_size});
    if (!parsed) return -1;
    MiniRtcSignalServerAddress result{};
    if (parsed->host.size() >= sizeof(result.host) || parsed->target.size() >= sizeof(result.target)) return -1;
    std::memcpy(result.host, parsed->host.data(), parsed->host.size());
    std::memcpy(result.target, parsed->target.data(), parsed->target.size());
    result.port = parsed->port.value_or(0);
    *output = result;
    return 0;
  } catch (...) { return -1; }
}
int NormalizeMiniRtcDirectAddress(const char* input, size_t input_size,
                                 char* output, size_t output_capacity) {
  if (!input || !output) return -1;
  try {
    const auto parsed = minirtc::ParseDirectAddress({input, input_size});
    if (!parsed) return -1;
    const auto text = minirtc::DirectAddressString(*parsed);
    if (text.size() >= output_capacity) return -1;
    std::memcpy(output, text.c_str(), text.size() + 1);
    return 0;
  } catch (...) { return -1; }
}
int IsMiniRtcDirectAddressPair(const char* local, size_t local_size,
                              const char* remote, size_t remote_size) {
  if (!local || !remote) return 0;
  try { return minirtc::IsDirectAddressPair({local, local_size}, {remote, remote_size}) ? 1 : 0; }
  catch (...) { return 0; }
}
