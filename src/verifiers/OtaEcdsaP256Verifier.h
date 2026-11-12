#pragma once
// ECDSA P-256 signatures. Header only, and included on purpose where a crypto
// library is on the include path:
//
//   #include <ModBeeOta.h>
//   #include <verifiers/OtaEcdsaP256Verifier.h>
//
// THE ENGINE. On an ESP32 it is the core's own mbedTLS: the board's TLS already
// links its P-256 arithmetic, so the check costs almost no flash. (It used to
// be wolfSSL, on the belief that MQTT linked it for TLS too - on a board it
// does not, TLS is the core's mbedTLS, and wolfSSL's P-256 then cost 14.6 kB
// for this check alone.) Elsewhere, wolfSSL when it is on the include path.
// Either way the check is slow for a scan (~160 ms on the Node UIO), so wrap
// it in an OtaAsyncVerifier and let the OTA worker run it.
//
// THE KEY is the raw public point, X then Y, 64 bytes: the uncompressed point
// without its leading 0x04.
// THE SIGNATURE is r then s, 32 bytes each, over SHA-256 of the bytes
// ota_signedMessage() builds. Whatever signs the offer signs exactly that.
//
// MODBEE_OTA_HAS_ECDSA is defined when an engine is there, and
// MODBEE_OTA_ECDSA_MBEDTLS or MODBEE_OTA_HAS_WOLFSSL says which.
// MODBEE_OTA_ECDSA_WOLFSSL forces wolfSSL where both are.
#include <string.h>
#include "../OtaHash.h"
#include "../OtaVerifier.h"

#if defined(__has_include)
#if !defined(MODBEE_OTA_ECDSA_WOLFSSL) && \
    (defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32) || defined(MODBEE_OTA_ECDSA_USE_MBEDTLS)) && \
    __has_include(<mbedtls/ecdsa.h>)
#define MODBEE_OTA_ECDSA_MBEDTLS 1
#elif __has_include(<modbee-wolfssl.h>)
// ModBee's packaging: wolfSSL's configuration only, none of the OpenSSL
// compatibility layer that collides with the ESP32 core's own TLS client.
#include <modbee-wolfssl.h>
#define MODBEE_OTA_HAS_WOLFSSL 1
#elif __has_include(<wolfssl/options.h>)
#include <wolfssl/options.h>
#define MODBEE_OTA_HAS_WOLFSSL 1
#endif
#endif

#if defined(MODBEE_OTA_ECDSA_MBEDTLS)
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>
#define MODBEE_OTA_HAS_ECDSA 1

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

    uint8_t point[65];
    point[0] = 0x04;  // uncompressed
    memcpy(point + 1, _key, 64);

    mbedtls_ecp_group group;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    // Every step must succeed. A key off the curve, or r / s out of range,
    // is refused (mbedtls_ecp_check_pubkey, mbedtls_ecdsa_verify) - never
    // taken as verified.
    int rc = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&group, &q, point, sizeof(point));
    if (rc == 0) rc = mbedtls_ecp_check_pubkey(&group, &q);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&r, signature, 32);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&s, signature + 32, 32);
    if (rc == 0) rc = mbedtls_ecdsa_verify(&group, digest, sizeof(digest), &q, &r, &s);
    const bool memory = rc == MBEDTLS_ERR_MPI_ALLOC_FAILED || rc == MBEDTLS_ERR_ECP_ALLOC_FAILED;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&group);
    if (rc == 0) return OtaResult::Ok;
    return memory ? OtaResult::NoMemory : OtaResult::BadSignature;
  }

private:
  uint8_t _key[64];
};

#elif defined(MODBEE_OTA_HAS_WOLFSSL)
#define MODBEE_OTA_HAS_ECDSA 1
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

#endif  // the engine
