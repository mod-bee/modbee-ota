#include "OtaEsp32Target.h"

#if defined(ESP_PLATFORM)

#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#include "../OtaNoExcept.h"

// =============================================================================
// OTADATA IS ONLY WRITTEN AT BOOT
// =============================================================================
//
// Measured on the Node UIO: writing the app slot leaves the other core
// running, but every rewrite of the otadata partition (the boot record at
// 0xe000) freezes BOTH cores for 45-55 ms. A PLC task still in its scan for 10
// periods is stopped by the runtime's watchdog - 10 ms for a 1 ms task - so a
// freeze like that while the PLC runs can stop the machine.
//
// So the runtime never touches otadata:
//
//   download   esp_ota_resume(slot, 0) - the same sequential write as
//              esp_ota_begin(), without its otadata erase
//   activate   "boot slot X next" goes into NVS (NVS writes do not freeze),
//              and the chip restarts
//   boot       ota_esp32Boot(), before setup() and the PLC: switches the slot,
//              confirms the new image with ESP-IDF, and runs the trial
//   trial      a software one: the new image is confirmed with ESP-IDF at
//              boot and must then prove itself (OtaUpdater's confirm policy).
//              confirm() ends the trial in NVS. Not confirmed in time, or
//              booting MODBEE_OTA_TRIAL_BOOTS times without it, and the boot
//              hook switches back to the previous slot.
//
// A crash before the hook runs is still the bootloader's: the image is
// PENDING_VERIFY until then, and the bootloader rolls it back.
//
// If the hook did not run (an ESP-IDF app that does not call it) or NVS is
// unusable, the target falls back to ESP-IDF's own calls at runtime.

#ifndef MODBEE_OTA_TRIAL_BOOTS
#define MODBEE_OTA_TRIAL_BOOTS 3
#endif

namespace {

const char* const NVS_NAMESPACE = "modbee_ota";
const char* const NVS_KEY = "boot";
const uint8_t BOOT_LAYOUT = 1;

enum Switch : uint8_t { SwitchNone = 0, SwitchInstall = 1, SwitchBack = 2 };

struct BootState {
  uint8_t layout = BOOT_LAYOUT;
  uint8_t pending = SwitchNone;  // a switch the runtime asked for
  uint8_t switchTo = 0;          // its app subtype (ota_0 + n)
  uint8_t trial = 0;             // the running image is on trial
  uint8_t boots = 0;             // boots of it while on trial
  uint8_t previous = 0;          // the subtype to go back to
};

bool s_managed = false;  // the boot hook ran with NVS: otadata is boot-only
bool s_testing = false;  // the running image is on trial

bool loadBoot(BootState& b) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  size_t n = sizeof(b);
  const esp_err_t err = nvs_get_blob(h, NVS_KEY, &b, &n);
  nvs_close(h);
  return err == ESP_OK && n == sizeof(b) && b.layout == BOOT_LAYOUT;
}

bool saveBoot(const BootState& b) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t err = nvs_set_blob(h, NVS_KEY, &b, sizeof(b));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err == ESP_OK;
}

const esp_partition_t* appSlot(uint8_t subtype) {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, (esp_partition_subtype_t)subtype, nullptr);
}

}  // namespace

