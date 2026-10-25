#pragma once

#include <string.h>

#include "../OtaVerifier.h"

#if defined(ESP_PLATFORM) && defined(__has_include)
#if __has_include(<sodium.h>)
#define MODBEE_OTA_HAS_ED25519 1
#endif
#endif

#if defined(MODBEE_OTA_HAS_ED25519)

/**
 * Ed25519, the same scheme ModBee already signs its editor packages with.
 *
 * Uses the libsodium the ESP32 Arduino core links anyway, so it costs a few
 * hundred bytes of flash, not a crypto library. The key is the raw 32-byte
 * public key: the last 32 bytes of the DER public key OpenSSL writes with
 * `openssl pkey -in key.pem -pubout -outform DER`. docs/PROTOCOL.md has the
 * signed message.
 */
class OtaEd25519Verifier : public OtaVerifier {
public:
  explicit OtaEd25519Verifier(const uint8_t publicKey[32]) { memcpy(_key, publicKey, 32); }
  OtaResult verify(const uint8_t* message, size_t length, const uint8_t* signature,
                   size_t signatureLength) override;

private:
  uint8_t _key[32];
};

#endif
