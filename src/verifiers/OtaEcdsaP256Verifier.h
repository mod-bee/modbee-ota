#pragma once

// ECDSA P-256 signatures, checked with wolfSSL. Header only, and included on
// purpose where wolfSSL is on the include path:
//
//   #include <ModBeeOta.h>
//   #include <verifiers/OtaEcdsaP256Verifier.h>
//
// WHY P-256 AND WOLFSSL. The ModBee MQTT library already links wolfSSL with
// P-256 for TLS, so on a node that speaks MQTT this check costs almost no
// extra flash. The arithmetic is the same code TLS uses; it is about a second
// on an ESP32, so wrap it in an OtaAsyncVerifier and let the OTA worker run it.
//
// THE KEY is the raw public point, X then Y, 64 bytes: the uncompressed point
// without its leading 0x04.
// THE SIGNATURE is r then s, 32 bytes each, over SHA-256 of the bytes
// ota_signedMessage() builds. Whatever signs the offer signs exactly that.

#include <string.h>

#include "../OtaHash.h"
#include "../OtaVerifier.h"

#if defined(__has_include)
#if __has_include(<modbee-wolfssl.h>)
// ModBee's packaging: wolfSSL's configuration only, none of the OpenSSL
// compatibility layer that collides with the ESP32 core's own TLS client.
#include <modbee-wolfssl.h>
#define MODBEE_OTA_HAS_WOLFSSL 1
#elif __has_include(<wolfssl/options.h>)
#include <wolfssl/options.h>
#define MODBEE_OTA_HAS_WOLFSSL 1
#endif
#endif

#if defined(MODBEE_OTA_HAS_WOLFSSL)

#include <wolfssl/wolfcrypt/ecc.h>

#if !defined(HAVE_ECC)
#error "OtaEcdsaP256Verifier needs wolfSSL built with HAVE_ECC"
#endif

class OtaEcdsaP256Verifier : public OtaVerifier {
public:
  explicit OtaEcdsaP256Verifier(const uint8_t publicKey[64]) { memcpy(_key, publicKey, 64); }

  OtaResult verify(const uint8_t* message, size_t length, const uint8_t* signature,
                   size_t signatureLength) override {
    if (!message || !signature || signatureLength != 64) return OtaResult::BadSignature;

    uint8_t digest[32];
    OtaSha256 sha;
    sha.update(message, length);
    sha.finish(digest);

    // r and s, raw, to the DER wolfSSL's verify takes.
    uint8_t der[80];
    word32 derLength = sizeof(der);
    if (wc_ecc_rs_raw_to_sig(signature, 32, signature + 32, 32, der, &derLength) != 0) {
      return OtaResult::BadSignature;
    }

    ecc_key key;
    if (wc_ecc_init(&key) != 0) return OtaResult::NoMemory;
    int verified = 0;
    const int imported = wc_ecc_import_unsigned(&key, _key, _key + 32, nullptr, ECC_SECP256R1);
    const int checked = imported == 0
                            ? wc_ecc_verify_hash(der, derLength, digest, sizeof(digest), &verified, &key)
                            : -1;
    wc_ecc_free(&key);
    return (checked == 0 && verified == 1) ? OtaResult::Ok : OtaResult::BadSignature;
  }

private:
  uint8_t _key[64];
};

#endif  // MODBEE_OTA_HAS_WOLFSSL
