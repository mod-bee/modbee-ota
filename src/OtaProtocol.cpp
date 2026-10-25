#include "OtaProtocol.h"

#include <string.h>

#include "OtaHash.h"
#include "OtaNoExcept.h"

namespace {

void put32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

uint32_t get32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

size_t header(OtaFrameType type, uint32_t job, uint8_t* out) {
  out[0] = (uint8_t)type;
  out[1] = MODBEE_OTA_PROTOCOL;
  put32(out + 2, job);
  return MODBEE_OTA_HEADER_SIZE;
}

bool isType(const uint8_t* frame, size_t length, OtaFrameType want) {
  OtaFrameType type;
  return ota_frameType(frame, length, type) && type == want;
}

// A length-prefixed string. Refused if it would not fit `capacity` with its
// terminator, so a decoded string is always terminated.
bool readString(const uint8_t*& p, const uint8_t* end, char* out, size_t capacity) {
  if (p >= end) return false;
  const uint8_t n = *p++;
  if ((size_t)(end - p) < n || n >= capacity) return false;
  memcpy(out, p, n);
  out[n] = '\0';
  p += n;
  return true;
}

bool writeString(uint8_t*& p, const uint8_t* end, const char* text, size_t maximum) {
  const size_t n = text ? strlen(text) : 0;
  if (n >= maximum || n > 255 || (size_t)(end - p) < n + 1) return false;
  *p++ = (uint8_t)n;
  if (n) memcpy(p, text, n);
  p += n;
  return true;
}

}  // namespace

bool ota_frameType(const uint8_t* frame, size_t length, OtaFrameType& type) {
  if (!frame || length < MODBEE_OTA_HEADER_SIZE) return false;
  if (frame[1] != MODBEE_OTA_PROTOCOL) return false;
  if (frame[0] < (uint8_t)OtaFrameType::Offer || frame[0] > (uint8_t)OtaFrameType::Command) {
    return false;
  }
  type = (OtaFrameType)frame[0];
  return true;
}

// Offer: size u32 | flags u8 | sigLen u8 | sha256[32] | version str | board str | signature

size_t ota_encodeOffer(const OtaImage& image, uint8_t* out, size_t capacity) {
  if (!out || capacity < MODBEE_OTA_HEADER_SIZE + 38) return 0;
  if (image.signatureLength > MODBEE_OTA_SIGNATURE_MAX) return 0;
  uint8_t* p = out + header(OtaFrameType::Offer, image.job, out);
  const uint8_t* end = out + capacity;
  put32(p, image.size);
  p[4] = image.flags;
  p[5] = image.signatureLength;
  memcpy(p + 6, image.sha256, 32);
  p += 38;
  if (!writeString(p, end, image.version, MODBEE_OTA_VERSION_MAX)) return 0;
  if (!writeString(p, end, image.board, MODBEE_OTA_BOARD_MAX)) return 0;
  if ((size_t)(end - p) < image.signatureLength) return 0;
  memcpy(p, image.signature, image.signatureLength);
  p += image.signatureLength;
  return (size_t)(p - out);
}

bool ota_decodeOffer(const uint8_t* frame, size_t length, OtaImage& image) {
  if (!isType(frame, length, OtaFrameType::Offer)) return false;
  if (length < MODBEE_OTA_HEADER_SIZE + 38) return false;
  OtaImage decoded;
  const uint8_t* p = frame + 2;
  const uint8_t* end = frame + length;
  decoded.job = get32(p);
  p += 4;
  decoded.size = get32(p);
  decoded.flags = p[4];
  decoded.signatureLength = p[5];
  memcpy(decoded.sha256, p + 6, 32);
  p += 38;
  if (decoded.signatureLength > MODBEE_OTA_SIGNATURE_MAX) return false;
  if (!readString(p, end, decoded.version, sizeof(decoded.version))) return false;
  if (!readString(p, end, decoded.board, sizeof(decoded.board))) return false;
  if ((size_t)(end - p) != decoded.signatureLength) return false;
  memcpy(decoded.signature, p, decoded.signatureLength);
  image = decoded;
  return true;
}

// Request: offset u32 | length u32 | maxChunk u16

size_t ota_encodeRequest(const OtaRequest& request, uint8_t* out, size_t capacity) {
  if (!out || capacity < MODBEE_OTA_REQUEST_SIZE) return 0;
  uint8_t* p = out + header(OtaFrameType::Request, request.job, out);
  put32(p, request.offset);
  put32(p + 4, request.length);
  p[8] = (uint8_t)request.maxChunk;
  p[9] = (uint8_t)(request.maxChunk >> 8);
  return MODBEE_OTA_REQUEST_SIZE;
}

