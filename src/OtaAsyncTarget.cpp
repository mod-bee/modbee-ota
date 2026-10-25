#include "OtaAsyncTarget.h"

#if defined(MODBEE_OTA_HAS_ASYNC)

#include <new>
#include <string.h>

#include "OtaNoExcept.h"

namespace {

const uint8_t OK = (uint8_t)OtaResult::Ok;

// The most handed to the inner target in one write: one flash sector.
const uint32_t WORKER_SLICE = 4096;

}  // namespace

OtaAsyncTarget::OtaAsyncTarget(OtaTarget& inner, uint32_t bufferSize)
    : _inner(inner), _size(bufferSize < 256 ? 256 : bufferSize) {
  _writeError.store(OK);
}

OtaAsyncTarget::~OtaAsyncTarget() { delete[] _buffer.load(); }

void OtaAsyncTarget::wake() {
  if (_wake) _wake(_wakeContext);
}

uint32_t OtaAsyncTarget::used() const {
  return _tail.load(std::memory_order_acquire) - _head.load(std::memory_order_acquire);
}

bool OtaAsyncTarget::busy() const {
  if (_abort.load(std::memory_order_acquire) || _retry.load(std::memory_order_acquire)) return true;
  if (used() > 0) return true;
  return _op.load(std::memory_order_acquire) != None && !_done.load(std::memory_order_acquire);
}

// --- The caller's side ---

OtaResult OtaAsyncTarget::request(Op op) {
  const uint8_t current = _op.load(std::memory_order_acquire);
  if (current == op && !_abandoned) {
    if (!_done.load(std::memory_order_acquire)) return OtaResult::Pending;
    const OtaResult r = (OtaResult)_result.load(std::memory_order_acquire);
    _op.store(None, std::memory_order_release);
    return r;
  }
  if (current != None) {
    // Another operation is out, or one from a job since aborted. If it is
    // finished, its answer was abandoned; drop it. If not, wait for it.
    if (!_done.load(std::memory_order_acquire)) return OtaResult::Pending;
    _op.store(None, std::memory_order_release);
  }
  _abandoned = false;
  _done.store(false, std::memory_order_relaxed);
  _op.store(op, std::memory_order_release);
  wake();
  return OtaResult::Pending;
}

OtaResult OtaAsyncTarget::begin(const OtaImage& image) {
  // An abort still being carried out, or a previous job's operation still
  // running: the new job waits for it.
  if (_abort.load(std::memory_order_acquire)) return OtaResult::Pending;
  if (_abandoned || _op.load(std::memory_order_acquire) != Begin) {
    if (!_buffer.load(std::memory_order_acquire)) {
      uint8_t* fresh = new (std::nothrow) uint8_t[_size];
      if (!fresh) return OtaResult::NoMemory;
      _buffer.store(fresh, std::memory_order_release);
    }
    if (used() > 0) return OtaResult::Pending;  // the worker is still discarding
    _writeError.store(OK, std::memory_order_release);
    _image = image;  // read by the worker only after _op is published
  }
  return request(Begin);
}

OtaResult OtaAsyncTarget::resume(const OtaImage& image, uint32_t& offset) {
  if (_abort.load(std::memory_order_acquire)) return OtaResult::Pending;
  if (_abandoned || _op.load(std::memory_order_acquire) != Resume) {
    if (!_buffer.load(std::memory_order_acquire)) {
      uint8_t* fresh = new (std::nothrow) uint8_t[_size];
      if (!fresh) return OtaResult::NoMemory;
      _buffer.store(fresh, std::memory_order_release);
    }
    if (used() > 0) return OtaResult::Pending;
    _writeError.store(OK, std::memory_order_release);
    _image = image;
    _offset = offset;
  }
  const OtaResult r = request(Resume);
  if (r != OtaResult::Pending) offset = _offset;  // published before _done
  return r;
}

