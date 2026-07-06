# Dig2go build: start to finish

A pre-assembled [QuinLED-Dig2go], a strip, and three wires. The board has the level
shifter, data resistor, power relay, fuses, and a USB-C port already on it, so this
is the short path. Realistically 20 minutes, most of it flashing.

Follow the steps in order. By the end the strip animates a golf stroke and you can
tune it from your phone.

## 0. What you need

- The parts in [BOM.md](BOM.md).
- A computer (macOS, Linux, or Windows) with a USB port.
- A data-capable USB-C cable.
- A small flat screwdriver for the board's screw terminals.

## 1. Assemble the hardware

1. **Cut and seat the strip.** Cut the WS2812B strip to 144 LEDs if longer (only on
   the marked copper pads). Slide it into the aluminum channel and clip the diffuser
   on top.
2. **Find the data-in end.** WS2812B is directional. Wire into the end its printed
   arrow points away from (DIN).
3. **Land three wires on the screw terminals:**

```
strip DIN  --> LED-data terminal   (GPIO16)
strip 5V   --> LED power +         (board's LED power out)
strip GND  --> LED power -
```

- Data goes to the **GPIO16** LED-data terminal. Not GPIO5: on the Dig2go that pin
  is the IR receiver and will not drive the strip.
- Power the strip from the board's LED power terminals, not the spare "switched 5 V"
  side pins.
- No external resistor, cap, or level shifter. They are all onboard.

Strip the wire ends cleanly (a ferrule helps) so no strands bridge two terminals.

## 2. Install drivers and toolchain

Same as any ESP32 build: `arduino-cli` (or the Arduino IDE) with the ESP32 core and
FastLED.

```sh
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install FastLED
```

The Dig2go uses a **CH340** USB-serial chip. On current macOS it enumerates with no
driver install. Older macOS, Linux, or Windows may need the CH340 (`wch`) driver.
Plug it in and confirm:

```sh
ls /dev/cu.*        # macOS: look for /dev/cu.wchusbserial* (CH340)
# Linux: ls /dev/ttyUSB*    Windows: check Device Manager for a COM port
```

## 3. Flash

This overwrites the WLED firmware the Dig2go ships with. That is intended. The
sketch here is pinned to the Dig2go (`#define BOARD_DIG2GO`: GPIO16 data, GPIO12
power relay). Use the **Huge APP** partition, same reason as always (BLE plus
FastLED overflow the default).

Run these from this folder (`docs/dig2go/`):

```sh
# compile
arduino-cli compile \
  --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" \
  tempo_trainer_dig2go

# upload (swap in your port from step 2, e.g. /dev/cu.wchusbserial110)
arduino-cli upload -p /dev/cu.wchusbserial... \
  --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" \
  tempo_trainer_dig2go
```

Arduino IDE equivalent: board **ESP32 Dev Module**, partition
**Huge APP (3MB No OTA/1MB SPIFFS)**. If upload stalls at `Connecting...`, hold the
board's **BOOT** button until it catches.

A clean flash ends with `Hash of data verified` and a hard reset. The board then
boots into the comet loop.

## 4. Verify

The strip should light and start animating on boot, no pairing required. A white
comet running a stroke with green and amber dots is your "it works." If it is dark,
see [Troubleshooting](#troubleshooting). This is the one place the Dig2go differs
from a bare board, so read that first entry if nothing lights.

## 5. Set up a controller

Pick one:

- **iOS app (best experience):** the native controller with numeric tuning (tap to
  type or hold to adjust), per-value strip diagrams, a live on-strip reach preview,
  and the stroke visualizer. Full build and install guide:
  [osstempo-ios repo](https://github.com/OSSGolf/osstempo-ios).
- **nRF Connect (phone):** [nRF Connect] scans, connects, and writes characteristics
  by hand.
- **Web page:** open `web/index.html` from the repo in Chrome or Edge.

The GATT table is in the repo root [README](../../README.md).

## Troubleshooting

- **Strip is dark but the flash succeeded.** This is the Dig2go's signature trap.
  The board's relay cuts 5 V to the LEDs until GPIO12 is driven high. Our firmware
  does that at boot. If it is still dark, your unit may wire the relay active-low:
  open the sketch, change `#define LED_POWER_ON HIGH` to `LOW` in the board-select
  block, and reflash. Also confirm data is on the **GPIO16** terminal and the strip
  is fed at its DIN (arrow) end.
- **No serial port.** The CH340 needs no driver on recent macOS, but it does need a
  **data-capable** USB-C cable. Re-check `ls /dev/cu.*` for `wchusbserial`.
- **Upload hangs at `Connecting...`.** Hold **BOOT** during upload. Close any serial
  monitor holding the port.
- **App says "incompatible device" after a reflash.** iOS caches the GATT table.
  Toggle the iPhone's Bluetooth off and on (or forget the device) to force fresh
  discovery. If it persists, it is the createService handle bug covered in the repo
  root README's Gotchas. Make sure you flashed this repo's sketch.

[QuinLED-Dig2go]: https://quinled.info/quinled-dig2go/
[nRF Connect]: https://www.nordicsemi.com/Products/Development-tools/nrf-connect-for-mobile