bool ota_esp32Boot() {
  static bool done = false;
  if (done) return s_managed;
  done = true;

  // Called before the Arduino core initialises NVS; nvs_flash_init() is
  // idempotent, and the core's own call after this finds it done.
  if (nvs_flash_init() != ESP_OK) return false;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return false;

  BootState b;
  if (!loadBoot(b)) b = BootState();

  // 1. A switch the runtime asked for: make it, and boot into it.
  if (b.pending != SwitchNone) {
    const uint8_t kind = b.pending;
    const esp_partition_t* to = appSlot(b.switchTo);
    b.pending = SwitchNone;
    if (to && to != running) {
      if (kind == SwitchInstall) {
        b.trial = 1;
        b.boots = 0;
        b.previous = (uint8_t)running->subtype;
      } else {
        b.trial = 0;
      }
      if (saveBoot(b) && esp_ota_set_boot_partition(to) == ESP_OK) esp_restart();
      // The image would not verify, or NVS refused: stay on what runs.
      b.trial = 0;
    }
    saveBoot(b);
  }

  // 2. Fresh from the bootloader's switch: ESP-IDF's confirm, here, where the
  // freeze costs nothing. The trial below decides whether it stays.
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
  }

  // 3. The trial.
  if (b.trial && running->subtype == b.previous) {
    b.trial = 0;  // already back on the old image
    saveBoot(b);
  }
  if (b.trial) {
    b.boots++;
    if (b.boots > MODBEE_OTA_TRIAL_BOOTS) {
      // Booted that often without proving itself: back to the old image.
      b.trial = 0;
      const esp_partition_t* back = appSlot(b.previous);
      saveBoot(b);
      if (back && esp_ota_set_boot_partition(back) == ESP_OK) esp_restart();
    } else {
      saveBoot(b);
      s_testing = true;
    }
  } else {
    // 4. No trial: clear the inactive otadata entry now, which ESP-IDF's
    // esp_ota_begin() would otherwise do at the start of a job - the other
    // otadata freeze, moved to boot.
    esp_ota_invalidate_inactive_ota_data_slot();
  }

  s_managed = true;
  return true;
}

// =============================================================================
// THE FIRST ERASE OF A RUN
// =============================================================================
//
// Measured on the Node UIO: the first flash erase after the application starts
// running (setup() onwards - erases during boot do not count) freezes BOTH
// cores for ~40-50 ms, whatever the partition. Every erase after it, in any
// task, leaves the other core running. So one erase is spent on purpose, early
// - in OpenPLC in OTA_NODE's first scan, which the watchdog does not time -
// instead of letting the first download take the freeze in mid-run.
//
// It erases only a sector that is already blank, so nothing is lost: the
// coredump partition first, then the tail of the inactive app slot.
bool ota_esp32WarmUp() {
  static bool done = false;
  if (done) return true;
  static uint8_t buf[256];
  const esp_partition_t* candidates[2] = {
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr),
      esp_ota_get_next_update_partition(nullptr),
  };
  for (const esp_partition_t* p : candidates) {
    if (!p || p->erase_size == 0 || p->size < p->erase_size) continue;
    const uint32_t at = p->size - p->erase_size;  // the last sector
    bool blank = true;
    for (uint32_t i = 0; i < p->erase_size && blank; i += sizeof(buf)) {
      if (esp_partition_read(p, at + i, buf, sizeof(buf)) != ESP_OK) blank = false;
      for (size_t k = 0; k < sizeof(buf) && blank; k++) blank = buf[k] == 0xff;
    }
    if (!blank) continue;
    if (esp_partition_erase_range(p, at, p->erase_size) == ESP_OK) {
      done = true;
      return true;
    }
  }
  return false;
}

#if defined(ARDUINO) && !defined(MODBEE_OTA_NO_ROLLBACK_HOOK)
// The Arduino core calls this at boot, before setup(). Its weak default
// answers false and the core then confirms a pending image itself; true
// leaves the boot record to ota_esp32Boot(). Linked exactly when the target is.
extern "C" bool verifyRollbackLater() {
  ota_esp32Boot();
  return true;
}
#endif

void OtaEsp32Target::prepare() {
  // The boot hook cleared the inactive otadata entry, before the PLC. Without
  // the hook: ESP-IDF's way, at the cost of one ~50 ms freeze.
  if (!s_managed && !testing()) esp_ota_invalidate_inactive_ota_data_slot();
  // And the first-erase freeze, now rather than mid-download. An OpenPLC
  // block calls ota_esp32WarmUp() in its first scan, before this.
  ota_esp32WarmUp();
}

uint32_t OtaEsp32Target::capacity() const {
  const esp_partition_t* slot = nextSlot();
  return slot ? slot->size : 0;
}

const esp_partition_t* OtaEsp32Target::nextSlot() const {
  return esp_ota_get_next_update_partition(nullptr);
}

OtaResult OtaEsp32Target::fail(esp_err_t err, OtaResult result) {
  _lastError = err;
  return result;
}

