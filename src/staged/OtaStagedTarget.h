#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../OtaHash.h"
#include "../OtaTarget.h"

// The driver kit for a chip with no OTA of its own - STM32, RP2040, SAMD, nRF.
//
// Such a chip cannot rewrite the code it is running from, so an update is two
// halves that meet at a bootloader:
//
//   OtaStorage      somewhere to stage the image: the upper flash bank, a spare
//                   region of internal flash, an external SPI NOR, a file
//   OtaBootControl  how to tell the bootloader there is an image to install,
//                   and how the bootloader says the new image is on trial
//
// OtaStagedTarget does everything in between - erase ahead of the write,
// alignment, read-back verification - so a new chip is two small classes, not
// a new OtaTarget. The bootloader is chip work (MCUboot, the RP2040 core's
// picoOTA, ST's dual-bank swap, or a small ModBee one); the contract it reads is
// whatever OtaBootControl::stage() writes. See docs/PORTING.md.

#ifndef MODBEE_OTA_WRITE_ALIGN_MAX
#define MODBEE_OTA_WRITE_ALIGN_MAX 32
#endif

/**
 * A raw staging region. Offsets are from its start.
 *
 * NOR semantics: write() may only clear bits of erased bytes, so the target
 * always erases a block before writing into it. Every call is bounded: a part
 * with very large sectors (an STM32F4's 128 kB) stages to external flash
 * instead, or erases up front (OtaStagedTarget's eraseFirst).
 */
class OtaStorage {
public:
  virtual ~OtaStorage() {}
  virtual uint32_t size() const = 0;
  /** Erase granularity. Erases are always whole, aligned blocks. */
  virtual uint32_t eraseSize() const = 0;
  /** Program granularity: 1 for SPI NOR, 8 for an STM32L4, 32 for an H7. */
  virtual uint32_t writeAlign() const { return 1; }
  virtual OtaResult erase(uint32_t offset, uint32_t length) = 0;
  virtual OtaResult write(uint32_t offset, const uint8_t* data, size_t length) = 0;
  virtual OtaResult read(uint32_t offset, uint8_t* out, size_t length) = 0;
};

/**
 * The hand-over to a bootloader.
 *
 * stage() is the commit point: until it returns Ok the running firmware is all
 * there is; after it, the next reboot installs the staged image. It writes
 * whatever the bootloader reads - an MCUboot trailer, a header in a reserved
 * sector, a flag in a backup register.
 */
class OtaBootControl {
public:
  virtual ~OtaBootControl() {}
  virtual OtaResult stage(const OtaImage& image) = 0;
  virtual void reboot() = 0;
  virtual bool testing() const { return false; }
  virtual OtaResult confirm() { return OtaResult::Ok; }
  virtual OtaResult rollback() { return OtaResult::Unsupported; }
};

class OtaStagedTarget : public OtaTarget {
public:
  /**
   * `eraseFirst` erases the whole image's worth of storage in begin(), one
   * block per call, before the first byte is asked for. For storage whose
   * erase is slow or asynchronous. Off: each block is erased just ahead of
   * the write that needs it.
   */
  OtaStagedTarget(OtaStorage& storage, OtaBootControl& boot, bool eraseFirst = false)
      : _storage(storage), _boot(boot), _eraseFirst(eraseFirst) {}

  uint32_t capacity() const override { return _storage.size(); }
  OtaResult begin(const OtaImage& image) override;
  OtaResult resume(const OtaImage& image, uint32_t& offset) override;
  OtaResult read(uint32_t offset, uint8_t* out, size_t length) override {
    return _storage.read(offset, out, length);
  }
  OtaResult write(const uint8_t* data, size_t length) override;
  OtaResult finish() override;
  void abort() override;
  OtaResult activate() override;
  void reboot() override { _boot.reboot(); }
  bool testing() const override { return _boot.testing(); }
  OtaResult confirm() override { return _boot.confirm(); }
  OtaResult rollback() override { return _boot.rollback(); }

  /** Bytes read back per finish() call while verifying. */
  void verifySlice(uint32_t bytes) { _verifySlice = bytes ? bytes : 1; }

private:
  OtaResult eraseTo(uint32_t end);
  OtaResult program(const uint8_t* data, size_t length);

  OtaStorage& _storage;
  OtaBootControl& _boot;
  bool _eraseFirst;
  bool _active = false;
  bool _finished = false;
  bool _good = false;        // read back and matched the offer's digest
  OtaImage _image;
  uint32_t _written = 0;     // bytes committed to storage
  uint32_t _erasedEnd = 0;   // everything below this is erased
  uint8_t _pending[MODBEE_OTA_WRITE_ALIGN_MAX];
  uint8_t _pendingCount = 0;
  uint32_t _verified = 0;
  uint32_t _verifySlice = 4096;
  OtaSha256 _sha;
};
