#include "OtaTypes.h"

#include <string.h>

#include "OtaNoExcept.h"

// The names are snake_case because they go on the wire in status JSON, where a
// platform matches them as strings. Changing one is a protocol change.

const char* ota_resultName(OtaResult result) {
  switch (result) {
    case OtaResult::Ok: return "ok";
    case OtaResult::Pending: return "pending";
    case OtaResult::Busy: return "busy";
    case OtaResult::InvalidArgument: return "invalid_argument";
    case OtaResult::NotReady: return "not_ready";
    case OtaResult::NoMemory: return "no_memory";
    case OtaResult::Unsupported: return "unsupported";
    case OtaResult::IoError: return "io_error";
    case OtaResult::WrongBoard: return "wrong_board";
    case OtaResult::TooLarge: return "too_large";
    case OtaResult::SameVersion: return "same_version";
    case OtaResult::Downgrade: return "downgrade";
    case OtaResult::Unsigned: return "unsigned";
    case OtaResult::BadSignature: return "bad_signature";
    case OtaResult::Refused: return "refused";
    case OtaResult::Timeout: return "timeout";
    case OtaResult::BadChunk: return "bad_chunk";
    case OtaResult::HashMismatch: return "hash_mismatch";
    case OtaResult::BadImage: return "bad_image";
    case OtaResult::Aborted: return "aborted";
    case OtaResult::RolledBack: return "rolled_back";
    case OtaResult::NotConfirmed: return "not_confirmed";
  }
  return "unknown";
}

const char* ota_stateName(OtaState state) {
  switch (state) {
    case OtaState::Idle: return "idle";
    case OtaState::Offered: return "offered";
    case OtaState::Downloading: return "downloading";
    case OtaState::Verifying: return "verifying";
    case OtaState::Ready: return "ready";
    case OtaState::Applying: return "applying";
    case OtaState::Testing: return "testing";
    case OtaState::Checking: return "checking";
  }
  return "unknown";
}

const char* ota_outcomeName(OtaOutcome outcome) {
  switch (outcome) {
    case OtaOutcome::None: return "none";
    case OtaOutcome::Success: return "success";
    case OtaOutcome::Failed: return "failed";
  }
  return "unknown";
}

const char* ota_commandName(OtaCommand command) {
  switch (command) {
    case OtaCommand::Abort: return "abort";
    case OtaCommand::Apply: return "apply";
    case OtaCommand::Confirm: return "confirm";
    case OtaCommand::Rollback: return "rollback";
    case OtaCommand::Status: return "status";
  }
  return "unknown";
}

bool ota_parseCommand(const char* text, size_t length, OtaCommand& out) {
  static const OtaCommand all[] = {OtaCommand::Abort, OtaCommand::Apply,
                                   OtaCommand::Confirm, OtaCommand::Rollback,
                                   OtaCommand::Status};
  if (!text) return false;
  // Tolerate the trailing newline a person typing into mosquitto_pub leaves.
  while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r' ||
                        text[length - 1] == ' ')) {
    length--;
  }
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    const char* name = ota_commandName(all[i]);
    if (strlen(name) == length && memcmp(name, text, length) == 0) {
      out = all[i];
      return true;
    }
  }
  return false;
}

uint8_t ota_percent(uint32_t received, uint32_t size) {
  if (size == 0) return 0;
  if (received >= size) return 100;
  return (uint8_t)(((uint64_t)received * 100u) / size);
}
