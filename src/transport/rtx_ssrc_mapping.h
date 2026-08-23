#ifndef _RTX_SSRC_MAPPING_H_
#define _RTX_SSRC_MAPPING_H_

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace minirtc {

struct RtxSsrcPair {
  uint32_t media_ssrc = 0;
  uint32_t rtx_ssrc = 0;
};

inline std::string BuildRtxSsrcAttributes(const std::string& stream_id,
                                          uint32_t media_ssrc,
                                          uint32_t rtx_ssrc) {
  std::string attributes = "a=ssrc:" + std::to_string(media_ssrc) +
                           " name:" + stream_id + "\n";
  if (media_ssrc == 0 || rtx_ssrc == 0 || media_ssrc == rtx_ssrc) {
    return attributes;
  }

  attributes += "a=ssrc-group:FID " + std::to_string(media_ssrc) + " " +
                std::to_string(rtx_ssrc) + "\n";
  attributes += "a=ssrc:" + std::to_string(rtx_ssrc) +
                " cname:" + stream_id + "\n";
  return attributes;
}

inline std::optional<RtxSsrcPair> ParseRtxSsrcGroup(std::string_view line) {
  constexpr std::string_view kPrefix = "a=ssrc-group:";
  if (line.size() < kPrefix.size() ||
      line.compare(0, kPrefix.size(), kPrefix) != 0) {
    return std::nullopt;
  }

  std::istringstream group_stream(std::string(line.substr(kPrefix.size())));
  std::string semantics;
  RtxSsrcPair pair;
  if (!(group_stream >> semantics >> pair.media_ssrc >> pair.rtx_ssrc) ||
      semantics != "FID" || pair.media_ssrc == 0 || pair.rtx_ssrc == 0 ||
      pair.media_ssrc == pair.rtx_ssrc) {
    return std::nullopt;
  }
  return pair;
}

}  // namespace minirtc

#endif