bool ota_decodeRequest(const uint8_t* frame, size_t length, OtaRequest& request) {
  if (!isType(frame, length, OtaFrameType::Request) || length != MODBEE_OTA_REQUEST_SIZE) {
    return false;
  }
  request.job = get32(frame + 2);
  request.offset = get32(frame + 6);
  request.length = get32(frame + 10);
  request.maxChunk = (uint16_t)(frame[14] | (frame[15] << 8));
  return true;
}

// Chunk: offset u32 | crc32 u32 | data

size_t ota_encodeChunk(uint32_t job, uint32_t offset, const uint8_t* data,
                       size_t dataLength, uint8_t* out, size_t capacity) {
  if (!out || (dataLength > 0 && !data)) return 0;
  if (capacity < MODBEE_OTA_CHUNK_HEADER || capacity - MODBEE_OTA_CHUNK_HEADER < dataLength) {
    return 0;
  }
  uint8_t* p = out + header(OtaFrameType::Chunk, job, out);
  put32(p, offset);
  put32(p + 4, ota_crc32(data, dataLength));
  // memmove: a caller may have read the payload into place already.
  if (dataLength && p + 8 != data) memmove(p + 8, data, dataLength);
  return MODBEE_OTA_CHUNK_HEADER + dataLength;
}

bool ota_decodeChunk(const uint8_t* frame, size_t length, OtaChunk& chunk) {
  if (!isType(frame, length, OtaFrameType::Chunk) || length < MODBEE_OTA_CHUNK_HEADER) {
    return false;
  }
  chunk.job = get32(frame + 2);
  chunk.offset = get32(frame + 6);
  chunk.crc = get32(frame + 10);
  chunk.data = frame + MODBEE_OTA_CHUNK_HEADER;
  chunk.length = (uint32_t)(length - MODBEE_OTA_CHUNK_HEADER);
  return true;
}

// Status: state u8 | outcome u8 | result u8 | reserved u8 | received u32 | size u32 | running str

size_t ota_encodeStatus(const OtaStatus& status, uint8_t* out, size_t capacity) {
  if (!out || capacity < MODBEE_OTA_HEADER_SIZE + 13) return 0;
  uint8_t* p = out + header(OtaFrameType::Status, status.job, out);
  const uint8_t* end = out + capacity;
  p[0] = (uint8_t)status.state;
  p[1] = (uint8_t)status.outcome;
  p[2] = (uint8_t)status.result;
  p[3] = 0;
  put32(p + 4, status.received);
  put32(p + 8, status.size);
  p += 12;
  if (!writeString(p, end, status.running, MODBEE_OTA_VERSION_MAX)) return 0;
  return (size_t)(p - out);
}

bool ota_decodeStatus(const uint8_t* frame, size_t length, OtaStatus& status,
                      char* running, size_t runningCapacity) {
  if (!isType(frame, length, OtaFrameType::Status)) return false;
  if (length < MODBEE_OTA_HEADER_SIZE + 13 || !running) return false;
  const uint8_t* p = frame + MODBEE_OTA_HEADER_SIZE;
  if (p[0] >= MODBEE_OTA_STATE_COUNT || p[1] > (uint8_t)OtaOutcome::Failed ||
      p[2] >= MODBEE_OTA_RESULT_COUNT) {
    return false;
  }
  OtaStatus decoded;
  decoded.job = get32(frame + 2);
  decoded.state = (OtaState)p[0];
  decoded.outcome = (OtaOutcome)p[1];
  decoded.result = (OtaResult)p[2];
  decoded.received = get32(p + 4);
  decoded.size = get32(p + 8);
  p += 12;
  if (!readString(p, frame + length, running, runningCapacity)) return false;
  decoded.running = running;
  status = decoded;
  return true;
}

// Command: command u8

size_t ota_encodeCommand(uint32_t job, OtaCommand command, uint8_t* out, size_t capacity) {
  if (!out || capacity < MODBEE_OTA_COMMAND_SIZE) return 0;
  header(OtaFrameType::Command, job, out);
  out[6] = (uint8_t)command;
  return MODBEE_OTA_COMMAND_SIZE;
}

bool ota_decodeCommand(const uint8_t* frame, size_t length, uint32_t& job, OtaCommand& command) {
  if (!isType(frame, length, OtaFrameType::Command) || length != MODBEE_OTA_COMMAND_SIZE) {
    return false;
  }
  if (frame[6] < (uint8_t)OtaCommand::Abort || frame[6] > (uint8_t)OtaCommand::Status) {
    return false;
  }
  job = get32(frame + 2);
  command = (OtaCommand)frame[6];
  return true;
}
