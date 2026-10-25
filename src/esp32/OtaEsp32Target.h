#pragma once

#include "../OtaTarget.h"

#if defined(ESP_PLATFORM)

#include <esp_ota_ops.h>
#include <esp_partition.h>

/**
 * The boot hook: switches, confirms and trials images before setup() and
 * the PLC run, so the runtime never writes otadata (whose rewrites freeze
 * both cores ~50 ms). Called by the Arduino core through
 * verifyRollbackLater(); an ESP-IDF app calls it early in app_main().
 * True when it managed the boot record.
 */
bool ota_esp32Boot();

/**
 * Spends the run's first flash erase (which freezes both cores ~45 ms) on a
 * sector that is already blank, so a later download does not take it mid-run.
 * Call it once, early: an OpenPLC block's first scan, a sketch's setup().
 * prepare() calls it too. True once done.
 */
bool ota_esp32WarmUp();

/**
 * The ESP32 family's own OTA: two app slots, the bootloader picks one, and an
 * image that does not confirm itself is rolled back.
 *
 * Needs a partition table with ota_0 and ota_1 and an otadata partition, as
 * the Arduino core's default and "Minimal SPIFFS" schemes have. Rollback needs
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, which the Arduino core's prebuilt
 * bootloader has on every ESP32 variant it ships.
 *
 * THE SLOT IS ERASED BEFORE THE FIRST BYTE IS ASKED FOR, one 4 kB sector per
 * call: begin() and resume() answer Pending until the image's whole range is
 * blank, so no call erases the whole slot at once (seconds, enough to trip a
 * watchdog or a PLC scan) and no erase happens while the image is arriving.
 * Measured on the Node UIO: erasing as the write reached each sector held the
 * flash cache off ~45 ms every 4 kB IN THE MIDDLE of the download, and every
 * one of those windows was a chance for the modem's serial link to lose
 * bytes. Writes are then page-sized (256 bytes), so the cache is never off for
 * more than a page program while data flows. The slot is checked by the IDF
 * itself in finish() and again by activate(); both read the image back, a few
 * hundred milliseconds for a full slot, once per job.
 *
 * OTADATA IS ONLY WRITTEN AT BOOT. Each rewrite of the boot record freezes
 * both cores ~50 ms (measured), enough to trip a PLC watchdog. So the slot
 * switch, ESP-IDF's confirm and the inactive-entry erase happen in
 * ota_esp32Boot(), before setup(), and the trial after it is a software one
 * (NVS). See OtaEsp32Target.cpp.
 *
 * UNDER ARDUINO the core calls verifyRollbackLater() at boot, and this file
 * defines it to run the hook. Define MODBEE_OTA_NO_ROLLBACK_HOOK to keep the
 * core's own behaviour (and ESP-IDF's runtime calls).
 */
class OtaEsp32Target : public OtaTarget {
public:
  OtaEsp32Target() {}

  void prepare() override;
  uint32_t capacity() const override;
  OtaResult begin(const OtaImage& image) override;
  OtaResult resume(const OtaImage& image, uint32_t& offset) override;
  OtaResult read(uint32_t offset, uint8_t* out, size_t length) override;
  OtaResult write(const uint8_t* data, size_t length) override;
  OtaResult finish() override;
  void abort() override;
  OtaResult activate() override;
  void reboot() override;
  bool testing() const override;  // the software trial when the hook ran
  OtaResult confirm() override;
  OtaResult rollback() override;

  /** The slot a job writes into, null when there is none (no OTA table). */
  const esp_partition_t* nextSlot() const;
  /** The ESP-IDF error behind the last failure, for a log line. */
  esp_err_t lastError() const { return _lastError; }

private:
  OtaResult fail(esp_err_t err, OtaResult result);

  const esp_partition_t* _slot = nullptr;
  esp_ota_handle_t _handle = 0;
  // The image range still to erase before the job may take data: sectors
  // [_eraseAt, _eraseEnd). One sector per begin()/resume() call.
  OtaResult eraseStep();

  bool _open = false;
  bool _ended = false;
  bool _erasing = false;
  int64_t _erasedAtUs = 0;
  uint32_t _eraseAt = 0;
  uint32_t _eraseEnd = 0;
  esp_err_t _lastError = ESP_OK;
};

#endif  // ESP_PLATFORM
