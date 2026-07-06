# Full-DIY bill of materials

The from-scratch build: a bare ESP32 dev board and a strip on a breadboard. This
is the version this project was first built and tested on. Prices are rough and
vary by supplier.

## Core parts

| Qty | Part | Spec that matters | ~USD |
|---:|---|---|---:|
| 1 | ESP32 dev board | classic ESP-WROOM-32, **30-pin**, micro-USB. USB-UART chip is a CP2102 (Silicon Labs) on the board we used. | 5-8 |
| 1 | WS2812B LED strip | **144 LEDs/m, 1 m**, 5 V, bare PCB 12 mm wide. Individually addressable (WS2812B / "NeoPixel"), not analog 12 V. | 8-15 |
| 1 | Aluminum LED channel + diffuser | **12 mm** inner width, frosted diffuser, 1 m. This is what makes the comet look like a bar of light instead of dots. | 6-10 |
| 1 | 5 V power supply | 2 A recommended. A USB power bank or a 5 V wall adapter both work. See the note below on current. | 6-10 |
| 1 | 470 ohm resistor | inline on the data line (GPIO5 to DIN). Tames ringing on the first pixel. | <0.10 |
| 1 | 1000 uF electrolytic cap | 6.3 V or higher, across 5 V/GND at the strip input. Absorbs the inrush when the strip powers up. | ~0.30 |

## Wiring and prototyping

| Qty | Part | Note | ~USD |
|---:|---|---|---:|
| 2 | solderless breadboard | The 30-pin ESP32 is too wide for one board. It straddles two, one pin row on each, so the outer holes stay usable. A single piece of perfboard works too if you solder. | 4-8 |
| 1 | jumper wires / hookup wire | a handful of male-male | 3 |
| 1 | USB cable | **data-capable** micro-USB. A charge-only cable powers the board but never enumerates, and that is the single most common "board not detected" trap. | reuse |

## Optional

| Qty | Part | Note | ~USD |
|---:|---|---|---:|
| 1 | SN74HCT245 level shifter | 3.3 V to 5 V on the data line. We kept one on hand and never needed it: the bare 3.3 V line drove the strip cleanly with no first-pixel flicker. Add it only if you see glitching later. | ~0.50 |

## Notes from building this

- **Current draw is low, so do not oversize the supply.** The comet
  lights a handful of LEDs at a time, so it pulls roughly 100-300 mA in normal use.
  The scary "144 LEDs at 8 A" number only applies to an all-white full-strip
  pattern, which this firmware never runs. A 2 A supply has plenty of headroom.
  Do not run a full white test pattern on a 2 A brick.
- **Feed 5 V from `VIN`, not `3V3`.** On the 30-pin board the silkscreen labels the
  5 V USB passthrough `VIN`. That is what powers the strip. `3V3` will underdrive it.
- **Feed the strip at the DIN end.** WS2812B is directional. Follow the arrow
  printed on the strip.
- **macOS needs the CP210x VCP driver approved,** not just installed. On Sequoia the
  approval moved to System Settings > General > Login Items & Extensions > Driver
  Extensions. A reboot alone will not approve it.

Next: [wire it and flash it](README.md).
