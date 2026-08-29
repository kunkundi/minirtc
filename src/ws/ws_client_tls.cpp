#include "ws_client.h"

// Platform-specific TLS certificate support for WsClient.

#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifdef _WIN32
#include <wincrypt.h>
#include <windows.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <TargetConditionals.h>
#endif

#include <vector>

#include "log.h"

namespace minirtc {
namespace {

#ifdef _WIN32
struct WindowsRootStoreLocation {
  DWORD flag;
  const char* name;
};

bool ShouldImportWindowsCertificateAsRoot(X509* x509,
                                          bool require_self_signed_ca) {
  if (!require_self_signed_ca) {
    return true;
  }

  return X509_check_issued(x509, x509) == X509_V_OK &&
         X509_check_ca(x509) > 0;
}

int LoadWindowsCertificateStore(X509_STORE* store, DWORD location_flag,
                                const wchar_t* store_name,
                                const char* location_name,
                                bool require_self_signed_ca) {
  HCERTSTORE sys_store =
      CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                    location_flag | CERT_STORE_READONLY_FLAG, store_name);
  if (!sys_store) {
    LOG_WARN("Failed to open Windows {} certificate store", location_name);
    return 0;
  }

  int imported_count = 0;
  PCCERT_CONTEXT cert_ctx = nullptr;
  while ((cert_ctx = CertEnumCertificatesInStore(sys_store, cert_ctx)) !=
         nullptr) {
    const unsigned char* cert_data = cert_ctx->pbCertEncoded;
    X509* x509 = d2i_X509(
        nullptr, &cert_data, static_cast<long>(cert_ctx->cbCertEncoded));
    if (!x509) {
      ERR_clear_error();
      continue;
    }

    if (!ShouldImportWindowsCertificateAsRoot(x509,
                                              require_self_signed_ca)) {
      X509_free(x509);
      continue;
    }

    if (X509_STORE_add_cert(store, x509) == 1) {
      ++imported_count;
    } else {
      ERR_clear_error();
    }
    X509_free(x509);
  }

