#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaTypes.h"

/**
 * Checks a detached signature. The updater gives it the bytes from
 * ota_signedMessage() - digest, size, board and version - and the signature
 * from the offer.
 *
 * Kept behind an interface so the core carries no crypto library, and a
 * project that does not sign pays nothing: no verifier object, nothing linked.
 *
 *   OtaEcdsaP256Verifier   ECDSA P-256 through wolfSSL - free where the
 *                          ModBee MQTT library already links it for TLS
 *   OtaEd25519Verifier     Ed25519 through the ESP32 core's libsodium
 *
 * Ok: signed by the key. BadSignature: not. Pending: still working - asked
 * again with the same arguments on the next update(). A check that takes
 * long (P-256 is ~1 s on an ESP32) is wrapped in OtaAsyncVerifier and runs on
 * the same worker as the flash.
 */
class OtaVerifier {
public:
  virtual ~OtaVerifier() {}
  virtual OtaResult verify(const uint8_t* message, size_t length, const uint8_t* signature,
                           size_t signatureLength) = 0;
};
