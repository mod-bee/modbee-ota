#pragma once

// OTA over ModBee MQTT. Header only, and included on purpose:
//
//   #include <ModBeeMqtt.h>
//   #include <links/OtaMqttLink.h>
//
// so the core builds without the MQTT library installed, and a sketch that
// uses this link compiles it in its own translation unit, where ModBeeMqtt.h
// is certainly on the include path.
//
// TOPICS, under a base the application chooses (one per node):
//
//   <base>/offer    in   JSON offer, or a binary Offer frame. May be retained.
//   <base>/data     in   binary Chunk frames
//   <base>/cmd      in   "abort" | "apply" | "confirm" | "rollback" | "status",
//                        or a binary Command frame
//   <base>/req      out  binary Request frames
//   <base>/status   out  JSON status, retained
//
// Two links, for the two ways a node holds MQTT:
//
//   OtaMqttDeviceLink   over MqttDevice - a sketch. Uses MqttDevice's own
//                       subscription slots, so the application's are untouched.
//   OtaMqttClientLink   over a bare MqttClient - what an OpenPLC MQTT_CLIENT
//                       block hands out. Attaches to it as a layer
//                       (MqttClientHook), so the application's handlers and
//                       the other layers on the connection - a Sparkplug node
//                       sharing it - see everything that is not OTA.

#include <ModBeeMqtt.h>
#include <string.h>

#include "../OtaLink.h"
#include "../OtaManifest.h"
#include "../OtaProtocol.h"
#include "../OtaUpdater.h"

#ifndef MODBEE_OTA_TOPIC_MAX
#define MODBEE_OTA_TOPIC_MAX 128
#endif

// Chunk payload per message. The default MQTT buffer is 4096 bytes in each
// direction; 2 kB of payload leaves room for the topic, the frame header and
// MQTT 5 properties. Raise the buffer (bufferSize()) before raising this.
#ifndef MODBEE_OTA_MQTT_CHUNK
#define MODBEE_OTA_MQTT_CHUNK 2048
#endif

// JSON status is short, but board and version are the application's strings.
#define MODBEE_OTA_STATUS_JSON_MAX 320

/** The five topics, built once from the base. */
class OtaMqttTopics {
public:
  enum Inbound : uint8_t { Offer = 0, Data, Command, None };

  bool set(const char* base) {
    if (!base || !base[0]) return false;
    return build(_offer, base, "/offer") && build(_data, base, "/data") &&
           build(_cmd, base, "/cmd") && build(_req, base, "/req") &&
           build(_status, base, "/status");
  }

  const char* offer() const { return _offer; }
  const char* data() const { return _data; }
  const char* command() const { return _cmd; }
  const char* request() const { return _req; }
  const char* status() const { return _status; }

  /** Which inbound topic this is, from a topic that need not be terminated. */
  Inbound match(const char* topic, size_t length) const {
    if (same(_data, topic, length)) return Data;  // the busy one first
    if (same(_offer, topic, length)) return Offer;
    if (same(_cmd, topic, length)) return Command;
    return None;
  }

private:
  static bool build(char* out, const char* base, const char* suffix) {
    const size_t a = strlen(base), b = strlen(suffix);
    if (a + b >= MODBEE_OTA_TOPIC_MAX) return false;
    memcpy(out, base, a);
    memcpy(out + a, suffix, b + 1);
    return true;
  }
  static bool same(const char* ours, const char* topic, size_t length) {
    return strlen(ours) == length && memcmp(ours, topic, length) == 0;
  }

  char _offer[MODBEE_OTA_TOPIC_MAX] = {};
  char _data[MODBEE_OTA_TOPIC_MAX] = {};
  char _cmd[MODBEE_OTA_TOPIC_MAX] = {};
  char _req[MODBEE_OTA_TOPIC_MAX] = {};
  char _status[MODBEE_OTA_TOPIC_MAX] = {};
};

/** What both MQTT links do with an inbound message once they know its topic. */
inline void ota_mqttDeliver(OtaLink& link, OtaMqttTopics::Inbound which,
                            const uint8_t* bytes, size_t length) {
  OtaUpdater* updater = link.updater();
  if (!updater || !bytes || length == 0) return;
  switch (which) {
    case OtaMqttTopics::Offer: {
      // JSON from a platform, or a binary frame relayed from elsewhere.
      if (bytes[0] == '{') {
        OtaImage image;
        if (ota_parseOfferJson((const char*)bytes, length, image)) {
          updater->handleOffer(link, image);
        }
      } else {
        updater->handleFrame(link, bytes, length);
      }
      break;
    }
    case OtaMqttTopics::Data:
      updater->handleFrame(link, bytes, length);
      break;
    case OtaMqttTopics::Command: {
      OtaCommand command;
      if (ota_parseCommand((const char*)bytes, length, command)) {
        updater->handleCommand(link, 0, command);
      } else {
        updater->handleFrame(link, bytes, length);
      }
      break;
    }
    case OtaMqttTopics::None:
      break;
  }
}

/** OTA over an MqttDevice. */
class OtaMqttDeviceLink : public OtaLink {
public:
  /**
   * `base` is copied. Subscriptions are registered on the first poll(), which
   * the updater makes - there is nothing else to call.
   */
  OtaMqttDeviceLink(MqttDevice& mqtt, const char* base,
                    uint16_t chunk = MODBEE_OTA_MQTT_CHUNK)
      : _mqtt(mqtt), _chunk(chunk) {
    _valid = _topics.set(base);
  }

  /** False when the base is too long or MqttDevice had no slot left. */
  bool valid() const { return _valid; }
  const OtaMqttTopics& topics() const { return _topics; }

