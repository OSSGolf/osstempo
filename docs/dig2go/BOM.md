# Dig2go bill of materials

The short build. The [QuinLED-Dig2go] is a pre-assembled, enclosed ESP32 LED
controller: level shifter, data resistor, power relay, fuses, and a USB-C port,
all onboard. You bring a strip and three wires. Prices are rough and vary.

## Core parts

| Qty | Part | Spec that matters | ~USD |
|---:|---|---|---:|
| 1 | QuinLED-Dig2go | pre-assembled. Onboard ESP32 (the unit we flashed reads as ESP32-D0WD-V3, 4 MB flash), CH340 USB-C serial, level shifter, data resistor, and a **switchable LED-power relay**. Ships with WLED, which you overwrite. | 25-35 |
| 1 | WS2812B LED strip | **144 LEDs/m, 1 m**, 5 V, 12 mm wide. Same strip as the DIY build. | 8-15 |
| 1 | Aluminum LED channel + diffuser | **12 mm** inner width, frosted, 1 m. | 6-10 |
| 1 | USB-C cable | **data-capable**, for flashing. If you also power the strip from USB, this carries the LED current too, so use a decent one. | reuse |

## Optional

| Qty | Part | Note | ~USD |
|---:|---|---|---:|
| 1 | 5 V power supply | Only needed if you power the strip from the Dig2go's DC input instead of USB. For this comet the low draw means USB-C is fine, so most builds skip this. | 6-10 |
| 1 | small flat screwdriver | for the board's screw terminals | reuse |
| few | wire ferrules | tidy crimps for the strip leads into the terminals | ~0.10 |

## What you do NOT need (it is onboard)

- **No 470 ohm data resistor.** The Dig2go has one on its LED-data output.
- **No level shifter.** Onboard, so the 5 V data line is clean by default.
- **No 1000 uF cap or breadboard.** Everything terminates at the screw terminals.

## The one thing that will bite you

The Dig2go has a **relay that physically cuts 5 V to the LED output when it is
"off."** It is gated by **GPIO12**. Stock WLED closes it for you; a from-scratch
sketch has to drive GPIO12 high or the strip gets data and no power, and just sits
there dark. The firmware in this repo already does this (see the board-select
block). This cost us real debugging time, so it is called out here and in the
build guide.

- LED data output is **GPIO16** on this board. GPIO5, which the DIY build uses,
  is the Dig2go's **IR-receiver** pin, so it will not drive a strip here.
- USB-C **does** feed the LED power output, so USB power alone lights the strip.

Sources: [QuinLED-Dig2go overview][QuinLED-Dig2go],
[pinout guide](https://quinled.info/quinled-dig2go-pinout-guide/).

Next: [wire it and flash it](README.md).

[QuinLED-Dig2go]: https://quinled.info/quinled-dig2go/
