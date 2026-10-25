#include "OtaStagedTarget.h"

#include <string.h>

#include "../OtaNoExcept.h"

OtaResult OtaStagedTarget::begin(const OtaImage& image) {
  if (!_active) {
    const uint32_t align = _storage.writeAlign();
    if (align == 0 || align > MODBEE_OTA_WRITE_ALIGN_MAX || _storage.eraseSize() == 0) {
      return OtaResult::Unsupported;
    }
    if (image.size > _storage.size()) return OtaResult::TooLarge;
    _image = image;
    _active = true;
    _finished = false;
    _good = false;
    _written = _erasedEnd = _verified = 0;
    _pendingCount = 0;
    _sha.reset();
  }
  if (!_eraseFirst) return OtaResult::Ok;

  // One block per call until the image's span is clean.
  if (_erasedEnd >= _image.size) return OtaResult::Ok;
  const OtaResult r = eraseTo(_erasedEnd + 1);
  if (r != OtaResult::Ok) {
    _active = false;
    return r;
  }
  return _erasedEnd >= _image.size ? OtaResult::Ok : OtaResult::Pending;
}

OtaResult OtaStagedTarget::resume(const OtaImage& image, uint32_t& offset) {
  const uint32_t block = _storage.eraseSize();
  const uint32_t align = _storage.writeAlign();
  if (block == 0 || align == 0 || align > MODBEE_OTA_WRITE_ALIGN_MAX || image.size > _storage.size()) {
    offset = 0;
    return OtaResult::Unsupported;
  }
  // From the start of the block the offset is in: the next write erases it,
  // because the power may have gone half way through programming it.
  offset -= offset % block;
  if (offset == 0 || offset >= image.size) {
    offset = 0;
    return OtaResult::Unsupported;
  }
  _image = image;
  _active = true;
  _finished = false;
  _good = false;
  _written = offset;
  _erasedEnd = offset;
  _verified = 0;
  _pendingCount = 0;
  _sha.reset();
  return OtaResult::Ok;
}

OtaResult OtaStagedTarget::eraseTo(uint32_t end) {
  const uint32_t block = _storage.eraseSize();
  while (_erasedEnd < end) {
    if (_erasedEnd + block > _storage.size()) return OtaResult::TooLarge;
    const OtaResult r = _storage.erase(_erasedEnd, block);
    if (r != OtaResult::Ok) return r;
    _erasedEnd += block;
  }
  return OtaResult::Ok;
}

OtaResult OtaStagedTarget::program(const uint8_t* data, size_t length) {
  const OtaResult r = eraseTo(_written + (uint32_t)length);
  if (r != OtaResult::Ok) return r;
  const OtaResult w = _storage.write(_written, data, length);
  if (w != OtaResult::Ok) return w;
  _written += (uint32_t)length;
  return OtaResult::Ok;
}

OtaResult OtaStagedTarget::write(const uint8_t* data, size_t length) {
  if (!_active || _finished) return OtaResult::NotReady;
  const uint32_t align = _storage.writeAlign();

  // Top up a partial word left from the last call first.
  if (_pendingCount > 0) {
    while (length > 0 && _pendingCount < align) {
      _pending[_pendingCount++] = *data++;
      length--;
    }
    if (_pendingCount < align) return OtaResult::Ok;
    const OtaResult r = program(_pending, align);
    if (r != OtaResult::Ok) return r;
    _pendingCount = 0;
  }

  const size_t whole = length - (length % align);
  if (whole > 0) {
    const OtaResult r = program(data, whole);
    if (r != OtaResult::Ok) return r;
  }
  memcpy(_pending, data + whole, length - whole);
  _pendingCount = (uint8_t)(length - whole);
  return OtaResult::Ok;
}

OtaResult OtaStagedTarget::finish() {
  if (!_active) return OtaResult::NotReady;
  if (_good) return OtaResult::Ok;
  if (_finished && _verified >= _image.size) return OtaResult::BadImage;
  if (!_finished) {
    // The last partial word, padded with the erased value.
    if (_pendingCount > 0) {
      const uint32_t align = _storage.writeAlign();
      memset(_pending + _pendingCount, 0xff, align - _pendingCount);
      const OtaResult r = program(_pending, align);
      if (r != OtaResult::Ok) return r;
      _pendingCount = 0;
    }
    _finished = true;
    _verified = 0;
    _sha.reset();
  }

  // Read back what the storage actually holds and hash it: a write that
  // reported Ok and did not stick is caught here, not by the bootloader.
  uint8_t buffer[256];
  uint32_t budget = _verifySlice;
  while (_verified < _image.size && budget > 0) {
    uint32_t n = _image.size - _verified;
    if (n > sizeof(buffer)) n = sizeof(buffer);
    if (n > budget) n = budget;
    const OtaResult r = _storage.read(_verified, buffer, n);
    if (r != OtaResult::Ok) return r;
    _sha.update(buffer, n);
    _verified += n;
    budget -= n;
  }
  if (_verified < _image.size) return OtaResult::Pending;

  uint8_t digest[32];
  _sha.finish(digest);
  if (memcmp(digest, _image.sha256, 32) != 0) return OtaResult::BadImage;
  _good = true;
  return OtaResult::Ok;
}

void OtaStagedTarget::abort() {
  _active = false;
  _finished = false;
  _good = false;
  _pendingCount = 0;
}

OtaResult OtaStagedTarget::activate() {
  if (!_active || !_good) return OtaResult::NotReady;
  return _boot.stage(_image);
}
