# Architecture

ModBee OTA updates a node's firmware over whatever link reaches it. It has
three kinds of part: one engine, and two kinds of plug-in that keep the engine
independent of hardware and transport.

```
                server side                               node side
   ┌──────────────────────────────┐           ┌──────────────────────────────────┐
   │ platform (any MQTT server)   │           │            OtaUpdater            │
   │ or OtaServer (gateway, test) │           │  offer → check → pull → verify → │
   └──────────────┬───────────────┘           │  activate → reboot → confirm     │
                  │ offer, chunks, commands   └───────┬──────────────────┬───────┘
                  │◄──── requests, status ────        │                  │
                  ▼                                   ▼                  ▼
          ┌───────────────┐   one wire format   ┌──────────┐      ┌─────────────┐
          │  any broker / │◄───────────────────►│ OtaLink  │      │  OtaTarget  │
          │  bus / cable  │                     └──────────┘      └─────────────┘
          └───────────────┘                      MQTT              ESP32 A/B slots
                                                 Sparkplug B       Staged + bootloader
                                                 BeeBus (next)     (STM32, RP2040, ...)
```

| Part | What it is | Exists today |
|---|---|---|
| `OtaUpdater` | The node's engine. A state machine that does a bounded slice of work per `update()`. | yes |
| `OtaTarget` | Where the image goes. One per kind of microcontroller. | `OtaEsp32Target`, `OtaStagedTarget` |
| `OtaLink` | How the node is reached. One per transport. | `OtaMqttDeviceLink`, `OtaMqttClientLink`, `OtaSparkplugLink` |
| `OtaVerifier` | Signature check. | `OtaEd25519Verifier` (ESP32, libsodium), `OtaEcdsaP256Verifier` (wolfSSL) |
| `OtaStore` | Remembers a job across the install reboot. | `OtaEsp32Store` (NVS) |
| `OtaServer` | The sender side, for a gateway or a node feeding a neighbour. | yes |
| `OtaImageSource` | Where a server reads an image. It may answer *later*, which is what makes a relay possible. | `OtaMemorySource` |

The core (`src/*.cpp`, `src/staged/`) has no Arduino or ESP-IDF code in it and
is tested on the host. Platform code lives only in `src/esp32/` and in the
header-only links (`src/links/`), which are included after the library they
adapt.

## The life of a job

```
 Idle ──offer──► [Offered] ──accept──► Downloading ──all bytes──► Verifying
   ▲               │ reject                │ abort/timeout/IO          │ hash, image check
   │               ▼                       ▼                           ▼
   │             Idle (Failed: refused)  Idle (Failed: reason)       Ready ──apply──► Applying
   │                                                                                    │ boot slot switched,
   │                                                                                    │ record saved, reboot
   │                                                                                    ▼
   └──── confirm (Success) ◄──── Testing ◄───── new image boots on trial ◄────── (reboot)
                                   │ window expires / app rejects
                                   ▼
                          rollback → old image boots → reports RolledBack / NotConfirmed
```

- **Offered** and **Ready** are only visited when the application asks for
  them (`autoAccept = false`, `autoApply = false`, or `"apply":"manual"` in the
  offer). A PLC can hold an update until the machine is idle.
- **Checking** comes between the offer and the download when a verifier is
  set: the signature is checked before a byte is pulled, and a bad one ends
  the job as a refusal.
- **Abort** works up to Ready. Once Applying, the boot slot is switching and
  `abort()` answers `Busy`.
- **Refusals are outcomes.** A wrong board, a downgrade or a bad signature is
  reported in a status with the job's number, so the server needs nothing
  more than the status topic to learn why.
- **The record** (`OtaStore`) is written once before the reboot and once when
  the outcome is known. After a rollback, the *old* firmware reads it and
  reports `rolled_back` for the job. Without a record, the server would see
  the old version come back and have to guess why.
- **No retry loops.** A job that installed or rolled back is never installed
  again under the same job number. A retained offer cannot cause an
  install, roll back, install cycle. Transient failures (timeout, bad hash)
  can be retried with the same number.

## Why the node pulls

The node asks for `[offset, offset + length)` and the server answers with
chunks that carry their own offset and CRC-32. Everything else follows from
that:

- **A lost message costs one request.** A gap in the offsets is noticed at
  once and asked for again; no chunk is ever "acknowledged".
- **Resume is free.** A link that drops pauses the job (no timeout runs while
  `ready()` is false), and the next request continues where the node is.
  A server that restarts loses nothing: the node's next request rebuilds its
  state.
- **Flow control is the node's buffer.** The node never asks for more than its
  ring has room for. The server can't overrun it, and a slow flash write
  slows the transfer instead of dropping bytes.
- **Latest-value links are safe.** BeeBus topics and Sparkplug metrics keep
  only the newest write. A superseded request is harmless, because the next
  one asks from the right place. Those links set `maxWindow()` to one chunk.
- **A gateway can relay without storing.** It forwards a node's requests
  upstream (or serves them from a small block cache) and never needs the whole
  image. See [ROADMAP.md](ROADMAP.md).

## Running beside a PLC: measured on the Node UIO

`update()` never waits on the network, and on the ESP32 it never waits on
flash either. Getting there took measurement, because ESP32 flash has three
behaviours that matter to a PLC. All were measured on the Node UIO (ESP32-S3,
Arduino core 3.3, ESP-IDF 5.5), with PLC-like probe tasks on both cores: tiny,
data-cache-missing, and instruction-cache-missing.

| What | Measured | How the library handles it |
|---|---|---|
| A 4 kB sector erase, an image check, a slot switch | 45–345 ms of CPU | On a lowest-priority worker (`OtaAsyncTarget` + `OtaEsp32Worker`). The caller does a memcpy |
| Every rewrite of **otadata**, the boot record | **both cores frozen 45–55 ms** | Only ever written at boot, before `setup()` (`ota_esp32Boot()`). The slot switch goes through NVS, confirm is a software trial |
| The **first erase after the program starts** | **both cores frozen 40–50 ms** | Spent on purpose on a blank sector (`ota_esp32WarmUp()`), in OpenPLC in the block's first scan, which the watchdog does not time |

What a PLC sees, measured with OpenPLC on the Node UIO in a typical layout
(logic task at T#20ms priority 0; MQTT and OTA at T#50ms priority 20),
pushing a 1 MB update:

| | Logic task (20 ms) | Comms task (50 ms) |
|---|---|---|
| Idle | scan max 0.13 ms, 0 overruns | scan max 1.6 ms |
| 40 KB/s of MQTT traffic, no flash | scan max 0.14 ms, **0 overruns** | scan max 6.5 ms |
| OTA download + verify (~25 s) | scan max 2.6 ms, **309 skipped releases of 2,030** | scan max 77 ms, limit 500 ms |
| Switch, reboot, confirm | the PLC restarts, as any firmware update must | |

The PLC never faulted, and the logic's own scans stayed short. But **each
sector erase made the logic task miss about one release**: the flash work
itself, since the same traffic without it caused none. The stock Arduino core
is built without `CONFIG_SPI_FLASH_AUTO_SUSPEND`, so an erase holds the
system up for its whole ~45 ms. Until a core with auto-suspend is in use:

- **let the program choose when.** `AUTO_ACCEPT := FALSE` and `ACCEPT` only
  while the machine is in a safe state, which is industry practice for
  firmware updates anyway;
- **keep OTA in the comms task, never the logic task,** at T#50ms, which is
  the layout tested. Network calls in
  that task stretched to ~200 ms during a download, and the watchdog stops a
  task stuck for 10 periods;
- **an earlier test layout tripped the watchdog**: MQTT and OTA in a 10 ms
  task, with `MQTT_CLIENT` also bringing up WiFi. That configuration is not
  suitable for OTA.

The updater's own ring (`bufferSize`, default 8 kB) is allocated when a job
starts and freed when it ends; `OtaAsyncTarget` keeps its own from the first
job on.

## Slow and unreliable links

ModBee nodes are reached over cellular, NB-IoT and radio as well as WiFi. Every
repeated byte costs airtime and money there, so the transfer is built to
learn the link instead of assuming it:

- **The request timeout adapts.** The updater times requests (RFC 6298, as TCP
  does) and waits round trip + 4 × variation, between `minRequestTimeoutMs`
  and `maxRequestTimeoutMs`. NB-IoT with 1.5 s each way settled on a 4.4 s
  timeout by itself and repeated nothing.
- **After a timeout it backs off and starts small.** The timeout doubles, and
  the window drops to one chunk and grows back a chunk per chunk received.
  A link that just choked is not flooded again with a whole window of
  repeats. The backoff clears as soon as data flows.
- **A stall is not a failure.** The job ends with `Timeout` only after
  `stallTimeoutMs` (10 minutes) without progress while the link is up. While
  the link is down the job just waits.
- **A power cut costs only what was in flight.** Progress is saved every
  `resumeEveryBytes` (64 kB). When the same job is offered again (a retained
  offer does that by itself), the node carries on from the last saved sector,
  re-hashing what it already has from flash. If the final hash fails after a
  resume, it starts once more from scratch before reporting a failure.
- **Buffer ≥ bandwidth × round trip.** The node never has more in flight than
  its buffer holds, so on a high-latency link a small buffer, not the link,
  sets the speed. On the Node UIO, a 200 kbit/s link with 1 s latency ran
  at ~6 KB/s with the 8 kB default. Use 32 kB for cellular.

Measured on the host with simulated links, and on the Node UIO over a
throttled, high-latency link:

| Link | Result |
|---|---|
| NB-IoT, 20 kbit/s, 1.5 s each way | 1.01× the image, no repeats |
| Cat-M1, 100 kbit/s, 90 s dead zones every 2 min while "up" | 1.02×, completes through 8 dead zones |
| 5 min outage mid-transfer | waits, finishes |
| link up but dead | `Timeout` after the stall time, 6 repeats in 5 min |
| Node UIO, power cut at 49 % | resumed from 58 %, 1.01× in total (1.46× before resume existed) |

**LoRaWAN is not a firmware link.** Downlinks are 51–242 bytes, limited by
duty cycle, and often capped at a handful a day by the network. A 1 MB image
would take days to weeks. Use LoRaWAN for status and for the command that
tells a node to update over another link. LoRaWAN's own FUOTA (multicast
fragmentation, TS004/TS005) suits small images on chips built for it, not an
ESP32 app.