// One sector of the job's range per call, PACED; Ok once it is all blank.
//
// Measured on the Node UIO: erasing a 1.8 MB image's 437 sectors
// back to back, ~45 ms each, took 20 s and in that time the LOGIC task
// overran 314 times, its longest scan 340 ms - the flash is busy, so every
// cache miss on the PLC's core waits for the erase. One sector, then a rest
// of 100 ms, keeps the flash free two thirds of the time and the erase
// about a minute (437 sectors).
OtaResult OtaEsp32Target::eraseStep() {
  constexpr int64_t kEraseRestUs = 100 * 1000;
  if (_eraseAt < _eraseEnd) {
    const int64_t now = esp_timer_get_time();
    if (_erasedAtUs != 0 && now - _erasedAtUs < kEraseRestUs) return OtaResult::Pending;
    const esp_err_t err = esp_partition_erase_range(_slot, _eraseAt, _slot->erase_size);
    if (err != ESP_OK) {
      _erasing = false;
      return fail(err, OtaResult::IoError);
    }
    _erasedAtUs = esp_timer_get_time();
    _eraseAt += _slot->erase_size;
    if (_eraseAt < _eraseEnd) return OtaResult::Pending;
  }
  _erasing = false;
  return OtaResult::Ok;
}

OtaResult OtaEsp32Target::begin(const OtaImage& image) {
  if (_erasing) return eraseStep();
  abort();
  _slot = nextSlot();
  if (!_slot) return fail(ESP_ERR_NOT_FOUND, OtaResult::Unsupported);
  if (image.size > _slot->size) return fail(ESP_ERR_INVALID_SIZE, OtaResult::TooLarge);
  // Managed: no otadata erase at runtime (see the top of this file); the IDF
  // way invalidated the inactive entry in prepare(). Either way the handle
  // is opened without erasing (esp_ota_resume erases nothing) and told the
  // image size rather than OTA_WITH_SEQUENTIAL_WRITES, so esp_ota_write()
  // never erases either: the range is erased here, a sector per call.
  const esp_err_t err = esp_ota_resume(_slot, image.size, 0, &_handle);
  if (err != ESP_OK) return fail(err, OtaResult::IoError);
  _open = true;
  _ended = false;
  _lastError = ESP_OK;
  _erasing = true;
  _erasedAtUs = 0;
  _eraseAt = 0;
  _eraseEnd = image.size + (_slot->erase_size - image.size % _slot->erase_size) % _slot->erase_size;
  return eraseStep();
}

OtaResult OtaEsp32Target::resume(const OtaImage& image, uint32_t& offset) {
  if (_erasing) {
    const OtaResult r = eraseStep();
    if (r != OtaResult::Ok && r != OtaResult::Pending) offset = 0;
    return r;
  }
  abort();
  _slot = nextSlot();
  // From the start of the sector the offset is in: that sector may have been
  // half written when the power went. It is erased again below, with the rest
  // of the range, so a sector is never rewritten unerased.
  if (_slot && _slot->erase_size) offset -= offset % _slot->erase_size;
  if (!_slot || offset == 0 || offset >= image.size || image.size > _slot->size) {
    offset = 0;
    return fail(ESP_ERR_INVALID_ARG, OtaResult::Unsupported);
  }
  const esp_err_t err = esp_ota_resume(_slot, image.size, offset, &_handle);
  if (err != ESP_OK) {
    offset = 0;
    return fail(err, OtaResult::Unsupported);
  }
  _open = true;
  _ended = false;
  _lastError = ESP_OK;
  // The rest of the image's range, from the sector the offset is in, a
  // sector per call - see begin().
  _erasing = true;
  _erasedAtUs = 0;
  _eraseAt = offset;
  _eraseEnd = image.size + (_slot->erase_size - image.size % _slot->erase_size) % _slot->erase_size;
  return eraseStep();
}

OtaResult OtaEsp32Target::read(uint32_t offset, uint8_t* out, size_t length) {
  const esp_partition_t* slot = _slot ? _slot : nextSlot();
  if (!slot) return OtaResult::Unsupported;
  const esp_err_t err = esp_partition_read(slot, offset, out, length);
  return err == ESP_OK ? OtaResult::Ok : fail(err, OtaResult::IoError);
}