  bool ready() override { return _valid && _registered && _mqtt.isConnected(); }
  uint16_t maxChunk() const override { return _chunk; }

  bool sendRequest(const OtaRequest& request) override {
    uint8_t frame[MODBEE_OTA_REQUEST_SIZE];
    const size_t n = ota_encodeRequest(request, frame, sizeof(frame));
    // A series, not a value: each request is a different range, and a plain
    // publish would let a newer one replace an older one still queued.
    return n && _mqtt.publishSeries(_topics.request(), frame, n, MQ_QOS_1, false);
  }

  bool sendStatus(const OtaStatus& status) override {
    char json[MODBEE_OTA_STATUS_JSON_MAX];
    if (!ota_writeStatusJson(status, json, sizeof(json))) return false;
    return _mqtt.publish(_topics.status(), json, MQ_QOS_1, true);
  }

  void poll() override {
    if (_valid && !_registered) {
      _valid = _mqtt.onMessage(_topics.offer(), &OtaMqttDeviceLink::received, this, MQ_QOS_1) &&
               _mqtt.onMessage(_topics.data(), &OtaMqttDeviceLink::received, this, MQ_QOS_1) &&
               _mqtt.onMessage(_topics.command(), &OtaMqttDeviceLink::received, this, MQ_QOS_1);
      _registered = true;
    }
    // A (re)connect republishes status: the retained one may be stale, and a
    // server waiting on this node learns it is back.
    const bool up = _mqtt.isConnected();
    if (up && !_wasUp && _updater) _updater->publishStatus();
    _wasUp = up;
  }

private:
  static void received(const char* topic, const MqttPayload& payload, void* context) {
    OtaMqttDeviceLink* self = static_cast<OtaMqttDeviceLink*>(context);
    const OtaMqttTopics::Inbound which = self->_topics.match(topic, strlen(topic));
    ota_mqttDeliver(*self, which, payload.bytes(), payload.length());
  }

  MqttDevice& _mqtt;
  OtaMqttTopics _topics;
  uint16_t _chunk;
  bool _valid = false;
  bool _registered = false;
  bool _wasUp = false;
};

/** OTA over a bare MqttClient, as one layer among any others on it. */
class OtaMqttClientLink : public OtaLink, private MqttClientHook {
public:
  OtaMqttClientLink(MqttClient& client, const char* base,
                    uint16_t chunk = MODBEE_OTA_MQTT_CHUNK)
      : _client(client), _chunk(chunk) {
    _valid = _topics.set(base);
    if (_valid) _client.attach(*this);
  }

  ~OtaMqttClientLink() override { _client.detach(*this); }

  OtaMqttClientLink(const OtaMqttClientLink&) = delete;
  OtaMqttClientLink& operator=(const OtaMqttClientLink&) = delete;
  bool valid() const { return _valid; }
  const OtaMqttTopics& topics() const { return _topics; }

  /**
   * Asked before each OTA message is delivered; false drops it. Delivery
   * happens inside the client's poll(), on whatever thread polls - a gate
   * lets the owner check that is the thread it expects (an OpenPLC block
   * placed in a different task from its MQTT_CLIENT) instead of racing.
   */
  void gate(bool (*allow)(void* context), void* context) {
    _gate = allow;
    _gateContext = context;
  }

  bool ready() override { return _valid && _subscribed && _client.isConnected(); }
  uint16_t maxChunk() const override { return _chunk; }

  bool sendRequest(const OtaRequest& request) override {
    uint8_t frame[MODBEE_OTA_REQUEST_SIZE];
    const size_t n = ota_encodeRequest(request, frame, sizeof(frame));
    // A series, not a value: see OtaMqttDeviceLink::sendRequest().
    return n && mq_accepted(_client.publishSeries(_topics.request(), frame, n, MQ_QOS_1, false));
  }

  bool sendStatus(const OtaStatus& status) override {
    char json[MODBEE_OTA_STATUS_JSON_MAX];
    const size_t n = ota_writeStatusJson(status, json, sizeof(json));
    return n && mq_accepted(_client.publish(_topics.status(), (const uint8_t*)json, n,
                                            MQ_QOS_1, true));
  }

  void poll() override {
    if (!_valid) return;
    // Subscribe on every connect. Harmless when the broker kept the session,
    // necessary when it did not, and nothing to track on the client.
    const bool up = _client.isConnected();
    if (!up) _subscribed = false;
    if (up && !_subscribed) {
      _subscribed = mq_accepted(_client.subscribe(_topics.offer(), MQ_QOS_1)) &&
                    mq_accepted(_client.subscribe(_topics.data(), MQ_QOS_1)) &&
                    mq_accepted(_client.subscribe(_topics.command(), MQ_QOS_1));
      if (_subscribed && _updater) _updater->publishStatus();
    }
  }

private:
  // The client's inbound messages (MqttClientHook). OTA's own are taken -
  // nobody else has a use for a firmware chunk - and the rest pass on.
  bool message(const MqttMessage& message) override {
    const OtaMqttTopics::Inbound which =
        _topics.match((const char*)message.topic.data, message.topic.length);
    if (which == OtaMqttTopics::None) return false;
    if (_gate && !_gate(_gateContext)) return true;
    ota_mqttDeliver(*this, which, message.payload.data, message.payload.length);
    return true;
  }
  MqttClient& _client;
  OtaMqttTopics _topics;
  uint16_t _chunk;
  bool (*_gate)(void*) = nullptr;
  void* _gateContext = nullptr;
  bool _valid = false;
  bool _subscribed = false;
};
