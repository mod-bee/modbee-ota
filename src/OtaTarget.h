#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaTypes.h"

/**
 * Where an image goes: the driver for one kind of microcontroller.
 *
 * This is the whole contract a new chip has to meet. The updater hands it an
 * image front to back, in pieces no bigger than OtaSettings::writeSlice, and
 * calls nothing else while a job runs.
 *
 *   ESP32           OtaEsp32Target   - the chip's own A/B slots and rollback
 *   STM32, RP2040   OtaStagedTarget  - an OtaStorage to stage into and an
 *                                       OtaBootControl to hand over to a
 *                                       bootloader; see OtaStagedTarget.h
 *   A remote node   (planned)        - a gateway that relays instead of flashing
 *
 * EVERY CALL IS BOUNDED. The updater may run inside a PLC scan, so a call
 * that would take long - verifying a whole image, erasing a big sector -
 * returns OtaResult::Pending and is called again on the next update() until it
 * gives an answer. write() returns Pending when it took nothing and wants the
 * same bytes again later.
 *
 * A driver that cannot make its calls short (an ESP32 sector erase is ~50 ms,
 * switching the boot slot ~130 ms) is wrapped in OtaAsyncTarget, which runs it
 * on a low-priority worker and answers the scan in microseconds.
 */
class OtaTarget {
public:
  virtual ~OtaTarget() {}

  /**
   * Called once by OtaUpdater::begin(), outside any job - at boot, before a
   * PLC is scanning. One-off slow set-up belongs here rather than in the
   * first job's begin(): on an ESP32 the first OTA call after boot erases an
   * otadata sector with both cores stalled (~45 ms measured on the Node UIO).
   */
  virtual void prepare() {}

  /** Largest image this target can take, in bytes. */
  virtual uint32_t capacity() const = 0;

  /** Prepares for an image of image.size bytes. May be Pending. */
  virtual OtaResult begin(const OtaImage& image) = 0;

  /** The next `length` bytes of the image. Always in order, never repeated
   *  once accepted. Pending: nothing taken, offer the same bytes again. */
  virtual OtaResult write(const uint8_t* data, size_t length) = 0;

  /** Every byte is in. The target's own image check goes here. May be Pending. */
  virtual OtaResult finish() = 0;

  /**
   * Continues a job that a reset interrupted. `offset` is how far the
   * updater knows the image got; the target sets it to where it can really
   * carry on - aligned down to a block it can safely rewrite, since the
   * block being written when the power went may be half programmed. The
   * default cannot resume, and the job starts again from zero. May be Pending.
   */
  virtual OtaResult resume(const OtaImage& image, uint32_t& offset) {
    (void)image;
    offset = 0;
    return OtaResult::Unsupported;
  }

  /** Reads back image bytes already written - to rebuild the hash on resume. */
  virtual OtaResult read(uint32_t offset, uint8_t* out, size_t length) {
    (void)offset;
    (void)out;
    (void)length;
    return OtaResult::Unsupported;
  }

  /** The most one write() call takes. The updater slices to fit. */
  virtual uint32_t maxWrite() const { return 0xffffffffu; }

  /** Bytes accepted by write() but not yet in storage. A progress record
   *  never claims more than is really written. */
  virtual uint32_t backlog() const { return 0; }

  /** Drops a job at any point. The running firmware is untouched. */
  virtual void abort() = 0;

  /** Makes the finished image the one that boots next. May be Pending. */
  virtual OtaResult activate() = 0;

  /** Restarts the chip. Does not return on a board. */
  virtual void reboot() = 0;

  // --- Trial boot. A target without rollback leaves these as they are. ---

  /** True when the running image is new and waiting to be confirmed. */
  virtual bool testing() const { return false; }

  /** Keeps the running image. After this a reboot will not undo it.
   *  May be Pending. */
  virtual OtaResult confirm() { return OtaResult::Ok; }

  /** Rejects the running image and reboots into the previous one. */
  virtual OtaResult rollback() { return OtaResult::Unsupported; }
};
