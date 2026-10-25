#include "OtaEsp32Store.h"

#if defined(ESP_PLATFORM)

#include <nvs.h>
#include <nvs_flash.h>
#include <string.h>

#include "../OtaNoExcept.h"

namespace {

// Bumped whenever OtaRecord's layout changes.
const uint8_t RECORD_LAYOUT = 2;

struct Stored {
  uint8_t layout;
  OtaRecord record;
};

}  // namespace

bool OtaEsp32Store::load(OtaRecord& record) {
  // nvs_flash_init() is idempotent; the Arduino core has normally done it.
  nvs_flash_init();
  nvs_handle_t handle;
  if (nvs_open(_namespace, NVS_READONLY, &handle) != ESP_OK) return false;
  Stored stored;
  size_t length = sizeof(stored);
  const esp_err_t err = nvs_get_blob(handle, "record", &stored, &length);
  nvs_close(handle);
  if (err != ESP_OK || length != sizeof(stored) || stored.layout != RECORD_LAYOUT) return false;
  // Terminate strings that came off flash, whatever is in them.
  stored.record.version[sizeof(stored.record.version) - 1] = '\0';
  stored.record.previous[sizeof(stored.record.previous) - 1] = '\0';
  record = stored.record;
  return true;
}

bool OtaEsp32Store::save(const OtaRecord& record) {
  nvs_flash_init();
  nvs_handle_t handle;
  if (nvs_open(_namespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  Stored stored;
  memset(&stored, 0, sizeof(stored));
  stored.layout = RECORD_LAYOUT;
  stored.record = record;
  esp_err_t err = nvs_set_blob(handle, "record", &stored, sizeof(stored));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err == ESP_OK;
}

#endif  // ESP_PLATFORM
