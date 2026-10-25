# ModBee OTA

Firmware updates for ModBee nodes over MQTT and Sparkplug B. The node pulls
the image, verifies it, and rolls it back if the new image doesn't come up.
Nothing waits: `update()` does a bounded slice of work, so it runs inside a
PLC scan.

```cpp
#include <WiFi.h>
#include <ModBeeMqtt.h>
#include <ModBeeOta.h>
#include <links/OtaMqttLink.h>

WiFiClient net;
MqttDevice mqtt(net);
OtaEsp32Target flash;                     // the ESP32's A/B slots
OtaAsyncTarget target(flash);             // flash work off the caller...
OtaEsp32Worker worker(target);            // ...on a lowest-priority task
OtaEsp32Store store;                      // the job, across reboots
OtaUpdater ota(target);
OtaMqttDeviceLink otaLink(mqtt, "modbee/node-1/ota");

void setup() {
  // ... WiFi, mqtt.connect(BROKER, "node-1") ...
  OtaSettings settings;
  settings.board = "my-board";
  settings.version = "1.0.0";
  worker.begin();
  ota.store(&store);
  ota.addLink(otaLink);
  ota.begin(settings);
}

void loop() {
  mqtt.poll();
  ota.update();     // a bounded slice of work; never waits
}
```

## What it does

- **Pulls.** The node asks for a byte range; the server answers with
  CRC-checked chunks. A lost message costs one request, a dropped link resumes
  where it stopped, and the server can't overrun the node.
- **Verifies before it switches.** It checks each chunk's CRC-32, the whole
  image's SHA-256, an optional Ed25519 or ECDSA P-256 signature, board and
  version, and the chip's own image check.
- **Rolls back.** The new image boots on trial and confirms itself once it
  can reach MQTT again. If it crashes, hangs or never connects, the old one
  comes back, and it reports `rolled_back` for the job.
- **Safe beside a PLC.** On the ESP32 all flash work runs on a lowest-priority
  worker, and `update()` returns in microseconds. Measured on the Node UIO
  with probe tasks on both cores, the only stall left is ~50 ms once per
  update, when the new image confirms. Every event is also polled state.
- **Made for slow links.** The timeout learns the link's round trip, a stall
  waits instead of failing, and a power cut resumes where it stopped.
  NB-IoT-like links cost 1.01× the image.
- **Isn't tied to the chip.** ESP32 uses its own OTA. Other chips implement an
  `OtaStorage` and an `OtaBootControl`, and `OtaStagedTarget` does the rest.
- **Isn't tied to the transport.** Links exist for `MqttDevice`, a bare
  `MqttClient` (as OpenPLC's `MQTT_CLIENT` block holds it), and Sparkplug B.
  One binary frame format runs on all of them, so a gateway can relay frames
  without decoding them.

## Requirements

- **ESP32** (any variant) with the [Arduino-ESP32](https://github.com/espressif/arduino-esp32)
  3.x core, and a partition table with two OTA slots. The core's default and
  "Minimal SPIFFS" schemes both have them.
- **[ModBee MQTT](https://github.com/mod-bee/modbee-mqtt)** for the MQTT and
  Sparkplug links. The core library builds without it.
- The `NodeUio_MQTT` example also needs
  [ModBee Node UIO](https://github.com/mod-bee/modbee-node-uio).

The core (`src/*.cpp`, `src/staged/`) is plain C++14 with no Arduino or
ESP-IDF code, so it builds for any target and on a desktop.

## Installation

**Arduino IDE:** download this repository as a ZIP and use *Sketch → Include
Library → Add .ZIP Library*, or clone it into your `libraries` folder. Install
ModBee MQTT the same way.

**PlatformIO:** add both to `platformio.ini`:

```ini
lib_deps =
    https://github.com/mod-bee/modbee-mqtt
    https://github.com/mod-bee/modbee-ota
```

## Sending an update

The library is the node side. The server is anything that speaks the protocol
in [docs/PROTOCOL.md](docs/PROTOCOL.md) over the same broker:

1. Publish a JSON offer (size, SHA-256, board, version, optional signature)
   to `<base>/offer`, retained.
2. Answer each Request frame the node publishes on `<base>/req` with Chunk
   frames on `<base>/data`.
3. Watch `<base>/status` for progress and the outcome.

That's a few dozen lines in any language with an MQTT client. On a device,
`OtaServer` with an `OtaImageSource` is the same server side in C++, for a
gateway feeding the nodes behind it.

To accept only signed images, give the updater a verifier with
`ota.verifier(&verifier)`: `OtaEd25519Verifier` on the ESP32, or
`OtaEcdsaP256Verifier` where wolfSSL is available. Unsigned offers are then
refused. If the verifier is attached after `begin()`, set
`settings.requireSignature` so nothing unsigned gets in before it. What is
signed, and how, is in [docs/PROTOCOL.md](docs/PROTOCOL.md#the-signature).

## Examples

- [`NodeUio_MQTT`](examples/NodeUio_MQTT/NodeUio_MQTT.ino): a Node UIO over
  MQTT, with the flash work on a background worker and optional signed images.
- [`Esp32_Sparkplug`](examples/Esp32_Sparkplug/Esp32_Sparkplug.ino): an ESP32
  Sparkplug B edge node updated through its own metrics.

## Docs

- [Architecture](docs/ARCHITECTURE.md): the parts, the job lifecycle, why
  pull, PLC timing (measured), slow links, and the safety layers
- [Protocol](docs/PROTOCOL.md): frames, topics, JSON and signatures, which is
  everything a server implements
- [Porting](docs/PORTING.md): adding STM32, RP2040 and other chips
- [Roadmap](docs/ROADMAP.md): the BeeBus gateway relay and what comes next

## Layout

```
src/              core: updater, protocol, hashes, server (no platform code)
src/staged/       OtaStagedTarget - the kit for chips without native OTA
src/esp32/        ESP32 target, NVS store, Ed25519 verifier, background worker
src/links/        MQTT and Sparkplug links (header-only, opt-in)
src/verifiers/    ECDSA P-256 verifier over wolfSSL (header-only, opt-in)
examples/         Node UIO over MQTT; ESP32 Sparkplug edge node
```

## Configuration

Limits are compile-time macros in [`src/OtaConfig.h`](src/OtaConfig.h), each
behind `#ifndef`, so a build overrides them with `-D` without editing the
library: chunk size, buffer and peer counts, string lengths. Run-time settings
(buffer size, timeouts, confirm window, accept and apply policy) are fields of
`OtaSettings` in [`src/OtaUpdater.h`](src/OtaUpdater.h).

## Building the core on a desktop

The core builds with CMake on Linux, macOS and Windows, for host-side tools
or to link into another project:

```sh
cmake -S . -B build
cmake --build build
```

## License

GPL-3.0. See [LICENSE](LICENSE).
