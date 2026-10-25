#pragma once

// No Arduino headers: the core compiles on a host for the test suite, and the
// OpenPLC blocks include this in a host compile too.
#include <stddef.h>
#include <stdint.h>

#include "OtaConfig.h"

/**
 * Every outcome the library reports, from a call and from a job.
 *
 * One enum rather than one per layer, so a function block mirrors a single
 * OTA_RESULT and a status message names the reason a job ended in the same
 * words a call would. Append only: the numbers are on the wire (STATUS frames)
 * and in the OpenPLC datatype.
 */
enum class OtaResult : uint8_t {
  Ok = 0,
  Pending,          // started, call again - not a failure
  Busy,             // another job owns the updater
  InvalidArgument,
  NotReady,         // begin() not called, or the link is down
  NoMemory,
  Unsupported,      // the target cannot do this (rollback on a part without it)
  IoError,          // flash or storage refused a read, write or erase
  WrongBoard,       // the image is built for different hardware
  TooLarge,         // the image is bigger than the target can hold
  SameVersion,      // already running it; the offer can set force
  Downgrade,        // older than what is running and downgrades are off
  Unsigned,         // a signature is required and the offer has none
  BadSignature,
  Refused,          // the application said no
  Timeout,          // the server stopped answering
  BadChunk,         // a chunk failed its CRC; counted, re-requested
  HashMismatch,     // the whole image does not match the offer's SHA-256
  BadImage,         // the target's own check failed (ESP32 image header, ...)
  Aborted,          // the server or the application cancelled the job
  RolledBack,       // the new image did not confirm and the old one is back
  NotConfirmed,     // the confirm window expired; rolling back
};

#define MODBEE_OTA_RESULT_COUNT 22

/** Where the updater is. Polled by a PLC block, reported in every status. */
enum class OtaState : uint8_t {
  Idle = 0,
  Offered,       // waiting for the application to accept() or reject()
  Downloading,
  Verifying,     // hash, signature and the target's own image check
  Ready,         // verified and waiting for apply()
  Applying,      // boot slot switched, rebooting shortly
  Testing,       // running the new image, not yet confirmed
  Checking,      // checking the offer's signature; can take a second
};

#define MODBEE_OTA_STATE_COUNT 8

/** How the last job ended. Survives a reboot when the target has a store. */
enum class OtaOutcome : uint8_t {
  None = 0,      // no job has finished since the record was cleared
  Success,
  Failed,        // result() says why
};

/** Commands a server (or a person, over a link) sends a node. */
enum class OtaCommand : uint8_t {
  Abort = 1,
  Apply = 2,     // reboot into a verified image held in Ready
  Confirm = 3,   // accept the image under test now
  Rollback = 4,  // reject the image under test and reboot into the old one
  Status = 5,    // publish status now
};

/** Raised through OtaUpdater::onEvent(). Every one is also visible as state. */
enum class OtaEvent : uint8_t {
  Offered = 0,   // an offer is waiting for accept() (autoAccept off)
  Started,
  Ready,         // verified; applies itself unless autoApply is off
  Applying,      // the last moment to save anything before the reboot
  Testing,       // booted into the new image, confirm window running
  Succeeded,
  Failed,        // result() says why, Aborted and RolledBack included
};

// Offer flags. A server sets them per job.
#define MODBEE_OTA_FLAG_MANUAL_APPLY 0x01  // stop in Ready even if autoApply is on
#define MODBEE_OTA_FLAG_FORCE        0x02  // install even if the version is the same

/**
 * The offer: what a server says about an image before sending a byte of it.
 *
 * Everything a node needs to decide yes or no without downloading anything,
 * and everything it checks once the download is done.
 */
struct OtaImage {
  uint32_t job = 0;    // the server's number for this transfer; never 0
  uint32_t size = 0;   // bytes
  uint8_t sha256[32] = {};
  uint8_t signature[MODBEE_OTA_SIGNATURE_MAX] = {};
  uint8_t signatureLength = 0;
  uint8_t flags = 0;
  char version[MODBEE_OTA_VERSION_MAX] = {};
  char board[MODBEE_OTA_BOARD_MAX] = {};
};

/** One status report, as a link publishes it. */
struct OtaStatus {
  OtaState state = OtaState::Idle;
  OtaOutcome outcome = OtaOutcome::None;
  OtaResult result = OtaResult::Ok;
  uint32_t job = 0;
  uint32_t received = 0;     // bytes written to the target so far
  uint32_t size = 0;
  const char* board = "";
  const char* running = "";  // the version this firmware says it is
  const char* offered = "";  // the version of the job, "" when idle
};

const char* ota_resultName(OtaResult result);
const char* ota_stateName(OtaState state);
const char* ota_outcomeName(OtaOutcome outcome);
const char* ota_commandName(OtaCommand command);

/** Parses a command word ("abort", "apply", ...). False when unknown. */
bool ota_parseCommand(const char* text, size_t length, OtaCommand& out);

/** Percent of the image written, 0..100. */
uint8_t ota_percent(uint32_t received, uint32_t size);
