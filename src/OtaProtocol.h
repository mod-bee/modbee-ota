#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaTypes.h"

// The binary wire format. One format on every transport - MQTT, Sparkplug,
// BeeBus, a serial cable - so a gateway relays a frame without decoding it, and
// a server written once serves every link. docs/PROTOCOL.md is the reference.
//
// Every frame starts with the same six bytes, little-endian throughout:
//
//   [0] type   [1] protocol revision   [2..5] job
//
// THE NODE PULLS. It asks for a byte range and the server answers with chunks,
// each carrying its own offset and CRC. A request that is lost is asked again;
// a chunk that arrives twice is ignored; a request superseded by a newer one is
// harmless, which is exactly what a "latest value wins" link (BeeBus topics,
// Sparkplug metrics) does to traffic. The server holds no per-transfer state it
// cannot rebuild from the next request.

enum class OtaFrameType : uint8_t {
  Offer = 1,    // server -> node: here is an image, see OtaImage
  Request = 2,  // node -> server: send me [offset, offset + length)
  Chunk = 3,    // server -> node: bytes at offset, CRC-32 over them
  Status = 4,   // node -> server: where the node is
  Command = 5,  // server -> node: abort, apply, confirm, rollback, status
};

#define MODBEE_OTA_HEADER_SIZE 6
#define MODBEE_OTA_REQUEST_SIZE 16
#define MODBEE_OTA_CHUNK_HEADER 14
#define MODBEE_OTA_COMMAND_SIZE 7
#define MODBEE_OTA_STATUS_MAX (MODBEE_OTA_HEADER_SIZE + 12 + 1 + MODBEE_OTA_VERSION_MAX)
#define MODBEE_OTA_OFFER_MAX                                              \
  (MODBEE_OTA_HEADER_SIZE + 4 + 1 + 1 + 32 + 1 + MODBEE_OTA_VERSION_MAX + \
   1 + MODBEE_OTA_BOARD_MAX + MODBEE_OTA_SIGNATURE_MAX)

struct OtaRequest {
  uint32_t job = 0;
  uint32_t offset = 0;
  uint32_t length = 0;
  // The largest chunk this node's link carries. The server slices the range
  // to fit, so one server feeds a 4 kB MQTT link and a 200 byte radio alike.
  uint16_t maxChunk = 0;
};

struct OtaChunk {
  uint32_t job = 0;
  uint32_t offset = 0;
  uint32_t crc = 0;
  const uint8_t* data = nullptr;  // points into the frame it was decoded from
  uint32_t length = 0;
};

/** The frame's type, if it is a frame of this protocol revision at all. */
bool ota_frameType(const uint8_t* frame, size_t length, OtaFrameType& type);

// Encoders return the bytes written, 0 when `capacity` is too small or the
// input is invalid. Decoders return false on anything malformed and never read
// past `length`.

size_t ota_encodeOffer(const OtaImage& image, uint8_t* out, size_t capacity);
bool ota_decodeOffer(const uint8_t* frame, size_t length, OtaImage& image);

size_t ota_encodeRequest(const OtaRequest& request, uint8_t* out, size_t capacity);
bool ota_decodeRequest(const uint8_t* frame, size_t length, OtaRequest& request);

/** Writes header and payload, computing the CRC. */
size_t ota_encodeChunk(uint32_t job, uint32_t offset, const uint8_t* data,
                       size_t dataLength, uint8_t* out, size_t capacity);
/** Decodes without checking the CRC - the caller decides what a bad one means. */
bool ota_decodeChunk(const uint8_t* frame, size_t length, OtaChunk& chunk);

/** `status.running` is written; `status.board` and `status.offered` are not
 *  (a server knows both). */
size_t ota_encodeStatus(const OtaStatus& status, uint8_t* out, size_t capacity);
/** `running` receives the version string; status.running is pointed at it. */
bool ota_decodeStatus(const uint8_t* frame, size_t length, OtaStatus& status,
                      char* running, size_t runningCapacity);

size_t ota_encodeCommand(uint32_t job, OtaCommand command, uint8_t* out, size_t capacity);
bool ota_decodeCommand(const uint8_t* frame, size_t length, uint32_t& job, OtaCommand& command);
