#include "OtaEd25519Verifier.h"

#if defined(MODBEE_OTA_HAS_ED25519)

#include <sodium.h>

#include "../OtaNoExcept.h"

OtaResult OtaEd25519Verifier::verify(const uint8_t* message, size_t length,
                                     const uint8_t* signature, size_t signatureLength) {
  if (!message || !signature || signatureLength != crypto_sign_ed25519_BYTES) {
    return OtaResult::BadSignature;
  }
  // Idempotent, and cheap after the first call.
  if (sodium_init() < 0) return OtaResult::BadSignature;
  return crypto_sign_ed25519_verify_detached(signature, message, length, _key) == 0
             ? OtaResult::Ok
             : OtaResult::BadSignature;
}

#endif
