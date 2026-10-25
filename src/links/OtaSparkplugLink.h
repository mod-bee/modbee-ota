#pragma once

// OTA over a Sparkplug B edge node. Header only, included on purpose after
// ModBeeMqtt.h, like OtaMqttLink.h.
//
// Sparkplug has no file transfer, so the image rides in metrics. What a Host
// Application writes (NCMD):
//
//   Node Control/Firmware Offer     String  the JSON offer (OtaManifest.h)
//   Node Control/Firmware Chunk     Bytes   one binary Chunk frame
//   Node Control/Firmware Command   String  "abort" | "apply" | "confirm" | ...
//
// What the node reports (NDATA):
//
//   Firmware/Version    String  what is running
//   Firmware/State      String  idle, downloading, ...
//   Firmware/Progress   Int32   0..100
//   Firmware/Status     String  the full JSON status, as MQTT publishes it
//   Firmware/Request    Bytes   one binary Request frame - the host answers it
//                               with a Firmware Chunk write
//
// ONE CHUNK PER REQUEST. A metric holds its latest write, so two chunks written
// between polls would lose the first. maxWindow() is one chunk: the node asks,
// the host writes, the node asks for the next. Slower than plain MQTT, and
// safe with any Host Application that writes one NCMD per Request it sees.
//
// The chunk metric is HELD (holdAt): a 2 kB write is never echoed back in the
// next NDATA, and its value is cleared once taken so a birth does not carry it.
//
// Commands are taken by polling (take()), never by onCommand(), which has one
// slot and belongs to the application.

#include <ModBeeMqtt.h>
#include <string.h>

#include "../OtaLink.h"
#include "../OtaManifest.h"
#include "../OtaProtocol.h"
#include "../OtaUpdater.h"

#ifndef MODBEE_OTA_SPARKPLUG_CHUNK
#define MODBEE_OTA_SPARKPLUG_CHUNK 2048
#endif

#define MODBEE_OTA_SP_OFFER    "Node Control/Firmware Offer"
#define MODBEE_OTA_SP_CHUNK    "Node Control/Firmware Chunk"
#define MODBEE_OTA_SP_COMMAND  "Node Control/Firmware Command"
#define MODBEE_OTA_SP_VERSION  "Firmware/Version"
#define MODBEE_OTA_SP_STATE    "Firmware/State"
#define MODBEE_OTA_SP_PROGRESS "Firmware/Progress"
#define MODBEE_OTA_SP_STATUS   "Firmware/Status"
#define MODBEE_OTA_SP_REQUEST  "Firmware/Request"

class OtaSparkplugLink : public OtaLink {
public:
  explicit OtaSparkplugLink(MqttSparkplugNode& node, uint16_t chunk = MODBEE_OTA_SPARKPLUG_CHUNK)
      : _node(node), _chunk(chunk) {}

  /**
   * Declares the metrics. Call BEFORE node.connect(): metrics are part of the
   * birth certificate. `version` is the running firmware's, for the birth.
   */
  bool begin(const char* version) {
    const bool declared =
        _node.declareWritable(MODBEE_OTA_SP_OFFER, MqttSparkplugDataType::String) &&
        _node.declareWritable(MODBEE_OTA_SP_CHUNK, MqttSparkplugDataType::Bytes) &&
        _node.declareWritable(MODBEE_OTA_SP_COMMAND, MqttSparkplugDataType::String) &&
        _node.declare(MODBEE_OTA_SP_VERSION, MqttSparkplugDataType::String) &&
        _node.declare(MODBEE_OTA_SP_STATE, MqttSparkplugDataType::String) &&
        _node.declare(MODBEE_OTA_SP_PROGRESS, MqttSparkplugDataType::Int32) &&
        _node.declare(MODBEE_OTA_SP_STATUS, MqttSparkplugDataType::String) &&
        _node.declare(MODBEE_OTA_SP_REQUEST, MqttSparkplugDataType::Bytes);
    if (!declared) return false;
    _offer = _node.indexOf(nullptr, MODBEE_OTA_SP_OFFER);
    _chunkIndex = _node.indexOf(nullptr, MODBEE_OTA_SP_CHUNK);
    _command = _node.indexOf(nullptr, MODBEE_OTA_SP_COMMAND);
    _state = _node.indexOf(nullptr, MODBEE_OTA_SP_STATE);
    _progress = _node.indexOf(nullptr, MODBEE_OTA_SP_PROGRESS);
    _status = _node.indexOf(nullptr, MODBEE_OTA_SP_STATUS);
    _request = _node.indexOf(nullptr, MODBEE_OTA_SP_REQUEST);
    const int versionIndex = _node.indexOf(nullptr, MODBEE_OTA_SP_VERSION);
    if (_offer < 0 || _chunkIndex < 0 || _command < 0 || _state < 0 || _progress < 0 ||
        _status < 0 || _request < 0 || versionIndex < 0) {
      return false;
    }
    _node.holdAt((size_t)_chunkIndex, true);
    // Copied: a value points at what the caller owns, and the caller's
    // string may not live as long as the node.
    copy(_version, sizeof(_version), version);
    _node.setAt((size_t)versionIndex, mq_spString(_version));
    _node.setAt((size_t)_state, mq_spString("idle"));
    _node.setAt((size_t)_progress, mq_spInt32(0));
    _ready = true;
    return true;
  }

