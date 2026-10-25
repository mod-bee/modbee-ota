#include "OtaHash.h"

#include <string.h>

#include "OtaNoExcept.h"

namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

void OtaSha256::reset() {
  static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(_h, init, sizeof(_h));
  _bits = 0;
  _used = 0;
}

void OtaSha256::block(const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  }
  for (int i = 16; i < 64; i++) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = _h[0], b = _h[1], c = _h[2], d = _h[3];
  uint32_t e = _h[4], f = _h[5], g = _h[6], h = _h[7];
  for (int i = 0; i < 64; i++) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + S1 + ch + K[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  _h[0] += a; _h[1] += b; _h[2] += c; _h[3] += d;
  _h[4] += e; _h[5] += f; _h[6] += g; _h[7] += h;
}

void OtaSha256::update(const uint8_t* data, size_t length) {
  if (!data) return;
  _bits += (uint64_t)length * 8u;
  while (length > 0) {
    if (_used == 0 && length >= 64) {
      block(data);
      data += 64;
      length -= 64;
      continue;
    }
    size_t take = 64u - _used;
    if (take > length) take = length;
    memcpy(_buffer + _used, data, take);
    _used = (uint8_t)(_used + take);
    data += take;
    length -= take;
    if (_used == 64) {
      block(_buffer);
      _used = 0;
    }
  }
}

void OtaSha256::finish(uint8_t digest[32]) {
  const uint64_t bits = _bits;
  _buffer[_used++] = 0x80;
  if (_used > 56) {
    memset(_buffer + _used, 0, 64u - _used);
    block(_buffer);
    _used = 0;
  }
  memset(_buffer + _used, 0, 56u - _used);
  for (int i = 0; i < 8; i++) _buffer[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
  block(_buffer);
  for (int i = 0; i < 8; i++) {
    digest[i * 4] = (uint8_t)(_h[i] >> 24);
    digest[i * 4 + 1] = (uint8_t)(_h[i] >> 16);
    digest[i * 4 + 2] = (uint8_t)(_h[i] >> 8);
    digest[i * 4 + 3] = (uint8_t)_h[i];
  }
  reset();
}

uint32_t ota_crc32(const uint8_t* data, size_t length, uint32_t previous) {
  // Nibble table: 64 bytes of flash instead of 1 kB, and a chunk is checked
  // once, so the extra shifts cost nothing that matters.
  static const uint32_t table[16] = {
      0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
      0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
      0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
  uint32_t crc = ~previous;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    crc = (crc >> 4) ^ table[crc & 0x0f];
    crc = (crc >> 4) ^ table[crc & 0x0f];
  }
  return ~crc;
}
