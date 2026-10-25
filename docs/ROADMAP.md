# Roadmap

## Phase 1: Node UIO over MQTT (0.1.0)

- [x] Core engine, pull protocol, SHA-256, CRC-32, JSON offer and status
- [x] `OtaEsp32Target`: A/B slots, sequential erase, trial boot and rollback
- [x] `OtaEsp32Store` (NVS) and `OtaEd25519Verifier` (libsodium)
- [x] `OtaMqttDeviceLink` (sketches) and `OtaMqttClientLink` (OpenPLC's raw client)
- [x] `OtaSparkplugLink`, using polled metrics and leaving the application's `onCommand()` slot free
- [x] `OtaStagedTarget` + `OtaStorage` + `OtaBootControl`: the kit for non-ESP32 chips
- [x] `OtaServer` + `OtaImageSource`: the sender side, ready for a gateway
- [x] Tested on the host (loss, corruption, outages, every refusal, rollback
      paths, bounded work, multiple nodes) and end to end over a real broker
- [x] **Tested on hardware (Node UIO):**
  - update, refusals (same version, downgrade, wrong board), abort;
  - power cut mid-download (resumes);
  - an image that can't reach MQTT rolls back after the confirm window;
  - an image that crashes rolls back via the bootloader;
  - signed images: unsigned and wrong-key offers refused, the correct key installs;
  - a throttled, high-latency link.
- [x] `OtaAsyncTarget` + `OtaEsp32Worker`: flash work off the caller.
      Measured: PLC-like tasks on both cores unaffected except one ~50 ms
      stall at confirm (docs/ARCHITECTURE.md)
- [x] Slow links: adaptive timeout, backoff, stall time limit, small restart
      window; resume after a power cut

## Phase 2: Through a gateway, over BeeBus

The gateway is a node with an `OtaUpdater` for itself and an `OtaServer` for
the nodes behind it:

```
 platform ──MQTT──► gateway ──BeeBus──► node 7
                    ├─ OtaUpdater + OtaMqttLink          (its own firmware)
                    └─ OtaServer + OtaBeeBusServerLink   (everyone else's)
                         └─ OtaRelaySource ──MQTT──► platform
```

The planned pieces:

- **`OtaBeeBusLink`** (node side) and **`OtaBeeBusServerLink`** (gateway side).
  BeeBus has no binary blob type, and its reliable topic layer keeps only the
  latest value. The pull protocol already tolerates that. The link carries
  frames either as Modbus FC16 register writes (246 binary bytes, the better
  fit) or as a base64 string topic (≤ 255 characters, about 180 bytes of
  payload). `maxWindow()` is one chunk. Expect about one chunk per one or two
  token rotations.
- **`OtaRelaySource`**: an `OtaImageSource` whose `read()` returns `Pending`
  while it fetches a 2–4 kB block from upstream, then serves the 200-byte slices
  BeeBus wants from that block. The gateway never holds more than one block.
  The upstream is the platform over MQTT, using the same request/chunk frames
  under a topic for the downstream node
  (`modbee/<gateway>/ota/relay/<node>/...`).
- **Store-and-forward** (optional): to update many nodes with one image,
  the gateway stages it once into its own idle OTA slot (`esp_partition_*`)
  and serves every node from flash. The Node UIO has 1.875 MB of slot it's
  not using unless it updates itself.
- **Routing**: the platform addresses `gateway + node id`. The gateway maps
  that to a BeeBus node and an `OtaServer` peer, and passes status back up
  unchanged. Status is the same JSON a direct node would publish.

## Phase 3: OpenPLC blocks

In [modbee-openplc](https://github.com/mod-bee/modbee-openplc), under
`modbee-communication/modbee-ota`.

- [x] `OTA_NODE`: the updater over an `MQTT_CLIENT`, settings and commands as
      pins, status out. Refuses messages from another task instead of
      racing. Flushes retained values before the reboot
- [x] `OTA_SIGNING`: ECDSA P-256 through the MQTT library's wolfSSL; a
      project without it links none of it
- [x] Datatypes `OTA_STATE`, `OTA_RESULT`, `OTA_OUTCOME`, `OTA_CONFIRM`,
      mirrored with `static_assert`s
- [x] On the Node UIO: OpenPLC firmware updated itself over MQTT and
      confirmed. The PLC kept running throughout. Costs ~26 kB of flash
- [x] **OTA beside `SPARKPLUG_NODE`, on one connection**: modbee-mqtt's
      `MqttClient` takes any number of layers (`MqttClientHook`); a Sparkplug
      node and `OtaMqttClientLink` are two of them on the same `MQTT_CLIENT`.
      The image travels on the OTA topics; `OTA_SPARKPLUG` reports the update
      as metrics and takes `Node Control/Firmware ...` commands
- [ ] Logic-task releases skipped during a download (one per erased
      sector): an Arduino core built with flash auto-suspend would remove it
- [ ] Block reference doc generated from the blocks, as modbee-mqtt does

## Later

- **`OtaHttpLink`**: for when the platform hands over a URL instead of serving
  chunks. It's another `OtaLink`, a range-request HTTP(S) client.
- **A smaller verifier**: `OtaEd25519Verifier` pulls libsodium in, which costs
  about 97 kB of flash on the S3. A Monocypher-based verifier (about 10 kB)
  would fit the same interface.
- **Compression**: a gzip/heatshrink image is ~40 % smaller, which matters on
  metered links. It means a decompressor between the ring and the target.
- **Delta images**, if link time on a slow mesh ever becomes the bottleneck.
- **STM32 / RP2040 targets**: see [PORTING.md](PORTING.md).
