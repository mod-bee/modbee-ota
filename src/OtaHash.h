#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * SHA-256, streaming. Portable C++ so the same digest is computed on every
 * target and on the host, and the core has no crypto library underneath it.
 *
 * Speed is not the point: an image is hashed a few kilobytes per update()
 * while it is written, so the cost is spread over the download.
 */
class OtaSha256 {
public:
  OtaSha256() { reset(); }

  void reset();
  void update(const uint8_t* data, size_t length);
  void finish(uint8_t digest[32]);

private:
  void block(const uint8_t* p);

  uint32_t _h[8];
  uint8_t _buffer[64];
  uint64_t _bits;
  uint8_t _used;
};

/** CRC-32 as zlib computes it (reflected 0xEDB88320), so a server in any
 *  language checks it with its standard library. Pass the previous value to
 *  continue a running CRC; start from 0. */
uint32_t ota_crc32(const uint8_t* data, size_t length, uint32_t previous = 0);
