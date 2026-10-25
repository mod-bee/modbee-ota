#pragma once

#include <stdint.h>

#include "OtaConfig.h"
#include "OtaTypes.h"

/**
 * What a node remembers about a job across the reboot that installs it.
 *
 * Without it a node that comes back on the old version cannot say whether the
 * new one was rolled back or never applied, and the server has to guess. With
 * it, the first status after boot names the job and how it ended.
 */
struct OtaRecord {
  enum Phase : uint8_t {
    Empty = 0,
    Applied = 1,      // boot slot switched, rebooting into `version`
    Finished = 2,     // outcome is final
    Downloading = 3,  // part written; `written`, `size` and `sha256` say what
  };

  uint8_t phase = Empty;
  OtaOutcome outcome = OtaOutcome::None;
  // Finished: why it ended. Applied: why the new image is about to be
  // abandoned (not_confirmed, rolled_back), written just before the rollback
  // so whichever firmware boots next reports the same reason. Ok while
  // Applied means none was given - a crash, which is a plain rollback.
  OtaResult result = OtaResult::Ok;
  uint32_t job = 0;
  char version[MODBEE_OTA_VERSION_MAX] = {};   // the image the job installed
  char previous[MODBEE_OTA_VERSION_MAX] = {};  // what was running before it

  // Downloading: enough to recognise the same image when it is offered again
  // after a reset, and where to carry on.
  uint32_t size = 0;
  uint32_t written = 0;
  uint8_t sha256[32] = {};
};

/**
 * Persistent storage for one OtaRecord. Written a few times per job: before
 * the reboot, once the outcome is known, and every
 * OtaSettings::resumeEveryBytes while downloading, so wear hardly matters.
 */
class OtaStore {
public:
  virtual ~OtaStore() {}
  virtual bool load(OtaRecord& record) = 0;
  virtual bool save(const OtaRecord& record) = 0;
};