## Safety, in layers

1. **CRC-32 per chunk**: a damaged chunk is re-requested, not written.
2. **SHA-256 of the whole image**, streamed as it is written, compared
   before anything is switched.
3. **Ed25519 or ECDSA P-256 signature** (optional) over the digest, size,
   board and version, so a signed image cannot be relabelled or replayed onto
   another board.
4. **The target's own check**: the ESP32 IDF validates the app image; the
   staged target reads the image back from storage and re-hashes it.
5. **Board and version policy**: wrong board, same version (unless forced)
   and downgrade (unless allowed) are refused before a byte moves.
6. **Trial boot**: the new image must confirm, by default once a link is up
   again. If it crashes before the boot hook runs, the bootloader rolls it
   back. After that, the software trial does: not confirmed within the
   window (two minutes by default), or `MODBEE_OTA_TRIAL_BOOTS` (3) boots
   without confirming, and the boot hook switches back to the old slot.

On the ESP32 under Arduino the boot hook runs from `verifyRollbackLater()`,
which `OtaEsp32Target.cpp` defines. Any firmware that links the library gets
it, used or not: it then confirms pending images at boot, as the core itself
would. Define `MODBEE_OTA_NO_ROLLBACK_HOOK` to opt out; the target then falls
back to ESP-IDF's runtime calls, and their freezes.

## Conventions

The library follows modbee-mqtt and BeeBus:

- No namespace. Types use the `Ota` prefix, free functions are `ota_camelCase`,
  and macros are `MODBEE_OTA_*`. It never uses `OTA_*`, which ArduinoOTA and
  ESP-IDF both define.
- One result enum (`OtaResult`), append-only, mirrored by the OpenPLC
  `OTA_RESULT` datatype.
- Every event is also polled state (`state()`, `result()`, `progress()`),
  because generated IEC code reads values and can't register a callback.
- No exceptions thrown or caught, and no exception tables on a board
  (`OtaNoExcept.h`); no RTTI use, and no `std::function` on the update path. The core compiles as C++14.