OtaResult OtaEsp32Target::write(const uint8_t* data, size_t length) {
  if (!_open || _ended || _erasing) return OtaResult::NotReady;
  // A page at a time: each call holds the flash cache off only for one page
  // program (well under a millisecond), so nothing waiting on an interrupt -
  // a UART, the PLC's other core - waits for a whole sector's worth.
  constexpr size_t kPage = 256;
  esp_err_t err = ESP_OK;
  for (size_t at = 0; at < length && err == ESP_OK; at += kPage) {
    const size_t n = length - at < kPage ? length - at : kPage;
    err = esp_ota_write(_handle, data + at, n);
  }
  if (err == ESP_OK) return OtaResult::Ok;
  // The first byte is checked against the image magic: a file that is not an
  // ESP32 application fails here, on the first chunk, not after the download.
  if (err == ESP_ERR_OTA_VALIDATE_FAILED) return fail(err, OtaResult::BadImage);
  return fail(err, OtaResult::IoError);
}

OtaResult OtaEsp32Target::finish() {
  if (!_open) return OtaResult::NotReady;
  if (_ended) return OtaResult::Ok;
  // Checks the image (chip, segments, digest). Reads only - no otadata.
  const esp_err_t err = esp_ota_end(_handle);
  _ended = true;
  if (err == ESP_OK) return OtaResult::Ok;
  _open = false;
  if (err == ESP_ERR_OTA_VALIDATE_FAILED) return fail(err, OtaResult::BadImage);
  return fail(err, OtaResult::IoError);
}

void OtaEsp32Target::abort() {
  if (_open && !_ended) esp_ota_abort(_handle);
  _open = false;
  _ended = false;
  _erasing = false;
}

OtaResult OtaEsp32Target::activate() {
  if (!_open || !_ended || !_slot) return OtaResult::NotReady;
  if (s_managed) {
    // The switch happens at the next boot, before the PLC.
    BootState b;
    if (!loadBoot(b)) b = BootState();
    b.pending = SwitchInstall;
    b.switchTo = (uint8_t)_slot->subtype;
    return saveBoot(b) ? OtaResult::Ok : fail(ESP_FAIL, OtaResult::IoError);
  }
  const esp_err_t err = esp_ota_set_boot_partition(_slot);
  if (err == ESP_OK) return OtaResult::Ok;
  if (err == ESP_ERR_OTA_VALIDATE_FAILED) return fail(err, OtaResult::BadImage);
  return fail(err, OtaResult::IoError);
}

void OtaEsp32Target::reboot() { esp_restart(); }

bool OtaEsp32Target::testing() const {
  if (s_managed) return s_testing;
  esp_ota_img_states_t state;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
  return state == ESP_OTA_IMG_PENDING_VERIFY;
}

OtaResult OtaEsp32Target::confirm() {
  if (s_managed) {
    if (!s_testing) return OtaResult::Ok;
    BootState b;
    if (!loadBoot(b)) b = BootState();
    b.trial = 0;
    b.boots = 0;
    if (!saveBoot(b)) return fail(ESP_FAIL, OtaResult::IoError);
    s_testing = false;
    return OtaResult::Ok;
  }
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  return err == ESP_OK ? OtaResult::Ok : fail(err, OtaResult::IoError);
}

OtaResult OtaEsp32Target::rollback() {
  if (s_managed) {
    BootState b;
    if (!loadBoot(b) || !s_testing || !appSlot(b.previous)) {
      return fail(ESP_ERR_OTA_ROLLBACK_FAILED, OtaResult::Unsupported);
    }
    b.pending = SwitchBack;
    b.switchTo = b.previous;
    b.trial = 0;
    if (!saveBoot(b)) return fail(ESP_FAIL, OtaResult::IoError);
    esp_restart();  // the boot hook switches back, before the PLC
  }
  if (!esp_ota_check_rollback_is_possible()) {
    return fail(ESP_ERR_OTA_ROLLBACK_FAILED, OtaResult::Unsupported);
  }
  // Marks the running image invalid and reboots. Returns only on failure.
  const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
  return fail(err, OtaResult::IoError);
}

#endif  // ESP_PLATFORM
