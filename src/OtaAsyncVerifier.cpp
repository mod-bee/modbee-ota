#include "OtaAsyncVerifier.h"

#if defined(MODBEE_OTA_HAS_ASYNC_VERIFIER)

#include <string.h>

#include "OtaNoExcept.h"

bool OtaAsyncVerifier::same(const uint8_t* message, size_t length, const uint8_t* signature,
                            size_t signatureLength) const {
  return length == _length && signatureLength == _signatureLength &&
         memcmp(message, _message, length) == 0 &&
         memcmp(signature, _signature, signatureLength) == 0;
}

OtaResult OtaAsyncVerifier::verify(const uint8_t* message, size_t length,
                                   const uint8_t* signature, size_t signatureLength) {
  if (!message || !signature || length > sizeof(_message) ||
      signatureLength > sizeof(_signature)) {
    return OtaResult::BadSignature;
  }
  const uint8_t phase = _phase.load(std::memory_order_acquire);
  if (phase == Asked) return OtaResult::Pending;  // the worker owns the buffers
  if (phase == Answered) {
    const bool mine = same(message, length, signature, signatureLength);
    const OtaResult r = (OtaResult)_result;
    _phase.store(Idle, std::memory_order_release);
    if (mine) return r;
    // An answer for an offer since abandoned: ask again for this one.
  }
  memcpy(_message, message, length);
  _length = length;
  memcpy(_signature, signature, signatureLength);
  _signatureLength = signatureLength;
  _phase.store(Asked, std::memory_order_release);
  if (_wake) _wake(_wakeContext);
  return OtaResult::Pending;
}

void OtaAsyncVerifier::work() {
  if (_phase.load(std::memory_order_acquire) != Asked) return;
  const OtaResult r = _inner.verify(_message, _length, _signature, _signatureLength);
  if (r == OtaResult::Pending) return;  // an inner check in steps: next time
  _result = (uint8_t)r;
  _phase.store(Answered, std::memory_order_release);
}

#endif