OtaResult OtaAsyncTarget::write(const uint8_t* data, size_t length) {
  const uint8_t error = _writeError.load(std::memory_order_acquire);
  if (error != OK) return (OtaResult)error;
  uint8_t* buffer = _buffer.load(std::memory_order_acquire);
  if (!buffer || length > _size) return OtaResult::InvalidArgument;
  if (length > _size - used()) return OtaResult::Pending;

  // Copy in, in two pieces where the ring wraps, then publish.
  const uint32_t tail = _tail.load(std::memory_order_relaxed);
  const uint32_t at = tail % _size;
  const uint32_t first = (uint32_t)length < _size - at ? (uint32_t)length : _size - at;
  memcpy(buffer + at, data, first);
  if (first < length) memcpy(buffer, data + first, length - first);
  _tail.store(tail + (uint32_t)length, std::memory_order_release);
  wake();
  return OtaResult::Ok;
}

OtaResult OtaAsyncTarget::finish() {
  const uint8_t error = _writeError.load(std::memory_order_acquire);
  if (error != OK) return (OtaResult)error;
  return request(Finish);
}

OtaResult OtaAsyncTarget::activate() { return request(Activate); }

OtaResult OtaAsyncTarget::confirm() { return request(Confirm); }

void OtaAsyncTarget::abort() {
  _abandoned = true;
  _abort.store(true, std::memory_order_release);
  wake();
}

// --- The worker's side ---

void OtaAsyncTarget::work() {
  if (_abort.load(std::memory_order_acquire)) {
    _inner.abort();
    // Throw away what was waiting, finish any operation as Aborted, and only
    // then let a new job in.
    _head.store(_tail.load(std::memory_order_acquire), std::memory_order_release);
    if (_op.load(std::memory_order_acquire) != None && !_done.load(std::memory_order_acquire)) {
      _result.store((uint8_t)OtaResult::Aborted, std::memory_order_relaxed);
      _done.store(true, std::memory_order_release);
    }
    _writeError.store(OK, std::memory_order_release);
    _retry.store(false, std::memory_order_release);
    _abort.store(false, std::memory_order_release);
    return;
  }

  const uint8_t op = _op.load(std::memory_order_acquire);
  const bool opWaiting = op != None && !_done.load(std::memory_order_acquire);

  // Begin and Resume come before the data of their own job; everything
  // else after it.
  if (opWaiting && (op == Begin || op == Resume)) {
    const OtaResult r = op == Begin ? _inner.begin(_image) : _inner.resume(_image, _offset);
    if (r == OtaResult::Pending) {
      _retry.store(true, std::memory_order_release);
      return;
    }
    _retry.store(false, std::memory_order_release);
    _result.store((uint8_t)r, std::memory_order_relaxed);
    _done.store(true, std::memory_order_release);
    return;
  }

  // Image data, a sector at a time, until the ring is empty or an abort comes.
  uint8_t* buffer = _buffer.load(std::memory_order_acquire);
  while (buffer && !_abort.load(std::memory_order_acquire)) {
    const uint32_t head = _head.load(std::memory_order_relaxed);
    const uint32_t waiting = _tail.load(std::memory_order_acquire) - head;
    if (waiting == 0) break;
    const uint32_t at = head % _size;
    uint32_t n = waiting < _size - at ? waiting : _size - at;
    if (n > WORKER_SLICE) n = WORKER_SLICE;
    if (_writeError.load(std::memory_order_acquire) != OK) {
      _head.store(head + waiting, std::memory_order_release);  // a dead job; drop it
      break;
    }
    const OtaResult r = _inner.write(buffer + at, n);
    if (r == OtaResult::Pending) {
      _retry.store(true, std::memory_order_release);
      return;
    }
    if (r != OtaResult::Ok) _writeError.store((uint8_t)r, std::memory_order_release);
    _head.store(head + n, std::memory_order_release);
  }
  if (_abort.load(std::memory_order_acquire)) return;

  if (!opWaiting || used() > 0) {
    _retry.store(false, std::memory_order_release);
    return;
  }
  OtaResult r = OtaResult::InvalidArgument;
  switch (op) {
    case Finish: {
      const uint8_t error = _writeError.load(std::memory_order_acquire);
      r = error != OK ? (OtaResult)error : _inner.finish();
      break;
    }
    case Activate: r = _inner.activate(); break;
    case Confirm: r = _inner.confirm(); break;
    default: break;
  }
  if (r == OtaResult::Pending) {
    _retry.store(true, std::memory_order_release);
    return;
  }
  _retry.store(false, std::memory_order_release);
  _result.store((uint8_t)r, std::memory_order_relaxed);
  _done.store(true, std::memory_order_release);
}

#endif  // MODBEE_OTA_HAS_ASYNC
