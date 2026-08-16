#ifndef _COMMON_H_
#define _COMMON_H_

#include <iostream>
#include <mutex>
#include <random>
#include <unordered_set>

namespace minirtc {

int CommonDummy();

constexpr size_t HASH_STRING_PIECE(const char *string_piece) {
  std::size_t result = 0;
  while (*string_piece) {
    result = (result * 131) + *string_piece++;
  }
  return result;
}

constexpr size_t operator""_H(const char *string_piece, size_t) {
  return HASH_STRING_PIECE(string_piece);
}

inline const std::string GetIceUsername(const std::string &sdp) {
  std::string result = "";

  std::string start = "ice-ufrag:";
  std::string end = "\r\n";
  size_t startPos = sdp.find(start);
  size_t endPos = sdp.find(end);

  if (startPos != std::string::npos && endPos != std::string::npos) {
    result = sdp.substr(startPos + start.length(),
                        endPos - startPos - start.length());
  }
  return result;
}

// SSRCManager is used to manage the SSRCs that have been used.

class SSRCManager {
 public:
  static SSRCManager &Instance() {
    static SSRCManager instance;
    return instance;
  }

  void AddSsrc(uint32_t ssrc) {
    std::lock_guard<std::mutex> lock(mutex_);
    ssrcs_.insert(ssrc);
  }

  void DeleteSsrc(uint32_t ssrc) {
    std::lock_guard<std::mutex> lock(mutex_);
    ssrcs_.erase(ssrc);
  }

  bool Contains(uint32_t ssrc) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssrcs_.count(ssrc) > 0;
  }

 private:
  SSRCManager() = default;
  ~SSRCManager() = default;
  SSRCManager(const SSRCManager &) = delete;
  SSRCManager &operator=(const SSRCManager &) = delete;

  std::unordered_set<uint32_t> ssrcs_;
  std::mutex mutex_;
};

inline uint32_t GenerateRandomSSRC() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint32_t> dis(1, 0xFFFFFFFF);
  return dis(gen);
}

inline uint32_t GenerateUniqueSsrc() {
  uint32_t new_ssrc;
  do {
    new_ssrc = GenerateRandomSSRC();
  } while (SSRCManager::Instance().Contains(new_ssrc));
  SSRCManager::Instance().AddSsrc(new_ssrc);
  return new_ssrc;
}
}  // namespace minirtc

#endif
