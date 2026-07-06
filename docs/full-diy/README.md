# Full-DIY build: start to finish

Bare ESP32 dev board, WS2812B strip, one resistor, one cap. This is the version the
project was first built and tested on. Budget an evening for a first build, less
once you have done it.

Follow the steps in order. By the end the strip animates a golf stroke and you can
tune it from your phone.

## 0. What you need

- The parts in [BOM.md](BOM.md).
- A computer (macOS, Linux, or Windows) with a USB port.
- A data-capable micro-USB cable. Charge-only cables are the number one "board not
  detected" cause.
- Optional but recommended: a multimeter to check 5 V and continuity.

## 1. Assemble the hardware

1. **Cut and seat the strip.** Cut the WS2812B strip to 144 LEDs if it is longer
   (cut only on the marked copper pads). Slide it into the aluminum channel and clip
   the frosted diffuser on top. The diffuser is what turns dots into a clean bar of
   light.
2. **Identify the data-in end.** WS2812B is directional. Find the arrow printed on
   the strip. You wire into the end the arrow points away from (DIN).
3. **Wire it** per this diagram:

```
ESP32 GPIO5 ---[ 470 ohm ]--- DIN   (strip data in, follow the arrow)
ESP32 VIN   --------------+--- 5V    (strip power, and the breadboard + rail)
ESP32 GND   --------------+--- GND   (common ground, the - rail)
                          |
                    [ 1000 uF ]      across 5V/GND at the strip input
                          |          (stripe / short leg to GND)
```

- `VIN` is the 5 V USB passthrough on the 30-pin board, not `3V3`. `3V3` will
  underdrive the strip.
- The 470 ohm resistor goes inline between GPIO5 and DIN, close to the strip.
- The 1000 uF cap goes across the strip's 5 V and GND, close to where power enters.
  The striped (short) leg is negative, to GND.
- The 30-pin ESP32 is wider than one breadboard. Straddle two, one pin row on each,
  so the outer holes stay usable.

Do not power the board yet. Double-check that 5 V and GND are not swapped or shorted
before you plug in.

## 2. Install drivers and toolchain

You need `arduino-cli` (or the Arduino IDE) with the ESP32 core and FastLED.

```sh
# ESP32 board support
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32

# FastLED (the only library you install by hand; BLE and Preferences ship with the core)
arduino-cli lib install FastLED
```

**Serial driver.** Most classic ESP32 boards use a CP2102 (Silicon Labs) USB chip.
Install the **CP210x VCP driver** for your OS. On macOS you must also approve it:
System Settings > General > Login Items & Extensions > Driver Extensions. A reboot
alone does not approve it.

Plug the board in and confirm it enumerates:

```sh
ls /dev/cu.*        # macOS: look for /dev/cu.usbserial-* or /dev/cu.SLAB_USBtoUART
# Linux: ls /dev/ttyUSB*    Windows: check Device Manager for a COM port
```

No port means a charge-only cable or an unapproved driver. Fix that before going on.

## 3. Flash

The sketch in this folder is pinned to the DIY board (`#define BOARD_DIY_ESP32`,
GPIO5 data, no power relay). Partition matters: BLE plus FastLED overflow the
default app partition, so use **Huge APP**.

Run these from this folder (`docs/full-diy/`):

```sh
# compile
arduino-cli compile \
  --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" \
  tempo_trainer_diy

# upload (swap in your port from step 2, e.g. /dev/cu.usbserial-0001 or /dev/cu.SLAB_USBtoUART)
arduino-cli upload -p /dev/cu.usbserial... \
  --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" \
  tempo_trainer_diy
```

Arduino IDE equivalent: board **ESP32 Dev Module**, partition scheme
**Huge APP (3MB No OTA/1MB SPIFFS)**, upload at 115200 baud.

If upload stalls at `Connecting...`, hold the board's **BOOT** button until it
catches, then release.

## 4. Verify

The board boots straight into the comet loop, no pairing needed. A lit strip
animating a stroke (white comet, green and amber dots) is your "it works." If it is
dark, see [Troubleshooting](#troubleshooting).

## 5. Set up a controller

The strip runs on its own, but the point is tuning it live. Pick one:

- **iOS app (best experience):** the native controller with numeric tuning (tap to
  type or hold to adjust), per-value strip diagrams, a live on-strip reach preview,
  and the stroke visualizer. Full build and install guide:
  [osstempo-ios repo](https://github.com/OSSGolf/osstempo-ios).
- **Web page:** open `web/index.html` from the repo in Chrome or Edge (Web Bluetooth
  needs a Chromium browser). Click connect, pick `PuttTempo`.
- **nRF Connect (phone):** [nRF Connect] scans, connects, and writes characteristics
  by hand. The GATT table is in the repo root [README](../../README.md).

## Troubleshooting

- **Board not detected / no serial port.** Almost always the USB cable (charge-only)
  or the unapproved CP210x driver. Fix both, re-check `ls /dev/cu.*`.
- **Upload fails or hangs at `Connecting...`.** Hold **BOOT** during upload. Make
  sure no serial monitor is holding the port open.
- **Strip stays dark after a good flash.** Check `VIN` (not `3V3`) feeds the strip,
  GND is common between board and strip, and DIN is on the arrow end. A persisted
  brightness of 0 cannot brick it: the firmware floors brightness to a visible level
  on every boot.
- **First pixel flickers or shows wrong colors.** Add the SN74HCT245 level shifter
  on the GPIO5 to DIN line. We did not need it, but a long lead or a fussy strip
  might.
- **App says "incompatible device" / missing characteristics.** That is a firmware
  GATT bug already fixed in this repo's sketch. Make sure you flashed this sketch,
  not an older copy. Details are in the repo root README's Gotchas.

[nRF Connect]: https://www.nordicsemi.com/Products/Development-tools/nrf-connect-for-mobile