  CertCloseStore(sys_store, 0);
  LOG_INFO("Loaded {} Windows certificates from {}", imported_count,
           location_name);
  return imported_count;
}

bool LoadWindowsRootCertificates(SSL_CTX* ssl_ctx) {
  if (!ssl_ctx) {
    return false;
  }

  X509_STORE* store = SSL_CTX_get_cert_store(ssl_ctx);
  if (!store) {
    LOG_WARN("Failed to get OpenSSL X509_STORE for Windows certificates");
    return false;
  }

  const WindowsRootStoreLocation locations[] = {
      {CERT_SYSTEM_STORE_CURRENT_USER, "CurrentUser"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE, "LocalMachine"},
      {CERT_SYSTEM_STORE_CURRENT_USER_GROUP_POLICY,
       "CurrentUserGroupPolicy"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY,
       "LocalMachineGroupPolicy"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE_ENTERPRISE,
       "LocalMachineEnterprise"},
  };

  int total_count = 0;
  for (const auto& location : locations) {
    total_count += LoadWindowsCertificateStore(store, location.flag, L"ROOT",
                                               location.name, false);
  }

  const WindowsRootStoreLocation compatibility_locations[] = {
      {CERT_SYSTEM_STORE_CURRENT_USER, "CurrentUserIntermediate"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE, "LocalMachineIntermediate"},
  };

  for (const auto& location : compatibility_locations) {
    total_count += LoadWindowsCertificateStore(store, location.flag, L"CA",
                                               location.name, true);
  }

  return total_count > 0;
}
#endif

#ifdef __APPLE__
#if TARGET_OS_IPHONE
std::string AppleTlsHostname(const std::string& uri) {
  size_t start = uri.find("://");
  start = start == std::string::npos ? 0 : start + 3;
  size_t end = uri.find('/', start);
  std::string authority = uri.substr(start, end - start);
  if (!authority.empty() && authority.front() == '[') {
    const size_t bracket = authority.find(']');
    return bracket == std::string::npos
               ? authority
               : authority.substr(1, bracket - 1);
  }
  const size_t colon = authority.rfind(':');
  return colon == std::string::npos ? authority : authority.substr(0, colon);
}

bool AddX509ToCertificateArray(CFMutableArrayRef certificates, X509* x509) {
  if (!certificates || !x509) {
    return false;
  }

  const int der_size = i2d_X509(x509, nullptr);
  if (der_size <= 0) {
    ERR_clear_error();
    return false;
  }

  std::vector<unsigned char> der(static_cast<size_t>(der_size));
  unsigned char* cursor = der.data();
  if (i2d_X509(x509, &cursor) != der_size) {
    ERR_clear_error();
    return false;
  }

  CFDataRef data = CFDataCreate(kCFAllocatorDefault, der.data(), der_size);
  if (!data) {
    return false;
  }
  SecCertificateRef certificate =
      SecCertificateCreateWithData(kCFAllocatorDefault, data);
  CFRelease(data);
  if (!certificate) {
    return false;
  }

  CFArrayAppendValue(certificates, certificate);
  CFRelease(certificate);
  return true;
}

bool VerifyWithAppleSystemTrust(X509_STORE_CTX* store_ctx,
                                const std::string& hostname) {
  if (!store_ctx || hostname.empty()) {
    return false;
  }

  CFMutableArrayRef certificates = CFArrayCreateMutable(
      kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
  if (!certificates) {
    return false;
  }

  X509* leaf = X509_STORE_CTX_get0_cert(store_ctx);
  bool has_certificate = AddX509ToCertificateArray(certificates, leaf);
  STACK_OF(X509)* untrusted = X509_STORE_CTX_get0_untrusted(store_ctx);
  const int untrusted_count = untrusted ? sk_X509_num(untrusted) : 0;
  for (int index = 0; index < untrusted_count; ++index) {
    X509* certificate = sk_X509_value(untrusted, index);
    if (certificate != leaf) {
      has_certificate |=
          AddX509ToCertificateArray(certificates, certificate);
    }
  }

  bool trusted = false;
  CFStringRef host = CFStringCreateWithCString(
      kCFAllocatorDefault, hostname.c_str(), kCFStringEncodingUTF8);
  SecPolicyRef policy = host ? SecPolicyCreateSSL(true, host) : nullptr;
  SecTrustRef trust = nullptr;
  if (has_certificate && policy &&
      SecTrustCreateWithCertificates(certificates, policy, &trust) ==
          errSecSuccess &&
      trust) {
    CFErrorRef error = nullptr;
    trusted = SecTrustEvaluateWithError(trust, &error);
    if (!trusted && error) {
      CFStringRef description = CFErrorCopyDescription(error);
      char buffer[512] = {};
      if (description && CFStringGetCString(description, buffer,
                                            sizeof(buffer),
                                            kCFStringEncodingUTF8)) {
        LOG_ERROR("Apple system trust evaluation failed: {}", buffer);
      }
      if (description) {
        CFRelease(description);
      }
      CFRelease(error);
    }
  }

  if (trust) {
    CFRelease(trust);
  }
  if (policy) {
    CFRelease(policy);
  }
  if (host) {
    CFRelease(host);
  }
  CFRelease(certificates);
  return trusted;
}
#else
int AddMacCertificateToOpenSslStore(X509_STORE* store, SecCertificateRef cert) {
  if (!store || !cert) {
    return 0;
  }

  CFDataRef cert_data = SecCertificateCopyData(cert);
  if (!cert_data) {
    return 0;
  }

  int imported_count = 0;
  const unsigned char* data =
      reinterpret_cast<const unsigned char*>(CFDataGetBytePtr(cert_data));
  long data_len = static_cast<long>(CFDataGetLength(cert_data));
  if (data && data_len > 0) {
    const unsigned char* cursor = data;
    X509* x509 = d2i_X509(nullptr, &cursor, data_len);
    if (x509) {
      if (X509_STORE_add_cert(store, x509) == 1) {
        imported_count = 1;
      } else {
        ERR_clear_error();
      }
      X509_free(x509);
    } else {
      ERR_clear_error();
    }
  }

  CFRelease(cert_data);
  return imported_count;
}

bool MacTrustSettingsAllowRoot(SecCertificateRef cert,
                               SecTrustSettingsDomain domain) {
  CFArrayRef trust_settings = nullptr;
  OSStatus status =
      SecTrustSettingsCopyTrustSettings(cert, domain, &trust_settings);
  if (status != errSecSuccess || trust_settings == nullptr) {
    return false;
  }

  bool allow_root = false;
  CFIndex settings_count = CFArrayGetCount(trust_settings);
  if (settings_count == 0) {
    allow_root = true;
  }

  for (CFIndex i = 0; i < settings_count; ++i) {
    CFTypeRef setting = CFArrayGetValueAtIndex(trust_settings, i);
    if (!setting || CFGetTypeID(setting) != CFDictionaryGetTypeID()) {
      continue;
    }

    auto setting_dict =
        reinterpret_cast<CFDictionaryRef>(const_cast<void*>(setting));
    SecTrustSettingsResult result = kSecTrustSettingsResultTrustRoot;
    CFTypeRef result_value =
        CFDictionaryGetValue(setting_dict, kSecTrustSettingsResult);
    if (result_value) {
      if (CFGetTypeID(result_value) != CFNumberGetTypeID()) {
        continue;
      }

      SInt32 raw_result = kSecTrustSettingsResultInvalid;
      if (!CFNumberGetValue(reinterpret_cast<CFNumberRef>(
                                const_cast<void*>(result_value)),
                            kCFNumberSInt32Type, &raw_result)) {
        continue;
      }
      result = static_cast<SecTrustSettingsResult>(raw_result);
    }

    if (result == kSecTrustSettingsResultTrustRoot ||
        result == kSecTrustSettingsResultTrustAsRoot) {
      allow_root = true;
      break;
    }
  }

  CFRelease(trust_settings);
  return allow_root;
}

int LoadMacCertificatesFromArray(X509_STORE* store, CFArrayRef certs) {
  if (!store || !certs) {
    return 0;
  }

  int imported_count = 0;
  CFIndex cert_count = CFArrayGetCount(certs);
  for (CFIndex i = 0; i < cert_count; ++i) {
    auto cert = reinterpret_cast<SecCertificateRef>(
        const_cast<void*>(CFArrayGetValueAtIndex(certs, i)));
    imported_count += AddMacCertificateToOpenSslStore(store, cert);
  }
  return imported_count;
}

int LoadMacAnchorCertificates(X509_STORE* store) {
  CFArrayRef certs = nullptr;
  OSStatus status = SecTrustCopyAnchorCertificates(&certs);
  if (status != errSecSuccess || certs == nullptr) {
    LOG_WARN("SecTrustCopyAnchorCertificates failed: {}",
             static_cast<int>(status));
    return 0;
  }

  int imported_count = LoadMacCertificatesFromArray(store, certs);
  CFRelease(certs);

  LOG_INFO("Loaded {} anchor certificates from macOS default anchors",
           imported_count);
  return imported_count;
}

int LoadMacTrustSettingsCertificates(X509_STORE* store,
                                     SecTrustSettingsDomain domain,
                                     const char* domain_name) {
  CFArrayRef certs = nullptr;
  OSStatus status = SecTrustSettingsCopyCertificates(domain, &certs);
  if (status != errSecSuccess || certs == nullptr) {
    if (status != errSecNoTrustSettings) {
      LOG_WARN("SecTrustSettingsCopyCertificates({}) failed: {}", domain_name,
               static_cast<int>(status));
    }
    return 0;
  }

  int imported_count = 0;
  CFIndex cert_count = CFArrayGetCount(certs);
  for (CFIndex i = 0; i < cert_count; ++i) {
    auto cert = reinterpret_cast<SecCertificateRef>(
        const_cast<void*>(CFArrayGetValueAtIndex(certs, i)));
    if (MacTrustSettingsAllowRoot(cert, domain)) {
      imported_count += AddMacCertificateToOpenSslStore(store, cert);
    }
  }

  CFRelease(certs);
  LOG_INFO("Loaded {} trusted root certificates from macOS {} trust settings",
           imported_count, domain_name);
  return imported_count;
}

bool LoadMacSystemAnchorCertificates(SSL_CTX* ssl_ctx) {
  if (!ssl_ctx) {
    return false;
  }

  X509_STORE* store = SSL_CTX_get_cert_store(ssl_ctx);
  if (!store) {
    LOG_WARN("Failed to get OpenSSL X509_STORE for macOS system certificates");
    return false;
  }

  int total_count = LoadMacAnchorCertificates(store);
  total_count += LoadMacTrustSettingsCertificates(
      store, kSecTrustSettingsDomainAdmin, "Admin");
  total_count += LoadMacTrustSettingsCertificates(
      store, kSecTrustSettingsDomainUser, "User");

  LOG_INFO("Loaded {} certificates from macOS trust stores", total_count);
  return total_count > 0;
}
#endif
#endif

bool IsTrustError(int error) {
  return error == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN ||
         error == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT ||
         error == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY ||
         error == X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE;
}

}  // namespace

void WsClient::LoadTlsSystemRootCertificates(SSL_CTX* ssl_ctx) {
  if (!ssl_ctx) {
    LOG_WARN("Unable to load system certificates: no SSL context");
    return;
  }

#ifdef _WIN32
  if (!LoadWindowsRootCertificates(ssl_ctx)) {
    LOG_WARN("Unable to load Windows Root certificates");
  }
#else
#ifdef __APPLE__
#if !TARGET_OS_IPHONE
  if (!LoadMacSystemAnchorCertificates(ssl_ctx)) {
    LOG_WARN(
        "Failed to load certificates from macOS system trust store, fallback "
        "to OpenSSL default verify paths");
  }
#endif
#endif

  const bool loaded_default_paths =
      SSL_CTX_set_default_verify_paths(ssl_ctx) == 1;
  if (!loaded_default_paths) {
    LOG_WARN("Failed to load system CA certificates from default verify paths");
    ERR_clear_error();
  }

#if defined(__linux__)
  const char* ca_bundle_paths[] = {
      "/etc/ssl/certs/ca-certificates.crt",  // Debian/Ubuntu
      "/etc/pki/tls/certs/ca-bundle.crt",    // RHEL/CentOS/Fedora
      "/etc/ssl/ca-bundle.pem",              // openSUSE/SLES
      "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
      "/etc/ssl/cert.pem"  // Arch, Alpine, etc.
  };

  bool loaded_linux_bundle = false;
  for (const char* path : ca_bundle_paths) {
    if (SSL_CTX_load_verify_locations(ssl_ctx, path, nullptr) == 1) {
      LOG_INFO("Loaded Linux system CA bundle from {}", path);
      loaded_linux_bundle = true;
      break;
    }
    ERR_clear_error();
  }

  if (!loaded_default_paths && !loaded_linux_bundle) {
    LOG_WARN(
        "Unable to load Linux system CA bundle from any known path; TLS "
        "verification may fail if no custom CA is provided");
  }
#endif
#endif
}

bool WsClient::VerifyTlsWithSystemTrust(X509_STORE_CTX* store_ctx,
                                       const std::string& uri) {
#ifdef __APPLE__
#if TARGET_OS_IPHONE
  return VerifyWithAppleSystemTrust(store_ctx, AppleTlsHostname(uri));
#else
  (void)store_ctx;
  (void)uri;
  return false;
#endif
#else
  (void)store_ctx;
  (void)uri;
  return false;
#endif
}

bool WsClient::LogTlsVerificationError(X509_STORE_CTX* store_ctx) {
  if (!store_ctx) {
    LOG_ERROR("TLS certificate verify failed: no certificate store context");
    return false;
  }

  const int error = X509_STORE_CTX_get_error(store_ctx);
  const char* message = X509_verify_cert_error_string(error);
  LOG_ERROR("TLS certificate verify failed: {} (err={}, depth={})",
            message ? message : "unknown", error,
            X509_STORE_CTX_get_error_depth(store_ctx));
  return IsTrustError(error);
}

}  // namespace minirtc
