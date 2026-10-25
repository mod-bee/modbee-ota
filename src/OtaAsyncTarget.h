#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaBackground.h"
#include "OtaTarget.h"

#if defined(__has_include)
#if __has_include(<atomic>)
#define MODBEE_OTA_HAS_ASYNC 1
#endif
#endif

#if defined(MODBEE_OTA_HAS_ASYNC)

#include <atomic>

/**
 * Runs any OtaTarget on a worker, so the caller - a PLC scan - never waits on
 * flash.
 *
 * Measured on the Node UIO (ESP32-S3), the ESP32 target's calls take: a write
 * that reaches a new sector ~60 ms (the erase), the image check ~80 ms, the
 * boot-slot switch ~130 ms, confirm ~50 ms. Through this wrapper the same calls
 * cost the scan a memcpy or an atomic store; the flash work happens on a
 * worker at a priority below the PLC's tasks.
 *
 *   OtaEsp32Target flash;
 *   OtaAsyncTarget target(flash);       // what the updater is given
 *   OtaEsp32Worker worker(target);      // a FreeRTOS task calling work()
 *   OtaUpdater ota(target);
 *
 * TWO SIDES, ONE EACH. The OtaTarget methods are the caller's side and are
 * called from one thread (the one calling OtaUpdater::update()). work() is the
 * worker's side and is called from one other thread. Between them: a byte ring
 * for image data and a one-slot mailbox for operations, both lock-free.
 *
 * A long call answers Pending until the worker has done it; write() answers
 * Pending while the ring is full. testing(), capacity(), reboot() and
 * rollback() go straight to the inner target - they are short, or they do not
 * return.
 */
class OtaAsyncTarget : public OtaTarget, public OtaBackground {
public:
  /** `bufferSize` bytes of image data can wait for the worker. Taken at the
   *  first begin() and kept, so it is not reallocated per job. */
  explicit OtaAsyncTarget(OtaTarget& inner, uint32_t bufferSize = 8192);
  ~OtaAsyncTarget();

  OtaAsyncTarget(const OtaAsyncTarget&) = delete;
  OtaAsyncTarget& operator=(const OtaAsyncTarget&) = delete;

  /** Called whenever there is new work - the worker wakes on it instead of
   *  polling. Optional. Runs on the caller's thread; keep it to a notify. */
  void onWork(void (*wake)(void*), void* context) override {
    _wake = wake;
    _wakeContext = context;
  }

  // --- The worker's side ---

  /** Does what is waiting: writes every byte in the ring, then the pending
   *  operation. One thread only. Returns quickly when there is nothing. */
  void work() override;
  /** True while the worker has something to do or an inner call answered
   *  Pending and must be asked again. A worker sleeps longer when false. */
  bool busy() const override;

  // --- The caller's side (OtaTarget) ---

  // At boot, before the worker matters: straight through.
  void prepare() override { _inner.prepare(); }
  uint32_t capacity() const override { return _inner.capacity(); }
  OtaResult begin(const OtaImage& image) override;
  OtaResult resume(const OtaImage& image, uint32_t& offset) override;
  // Only used at the start of a resumed job, before any data is queued.
  OtaResult read(uint32_t offset, uint8_t* out, size_t length) override {
    return _inner.read(offset, out, length);
  }
  uint32_t backlog() const override { return used(); }
  uint32_t maxWrite() const override { return _size; }
  OtaResult write(const uint8_t* data, size_t length) override;
  OtaResult finish() override;
  void abort() override;
  OtaResult activate() override;
  void reboot() override { _inner.reboot(); }
  bool testing() const override { return _inner.testing(); }
  OtaResult confirm() override;
  OtaResult rollback() override { return _inner.rollback(); }

private:
  enum Op : uint8_t { None = 0, Begin, Resume, Finish, Activate, Confirm };

  OtaResult request(Op op);
  void wake();
  uint32_t used() const;

  OtaTarget& _inner;
  // Allocated by the caller at the first begin(), read by the worker: atomic
  // so the worker never sees the pointer half-published.
  std::atomic<uint8_t*> _buffer{nullptr};
  uint32_t _size;

  // Monotonic counters: the caller advances _tail, the worker _head.
  std::atomic<uint32_t> _head{0};
  std::atomic<uint32_t> _tail{0};

  // The mailbox. The caller writes _image and then _op; the worker writes
  // _result and then _done.
  OtaImage _image;
  uint32_t _offset = 0;  // Resume: in, then where the inner target carries on
  std::atomic<uint8_t> _op{None};
  std::atomic<bool> _done{false};
  std::atomic<uint8_t> _result{0};
  // The caller's own: abort() since the last request, so an operation still in
  // the mailbox belongs to the dropped job, even one of the same kind.
  bool _abandoned = false;

  std::atomic<bool> _abort{false};
  std::atomic<uint8_t> _writeError{0};  // OtaResult; Ok when none
  std::atomic<bool> _retry{false};      // an inner call answered Pending

  void (*_wake)(void*) = nullptr;
  void* _wakeContext = nullptr;
};

#endif  // MODBEE_OTA_HAS_ASYNC
