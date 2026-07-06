# OSSTempo build docs

An open-source golf tempo trainer. A WS2812B LED strip animates a white "comet"
that runs a putt or a full swing, so you groove your tempo to a moving light.
Two reference dots (green = ball/impact, amber = top of backswing) bound the
motion. Control is over Bluetooth from a phone or a web page. No audio, no cloud,
no account.

There are two ways to build it. Same firmware, same features, same app. Pick by
how much you feel like wiring.

| Path | Board | What you do | Good for |
|---|---|---|---|
| [**full-diy/**](full-diy/) | generic ESP32 dev board | breadboard a resistor, a cap, and the strip | lowest cost, parts you may already have, learning how it works |
| [**dig2go/**](dig2go/) | QuinLED-Dig2go (pre-assembled) | screw in three wires, flash, done | fastest path to a finished, enclosed unit |

Each folder has its own [bill of materials], wiring and flashing guide, and a
ready-to-flash sketch. Both paths finish the same way: flash the board, then set up
a controller.

## The whole journey

1. Pick a board and follow its guide end to end: [full-diy](full-diy/README.md) or
   [dig2go](dig2go/README.md). Each covers hardware, drivers, toolchain, flashing,
   and verifying the strip animates.
2. Set up a controller to tune it. The best experience is the iOS app: the
   [osstempo-ios repo](https://github.com/OSSGolf/osstempo-ios) has the full build
   and install walkthrough. A web page and nRF Connect are lighter alternatives,
   covered in each board guide.

The [osstempo-ios repo](https://github.com/OSSGolf/osstempo-ios) holds the complete
app source (SwiftUI plus CoreBluetooth, Swift 6), so you can build it yourself.

## One firmware, two builds

Both sketches are the same code. They differ by one line at the top, a board
select:

```c
#define BOARD_DIY_ESP32     // GPIO5 data, external 470ohm + 1000uF
// or
#define BOARD_DIG2GO        // GPIO16 data, onboard level shifter + power relay
```

The canonical source lives in `tempo_trainer/` at the repo root. The two copies
under `docs/` are pinned to their board so you can open one and flash it without
thinking about the toggle.

## What it does

- **Putt mode:** a 2:1 pendulum. The comet swings out to the amber backswing dot,
  eases to a stop, then accelerates back through impact and out the follow side.
- **Full mode:** a full swing's arc projected onto the 1D strip. The clubhead runs
  out to max width (red, at the strip end), up to the top (amber), back out, then
  down through impact.
- Per-rep **get-ready countdown**: the green impact LED pulses smoothly N times
  (a 3-2-1-go) before each stroke.
- Everything (tempo, reach, brightness, timing, mode) is tunable live over BLE and
  saved to flash.

The full hardware and firmware spec, the BLE GATT table, and the physics live in
the repo root [`README.md`](../README.md). The iOS controller source and its build
guide live in the [osstempo-ios repo](https://github.com/OSSGolf/osstempo-ios).

[bill of materials]: full-diy/BOM.md
