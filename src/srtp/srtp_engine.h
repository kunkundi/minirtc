/*
 * @Author: DI JUNKUN
 * @Date: 2025-09-15
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SRTP_H_
#define _SRTP_H_

#include <srtp2/srtp.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace minirtc {

class SrtpEngine {
 public:
  struct Params {
    // 16-byte AES key + 12-byte salt for AES-128-GCM.
    uint8_t key[16]{};
    uint8_t salt[12]{};
    // Sender side usually binds to a specific SSRC.
    // Receiver may ignore and use ssrc_any_inbound.
    uint32_t ssrc = 0;
    bool receiver_any_inbound = false;
  };

  class SrtpSession {
   public:
    SrtpSession() = default;
    SrtpSession(const SrtpSession&) = delete;
    SrtpSession& operator=(const SrtpSession&) = delete;
    SrtpSession(SrtpSession&& other) noexcept { moveFrom(std::move(other)); }
    SrtpSession& operator=(SrtpSession&& other) noexcept {
      if (this != &other) {
        cleanup();
        moveFrom(std::move(other));
      }
      return *this;
    }
    ~SrtpSession() { cleanup(); }

    // buf: full RTP packet (header + plaintext payload)
    // len: in/out length. On protect, grows by 16 (GCM tag).
    // Returns 0 on success, <0 on failure.
    int protectRtp(uint8_t* buf, int* len) const;

    // buf: full SRTP packet (header + ciphertext + tag)
    // len: in/out length. On unprotect, shrinks by 16.
    // Returns 0 on success, <0 on failure.
    int unprotectRtp(uint8_t* buf, int* len) const;

    bool valid() const { return session_ != nullptr; }

   private:
    friend class SrtpEngine;
    explicit SrtpSession(srtp_t s) : session_(s) {}
    void cleanup() {
      if (session_) {
        srtp_dealloc(session_);
        session_ = nullptr;
      }
    }
    void moveFrom(SrtpSession&& other) {
      session_ = other.session_;
      other.session_ = nullptr;
    }
    srtp_t session_ = nullptr;
  };

  // One-time global init. Safe to call multiple times.
  static void GlobalInit();

  // Create a sender SrtpSession bound to a specific SSRC.
  static SrtpSession CreateSender(const Params& p);

  static std::shared_ptr<SrtpSession> CreateSenderPtr(const Params& p) {
    return std::make_shared<SrtpSession>(CreateSender(p));
  }

  // Create a receiver SrtpSession; by default uses ssrc_any_inbound when
  // p.receiver_any_inbound == true. If false, it binds to p.ssrc.
  static SrtpSession CreateReceiver(const Params& p);

  static std::shared_ptr<SrtpSession> CreateReceiverPtr(const Params& p) {
    return std::make_shared<SrtpSession>(CreateReceiver(p));
  }

  // Helper to build the 28-byte master key for AES-128-GCM.
  static std::vector<uint8_t> BuildMasterKeyGcm(const uint8_t key16[16],
                                                const uint8_t salt12[12]);

  // Translate libsrtp error to string for logging.
  static const char* ErrToStr(srtp_err_status_t e);

 private:
  static void FillGcmPolicy(srtp_policy_t& pol);
  static srtp_t CreateSessionInternal(const Params& p);
};
}  // namespace minirtc

#endif