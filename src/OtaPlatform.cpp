#include "OtaPlatform.h"

#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <chrono>
#endif

#include "OtaNoExcept.h"

namespace {

uint32_t defaultClock() {
#if defined(ARDUINO)
  return millis();
#else
  using namespace std::chrono;
  static const steady_clock::time_point start = steady_clock::now();
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - start).count();
#endif
}

OtaClockFn s_clock = &defaultClock;

}  // namespace

uint32_t ota_millis() { return s_clock(); }

void ota_setClock(OtaClockFn clock) { s_clock = clock ? clock : &defaultClock; }

void ota_resetClock() { s_clock = &defaultClock; }
