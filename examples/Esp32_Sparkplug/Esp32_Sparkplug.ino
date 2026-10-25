// A Sparkplug B edge node that can be updated through its own metrics.
//
// The Host Application writes the JSON offer to "Node Control/Firmware Offer",
// then answers each "Firmware/Request" the node reports with one chunk written
// to "Node Control/Firmware Chunk". docs/PROTOCOL.md has the frame layouts.
//
// The OTA link takes its commands by polling, so the application keeps
// onCommand() for itself.

#include <WiFi.h>
#include <time.h>
#include <ModBeeMqtt.h>
#include <ModBeeOta.h>
#include <links/OtaSparkplugLink.h>

#define FIRMWARE_VERSION "1.0.0"

const char* WIFI_NAME = "plant-wifi";
const char* WIFI_PASSWORD = "";
const char* BROKER = "192.168.1.10";

WiFiClient net;
MqttSparkplugNode node(net);

OtaEsp32Target target;
OtaEsp32Store store;
OtaUpdater ota(target);
OtaSparkplugLink otaLink(node);

void onCommand(const MqttSparkplugCommand& command) {
  Serial.printf("command: %s\n", command.name);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(250);
  configTime(0, 0, "pool.ntp.org");
  while (time(nullptr) < 1700000000) delay(250);
  node.epochNow((uint64_t)time(nullptr) * 1000);

  node.group("Plant");
  node.node("Tank-01");
  node.server(BROKER);
  node.declare("Tank/Level", MqttSparkplugDataType::Float);
  otaLink.begin(FIRMWARE_VERSION);   // before connect(): the metrics go in the birth
  node.onCommand(&onCommand);
  node.connect();

  OtaSettings settings;
  settings.board = "esp32-tank";
  settings.version = FIRMWARE_VERSION;
  ota.store(&store);
  ota.addLink(otaLink);
  ota.begin(settings);
}

void loop() {
  node.poll();
  ota.update();

  static unsigned long lastSent = 0;
  if (millis() - lastSent >= 1000) {
    lastSent = millis();
    node.set("Tank/Level", mq_spFloat(analogRead(A0) * (5.0f / 4095.0f)));
    node.publishChanged();
  }
}
