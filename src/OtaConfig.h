#pragma once

// Compile-time limits. Every one is behind #ifndef so a build can override it
// with -D without editing the library.
//
// The prefix is MODBEE_OTA_, never OTA_: ArduinoOTA and ESP-IDF both define
// OTA_* names, and a sketch can include either beside this library.

// Wire protocol revision carried in every binary frame. A receiver refuses a
// frame from a revision it does not know rather than guessing at its layout.
#define MODBEE_OTA_PROTOCOL 1

// Board and version strings, including the terminator. A version is what a
// person reads ("1.4.2-rc1"), a board is what a build is for ("modbee-node-uio").
#ifndef MODBEE_OTA_VERSION_MAX
#define MODBEE_OTA_VERSION_MAX 32
#endif
#ifndef MODBEE_OTA_BOARD_MAX
#define MODBEE_OTA_BOARD_MAX 32
#endif

// One updater can listen on several links at once - MQTT and BeeBus on the same
// node, say. A job is bound to the link its offer came in on.
#ifndef MODBEE_OTA_MAX_LINKS
#define MODBEE_OTA_MAX_LINKS 3
#endif

// Peers one OtaServer serves at the same time. A gateway feeding a ring of
// nodes raises it; each costs about 80 bytes.
#ifndef MODBEE_OTA_SERVER_PEERS
#define MODBEE_OTA_SERVER_PEERS 4
#endif

// Largest chunk payload any link may carry. Bounds the server's scratch buffer
// and the frame a link has to hold. 4 kB is one flash sector on most parts.
#ifndef MODBEE_OTA_CHUNK_MAX
#define MODBEE_OTA_CHUNK_MAX 4096
#endif

// Detached signature length. Ed25519 is 64; a scheme with longer signatures
// raises this.
#ifndef MODBEE_OTA_SIGNATURE_MAX
#define MODBEE_OTA_SIGNATURE_MAX 64
#endif

// Lean board build: no exception tables in library code (OtaNoExcept.h).
#ifndef MODBEE_OTA_LEAN
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#define MODBEE_OTA_LEAN 1
#else
#define MODBEE_OTA_LEAN 0
#endif
#endif
