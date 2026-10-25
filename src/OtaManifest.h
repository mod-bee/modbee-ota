#pragma once

#include <stddef.h>
#include <stdint.h>

#include "OtaTypes.h"

// The text side of the protocol: what a platform writes and reads.
//
// Over MQTT an offer may be JSON and status is always JSON, because the thing
// on the other end is a web service or a person with mosquitto_sub, not another
// node. Chunks and requests stay binary everywhere (OtaProtocol.h).
//
// Offer:
//   {"job":17,"version":"1.4.0","board":"modbee-node-uio","size":1048576,
//    "sha256":"<64 hex>","signature":"<128 hex>","apply":"auto","force":false}
//
// `signature`, `apply` ("auto" | "manual") and `force` are optional. No JSON
// library: the parser reads one flat object, skips keys it does not know, and
// refuses anything malformed rather than guessing.

/** Parses a JSON offer. False if a required field is missing or bad. */
bool ota_parseOfferJson(const char* json, size_t length, OtaImage& image);

/** Writes an offer as JSON - for a server built on this library, and tests. */
size_t ota_writeOfferJson(const OtaImage& image, char* out, size_t capacity);

/** Status as one JSON object. 0 if it does not fit. */
size_t ota_writeStatusJson(const OtaStatus& status, char* out, size_t capacity);

/**
 * Orders two version strings: <0, 0 or >0, like strcmp.
 *
 * Dotted numbers compare as numbers ("1.10" > "1.9"), a leading "v" is ignored,
 * and a pre-release suffix sorts before its release ("1.4.0-rc1" < "1.4.0").
 * Anything else falls back to plain text order, so the answer is at least
 * consistent.
 */
int ota_compareVersions(const char* a, const char* b);

/**
 * The exact bytes a signature covers.
 *
 *   "modbee-ota-v1" 0x00 | sha256[32] | size u32 LE | board[32] | version[32]
 *
 * Board and version are zero padded. Signing the metadata with the digest
 * means a signed image cannot be replayed onto another board, or relabelled
 * with a newer version to get past the downgrade check.
 */
#define MODBEE_OTA_SIGNED_SIZE (14 + 32 + 4 + 32 + 32)
size_t ota_signedMessage(const OtaImage& image, uint8_t* out, size_t capacity);

/** Hex to bytes. Returns the bytes written, 0 on an odd length or bad digit. */
size_t ota_fromHex(const char* hex, size_t length, uint8_t* out, size_t capacity);
/** Bytes to lowercase hex, terminated. False if it does not fit. */
bool ota_toHex(const uint8_t* data, size_t length, char* out, size_t capacity);
