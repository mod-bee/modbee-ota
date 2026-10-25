#pragma once

#include <stdint.h>

// The one thing the core needs from the platform: a millisecond clock.
//
// Replaceable so the host tests run a simulated clock and a timeout of two
// minutes takes no time at all. Arduino's millis() by default, a steady clock
// on a host.

typedef uint32_t (*OtaClockFn)();

uint32_t ota_millis();

/** Wrap-safe: true once `now` is at or past `deadline`. */
inline bool ota_reached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

void ota_setClock(OtaClockFn clock);
void ota_resetClock();
