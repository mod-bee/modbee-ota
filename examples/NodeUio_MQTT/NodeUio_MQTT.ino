// A Node UIO that can be updated over MQTT.
//
// The server publishes an offer to modbee/node-uio-1/ota/offer and answers the
// node's requests on .../req with chunks on .../data (docs/PROTOCOL.md). The
// node pulls the image, checks it, reboots into it, and confirms it once it is
// back on MQTT. If it never gets back on MQTT, it goes back to this firmware
// by itself.
//
// Needs a partition table with two OTA slots: the OpenPLC board package's, or
// the Arduino core's "Minimal SPIFFS" scheme.

#include <WiFi.h>
#include <ModBeeMqtt.h>
#include <ModBeeOta.h>
#include <links/OtaMqttLink.h>
#include <NodeUioAPI.h>

// What this build says it is. Bump it for every image you push: an offer for
// the version already running is refused (unless forced), and an older one is
// refused as a downgrade.
//
// Every setting here can come from the build instead (-D FIRMWARE_VERSION=...),
// so credentials stay out of the source.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0.0"
#endif
#ifndef WIFI_NAME
#define WIFI_NAME "plant-wifi"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef BROKER
#define BROKER "192.168.1.10"
#endif
#ifndef NODE_ID
#define NODE_ID "node-uio-1"
#endif
// How long a new image has to reach MQTT before it is rolled back.
#ifndef CONFIRM_WINDOW_MS
#define CONFIRM_WINDOW_MS 120000
#endif

// Signed images only. Make an Ed25519 key pair once, paste the raw 32-byte
// public key here as a C array, set this true, and sign every offer with the
// private key (docs/PROTOCOL.md, "The signature").
// (Or from the build: -D OTA_REQUIRE_SIGNED=true and OTA_PUBLIC_KEY_BYTES.)
#ifndef OTA_REQUIRE_SIGNED
#define OTA_REQUIRE_SIGNED false
#endif
#ifndef OTA_PUBLIC_KEY_BYTES
#define OTA_PUBLIC_KEY_BYTES {0}
#endif
const bool REQUIRE_SIGNED = OTA_REQUIRE_SIGNED;
const uint8_t OTA_PUBLIC_KEY[32] = OTA_PUBLIC_KEY_BYTES;

WiFiClient net;
MqttDevice mqtt(net);

OtaEsp32Target flash;          // the ESP32's own A/B slots and rollback
OtaAsyncTarget target(flash);  // flash work off the loop: erases take ~50 ms
OtaEsp32Worker worker(target); // ...done here, at the lowest priority
OtaEsp32Store store;           // remembers the job across the install reboot
OtaUpdater ota(target);
OtaMqttDeviceLink otaLink(mqtt, "modbee/" NODE_ID "/ota");
OtaEd25519Verifier verifier(OTA_PUBLIC_KEY);

void onOta(void*, OtaEvent event, OtaUpdater& updater) {
  switch (event) {
    case OtaEvent::Started:
      Serial.printf("OTA: downloading %s (%lu bytes)\n", updater.image().version,
                    (unsigned long)updater.size());
      break;
    case OtaEvent::Applying:
      // The last moment before the reboot: keep what must survive it.
      uio().saveRetain();
      Serial.println("OTA: verified, rebooting into the new image");
      break;
    case OtaEvent::Testing:
      Serial.println("OTA: running a new image on trial");
      break;
    case OtaEvent::Succeeded:
      Serial.printf("OTA: job %lu installed\n", (unsigned long)updater.job());
      break;
    case OtaEvent::Failed:
      Serial.printf("OTA: job %lu failed: %s\n", (unsigned long)updater.job(),
                    ota_resultName(updater.result()));
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  uio().begin();
  uio().connect(WIFI_NAME, WIFI_PASSWORD);

  mqtt.statusTopic("modbee/" NODE_ID "/status");
  mqtt.connect(BROKER, NODE_ID);   // returns at once; poll() finishes it

  OtaSettings settings;
  settings.board = "modbee-node-uio";
  settings.version = FIRMWARE_VERSION;
  settings.confirmWindowMs = CONFIRM_WINDOW_MS;
  // The defaults: confirm once MQTT is back, roll back if that takes more
  // than two minutes, apply as soon as verified.
  worker.begin();
  ota.store(&store);
  if (REQUIRE_SIGNED) ota.verifier(&verifier);
  ota.addLink(otaLink);
  ota.onEvent(&onOta);
  ota.begin(settings);

  const esp_partition_t* running = esp_ota_get_running_partition();
  Serial.printf("Firmware %s, running from %s, broker %s\n", FIRMWARE_VERSION,
                running ? running->label : "?", BROKER);
}

void loop() {
  mqtt.poll();
  uio().update();
  ota.update();

  static unsigned long last = 0;
  if (ota.state() == OtaState::Downloading && millis() - last >= 2000) {
    last = millis();
    Serial.printf("OTA: %u%%\n", ota.progress());
  }
}
