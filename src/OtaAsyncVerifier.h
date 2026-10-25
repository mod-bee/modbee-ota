#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaBackground.h"
#include "OtaManifest.h"
#include "OtaVerifier.h"

#if defined(__has_include)
#if __has_include(<atomic>)
#define MODBEE_OTA_HAS_ASYNC_VERIFIER 1
#endif
#endif

#if defined(MODBEE_OTA_HAS_ASYNC_VERIFIER)

#include <atomic>

/**
 * Runs a slow OtaVerifier on a worker. A P-256 check through wolfSSL takes
 * about a second on an ESP32: never in a PLC scan.
 *
 *   OtaEcdsaP256Verifier ecdsa(key);
 *   OtaAsyncVerifier verifier(ecdsa);
 *   worker.add(verifier);                // the same OtaEsp32Worker as the flash
 *   ota.verifier(&verifier);
 *
 * verify() answers Pending until the worker has an answer for exactly that
 * message and signature. A different one while busy waits its turn; a stale
 * answer for a different one is thrown away.
 */
class OtaAsyncVerifier : public OtaVerifier, public OtaBackground {
public:
  explicit OtaAsyncVerifier(OtaVerifier& inner) : _inner(inner) {}

  OtaResult verify(const uint8_t* message, size_t length, const uint8_t* signature,
                   size_t signatureLength) override;

  void work() override;
  bool busy() const override { return _phase.load(std::memory_order_acquire) == Asked; }
  void onWork(void (*wake)(void*), void* context) override {
    _wake = wake;
    _wakeContext = context;
  }

private:
  enum Phase : uint8_t { Idle = 0, Asked, Answered };

  bool same(const uint8_t* message, size_t length, const uint8_t* signature,
            size_t signatureLength) const;

  OtaVerifier& _inner;
  std::atomic<uint8_t> _phase{Idle};
  uint8_t _result = 0;  // written before _phase = Answered
  uint8_t _message[MODBEE_OTA_SIGNED_SIZE] = {};
  size_t _length = 0;
  uint8_t _signature[MODBEE_OTA_SIGNATURE_MAX] = {};
  size_t _signatureLength = 0;
  void (*_wake)(void*) = nullptr;
  void* _wakeContext = nullptr;
};

#endif
