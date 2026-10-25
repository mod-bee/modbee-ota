# Porting to a new microcontroller

To add a chip, you implement an `OtaTarget`. You don't touch the updater, the
protocol or the links.

## Chips with their own OTA

These are the ESP32 family, and to some extent nRF5 with its own DFU and
RP2040 under the Arduino-Pico core. Implement `OtaTarget` directly around the
vendor API, as `src/esp32/OtaEsp32Target.cpp` does. The plain mapping is:

| Method | ESP32 equivalent |
|---|---|
| `capacity()` | size of the next OTA slot |
| `begin()` | `esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES)` |
| `write()` | `esp_ota_write()` |
| `finish()` | `esp_ota_end()`: the vendor's image check |
| `activate()` | `esp_ota_set_boot_partition()` |
| `testing()` / `confirm()` / `rollback()` | `ESP_OTA_IMG_PENDING_VERIFY`, `mark_app_valid`, `mark_app_invalid_rollback_and_reboot` |

The ESP32 target goes further to stay out of a PLC's way: it erases the slot
a sector per call before the download, and moves every boot-record write to
boot (see the top of `OtaEsp32Target.cpp`). A new target only needs that much
care if its vendor calls freeze the CPU.

Beyond the table, a target may implement:

| Method | For |
|---|---|
| `prepare()` | one-off slow set-up, done at boot instead of in the first job |
| `resume()` + `read()` | carrying on after a power cut (ESP32: `esp_ota_resume()`) |
| `backlog()` | a target with a queue says how much is not in storage yet |
| `maxWrite()` | the most one `write()` takes |

If its calls can't be made short, wrap it in `OtaAsyncTarget` and give that a
worker (`OtaEsp32Worker` on FreeRTOS). The updater then never waits on it.

The rules are in `src/OtaTarget.h`:

- `write()` gets the image in order, never repeated.
- No call may block for long. A step that would (verifying a whole image,
  erasing a large region) returns `OtaResult::Pending` and is called again on
  the next `update()`.

## Chips without OTA: STM32, RP2040, SAMD

These chips can't rewrite the flash they're running from while they run.
An update is staged somewhere else, and a bootloader installs it on reset.
`OtaStagedTarget` does the generic part:

- erasing just ahead of each write (or up front, one block per call);
- padding writes to the program granularity (8 bytes on an STM32L4, 32 on
  an H7);
- reading the staged image back and re-hashing it before handing over.

A new chip then needs two small classes:

```cpp
class OtaStorage {           // where the image is staged
  uint32_t size(), eraseSize(), writeAlign();
  OtaResult erase(offset, length), write(offset, data, length), read(offset, out, length);
};

class OtaBootControl {       // the hand-over to the bootloader
  OtaResult stage(const OtaImage&);   // the commit point
  void reboot();
  bool testing(); OtaResult confirm(), rollback();
};
```

Because the core builds on a desktop, a new boot control can be tested there
before it ever runs on a board: an `OtaStorage` over a RAM buffer with real
NOR semantics (it refuses to program a byte that isn't erased) catches most
mistakes in the erase and write logic.

### Where to stage

| Option | Fits | Notes |
|---|---|---|
| Upper bank of dual-bank flash | STM32G4/L4/H7 with `DBANK`/`BFB2` | The hardware can swap banks; the bootloader is nearly free |
| Spare region of internal flash | Single-bank parts with 2× the app size | Watch sector size: an F4's 128 kB sectors take over a second to erase. Use `eraseFirst` |
| External SPI NOR | Any chip, any size | 4 kB erases; this is what a ModBee node would carry |
| LittleFS file | RP2040 (Arduino-Pico) | The core's `picoOTA` bootloader installs from a file |

### Which bootloader

| Bootloader | When |
|---|---|
| **MCUboot** | The default for STM32 and nRF. Swap with revert gives trial boot and rollback for free; `stage()` writes its image trailer, `confirm()` sets `image_ok` |
| **picoOTA** | RP2040 under the Arduino-Pico core. `stage()` writes the command file; no trial boot |
| **ST system DFU / bank swap** | Dual-bank STM32 without a custom bootloader |
| **A small ModBee bootloader** | Only if none of the above fit. It needs: a header with size and SHA-256, copy-and-verify, and a boot counter for rollback |

## Checklist

1. Implement `OtaTarget`, or `OtaStorage` + `OtaBootControl`.
2. If staged, run it on a desktop against a RAM `OtaStorage` with NOR semantics.
3. Add a guarded source folder (`src/stm32/`, `#if defined(ARDUINO_ARCH_STM32)`)
   so other platforms compile it to nothing.
4. Add the chip to `library.properties` `architectures` if it's narrower
   than `*`.
5. Add an example and compile it for that board.
