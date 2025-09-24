#include "srtp_engine.h"

#include "log.h"

namespace minirtc {

static std::atomic<bool> g_srtp_inited{false};

void SrtpEngine::GlobalInit() {
  bool expected = false;
  if (g_srtp_inited.compare_exchange_strong(expected, true)) {
    srtp_err_status_t st = srtp_init();
    if (st != srtp_err_status_ok) {
      g_srtp_inited = false;
      LOG_ERROR("srtp_init failed");
    }
  }
}

std::vector<uint8_t> SrtpEngine::BuildMasterKeyGcm(const uint8_t key16[16],
                                                   const uint8_t salt12[12]) {
  std::vector<uint8_t> mk(28);
  std::memcpy(mk.data(), key16, 16);
  std::memcpy(mk.data() + 16, salt12, 12);
  return mk;
}

const char* SrtpEngine::ErrToStr(srtp_err_status_t e) {
  switch (e) {
    case srtp_err_status_ok:
      return "ok";
    case srtp_err_status_fail:
      return "fail";
    case srtp_err_status_bad_param:
      return "bad_param";
    case srtp_err_status_alloc_fail:
      return "alloc_fail";
    case srtp_err_status_dealloc_fail:
      return "dealloc_fail";
    case srtp_err_status_init_fail:
      return "init_fail";
    case srtp_err_status_terminus:
      return "terminus";
    case srtp_err_status_auth_fail:
      return "auth_fail";
    case srtp_err_status_cipher_fail:
      return "cipher_fail";
    case srtp_err_status_replay_fail:
      return "replay_fail";
    case srtp_err_status_replay_old:
      return "replay_old";
    case srtp_err_status_algo_fail:
      return "algo_fail";
    case srtp_err_status_no_such_op:
      return "no_such_op";
    case srtp_err_status_no_ctx:
      return "no_ctx";
    case srtp_err_status_cant_check:
      return "cant_check";
    case srtp_err_status_key_expired:
      return "key_expired";
    case srtp_err_status_socket_err:
      return "socket_err";
    case srtp_err_status_signal_err:
      return "signal_err";
    case srtp_err_status_nonce_bad:
      return "nonce_bad";
    case srtp_err_status_read_fail:
      return "read_fail";
    case srtp_err_status_write_fail:
      return "write_fail";
    case srtp_err_status_parse_err:
      return "parse_err";
    case srtp_err_status_encode_err:
      return "encode_err";
    case srtp_err_status_semaphore_err:
      return "semaphore_err";
    case srtp_err_status_pfkey_err:
      return "pfkey_err";
    default:
      return "unknown";
  }
}

void SrtpEngine::FillGcmPolicy(srtp_policy_t& pol) {
  std::memset(&pol, 0, sizeof(pol));
  // RTP uses AEAD AES-128-GCM with 16-byte auth tag.
  srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol.rtp);
  // RTCP is not used by this engine, but libsrtp requires a policy; set same.
  srtp_crypto_policy_set_aes_gcm_128_16_auth(&pol.rtcp);
  pol.rtp.sec_serv = sec_serv_conf_and_auth;
  pol.rtcp.sec_serv = sec_serv_conf_and_auth;

  pol.next = nullptr;
}

srtp_t SrtpEngine::CreateSessionInternal(const Params& p) {
  if (!g_srtp_inited.load()) {
    throw std::logic_error("SrtpEngine::GlobalInit must be called first");
  }

  auto mk = BuildMasterKeyGcm(p.key, p.salt);  // 28 bytes
  srtp_policy_t pol;
  memset(&pol, 0, sizeof(pol));
  FillGcmPolicy(pol);

  if (p.receiver_any_inbound) {
    pol.ssrc.type = ssrc_any_inbound;
    pol.ssrc.value = 0;
  } else {
    pol.ssrc.type = ssrc_specific;
    pol.ssrc.value = p.ssrc;
  }
  pol.key = mk.data();
  pol.window_size = 128;
  pol.allow_repeat_tx = 0;
  pol.enc_xtn_hdr_count = 0;
  pol.next = nullptr;

  srtp_t SrtpSession = nullptr;
  srtp_err_status_t st = srtp_create(&SrtpSession, &pol);
  if (st != srtp_err_status_ok) {
    LOG_ERROR("srtp_create failed: {}", ErrToStr(st));
    return nullptr;
  }

  // libsrtp does not take ownership of mk memory; but it expects pol.key to
  // live as long as the SrtpSession. We therefore duplicate into SRTP internal
  // key store by rekeying immediately so we can free mk. Alternatively, keep
  // mk in a heap allocation tied to the SrtpSession lifetime. Here we choose to
  // keep mk alive by attaching it to SRTP with srtp_update.
  // However, srtp_create already copies the key into the SrtpSession; so mk can
  // go out of scope safely in libsrtp >= 2.x.
  // No further action needed.

  return SrtpSession;
}

SrtpEngine::SrtpSession SrtpEngine::CreateSender(const Params& p) {
  Params cp = p;
  cp.receiver_any_inbound = false;  // sender should be specific SSRC
  if (cp.ssrc == 0) {
    throw std::invalid_argument("sender requires a non-zero SSRC");
  }
  srtp_t s = CreateSessionInternal(cp);
  return SrtpSession{s};
}

SrtpEngine::SrtpSession SrtpEngine::CreateReceiver(const Params& p) {
  srtp_t s = CreateSessionInternal(p);
  return SrtpSession{s};
}

int SrtpEngine::SrtpSession::protectRtp(uint8_t* buf, int* len) const {
  if (!session_) return -1;
  if (!buf || !len || *len <= 0) return -2;
  // Caller must ensure tailroom >= 16 for GCM tag.
  srtp_err_status_t st = srtp_protect(session_, buf, len);
  return (st == srtp_err_status_ok) ? 0 : -3;
}

int SrtpEngine::SrtpSession::unprotectRtp(uint8_t* buf, int* len) const {
  if (!session_) return -1;
  if (!buf || !len || *len <= 0) return -2;
  srtp_err_status_t st = srtp_unprotect(session_, buf, len);
  return (st == srtp_err_status_ok) ? 0 : -3;
}
}