  bool ready() override { return _ready && _node.isBorn(); }
  uint16_t maxChunk() const override { return _chunk; }
  uint32_t maxWindow() const override { return _chunk; }

  bool sendRequest(const OtaRequest& request) override {
    const size_t n = ota_encodeRequest(request, _requestFrame, sizeof(_requestFrame));
    if (!n) return false;
    _node.setAt((size_t)_request, mq_spBytes(_requestFrame, (uint32_t)n));
    // The same request asked twice is still a change the host must see.
    _node.reportAt((size_t)_request);
    return _node.publishChanged();
  }

  bool sendStatus(const OtaStatus& status) override {
    if (!ota_writeStatusJson(status, _statusJson, sizeof(_statusJson))) return false;
    _node.setAt((size_t)_status, mq_spString(_statusJson));
    _node.setAt((size_t)_state, mq_spString(ota_stateName(status.state)));
    _node.setAt((size_t)_progress,
                mq_spInt32((int32_t)ota_percent(status.received, status.size)));
    return _node.publishChanged();
  }

  void poll() override {
    if (!_ready || !_updater) return;
    MqttSparkplugValue value;
    if (_node.takeAt((size_t)_offer, value) && value.isText()) {
      OtaImage image;
      if (ota_parseOfferJson((const char*)value.as.bytes.data, value.as.bytes.length, image)) {
        _updater->handleOffer(*this, image);
      }
    }
    if (_node.takeAt((size_t)_chunkIndex, value) && value.isBytes()) {
      _updater->handleFrame(*this, value.as.bytes.data, value.as.bytes.length);
      // Not carried in a later birth.
      _node.setAt((size_t)_chunkIndex, mq_spBytes(nullptr, 0));
    }
    if (_node.takeAt((size_t)_command, value) && value.isText()) {
      OtaCommand command;
      if (ota_parseCommand((const char*)value.as.bytes.data, value.as.bytes.length, command)) {
        _updater->handleCommand(*this, 0, command);
      }
    }
    const bool born = _node.isBorn();
    if (born && !_wasBorn) _updater->publishStatus();
    _wasBorn = born;
  }

private:
  static void copy(char* out, size_t capacity, const char* in) {
    if (!in) in = "";
    size_t n = strlen(in);
    if (n >= capacity) n = capacity - 1;
    memcpy(out, in, n);
    out[n] = '\0';
  }

  MqttSparkplugNode& _node;
  uint16_t _chunk;
  bool _ready = false;
  bool _wasBorn = false;
  int _offer = -1, _chunkIndex = -1, _command = -1;
  int _state = -1, _progress = -1, _status = -1, _request = -1;
  char _version[MODBEE_OTA_VERSION_MAX] = {};
  char _statusJson[320] = {};
  uint8_t _requestFrame[MODBEE_OTA_REQUEST_SIZE] = {};
};
