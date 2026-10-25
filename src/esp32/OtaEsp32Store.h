#pragma once

#include "../OtaStore.h"

#if defined(ESP_PLATFORM)

/**
 * The job record in NVS, under its own namespace so it never meets the
 * application's keys (node-uio uses "uio" and "uio_retain").
 *
 * The record is stored with a layout version; one written by a different
 * layout reads as empty rather than as garbage.
 */
class OtaEsp32Store : public OtaStore {
public:
  explicit OtaEsp32Store(const char* nvsNamespace = "modbee_ota") : _namespace(nvsNamespace) {}
  bool load(OtaRecord& record) override;
  bool save(const OtaRecord& record) override;

private:
  const char* _namespace;
};

#endif  // ESP_PLATFORM
