# Tempo Trainer

A TempoStik-style tempo trainer: a WS2812B LED strip animates a swing so the
user follows a moving dot to groove their tempo. Two modes — **putting** (2:1)
and **full swing** — selectable over BLE. No audio.

## Goal

A white LED "comet" sweeps along the strip following a pendulum/arc model so the
motion feels like a real stroke. The user matches their club/putter head to the
dot to train tempo and stroke length. Reference dots (green = ball/impact,
amber = top of backswing, red = full extent, optional purple = full-swing
weight-shift aim) orient the golfer before each rep.

- **Putt mode:** comet sweeps center → amber (backswing), eases to a stop, then
  accelerates back through center (impact) and out the far side (follow-through).
- **Full mode:** models a full swing's *arc projected onto the 1D strip* (see
  [Animation models](#animation-models)). Lets a 1 m strip represent a swing that
  goes back and up behind the golfer.

## Build guides

Two ways to build it, same firmware and app. See [`docs/`](docs/):

- [**docs/full-diy/**](docs/full-diy/) - bare ESP32 dev board, strip, one resistor
  and one cap on a breadboard. Lowest cost.
- [**docs/dig2go/**](docs/dig2go/) - pre-assembled [QuinLED-Dig2go]. Screw in three
  wires, flash, done.

Each folder has a bill of materials, a wiring and flashing guide, and a pinned,
ready-to-flash sketch. The sections below are the full hardware and firmware spec
that both builds share.

[QuinLED-Dig2go]: https://quinled.info/quinled-dig2go/

## Hardware

- **MCU:** ESP32 classic (ESP-WROOM-32, 30-pin dev board, micro-USB). BLE used, not WiFi.
  USB-UART chip is a **CP2102** (Silicon Labs).
- **LED strip:** WS2812B, **144 LEDs on a ~1 m strip (144 LEDs/m)**, bare PCB
  12 mm wide x 2.2 mm tall, 5 V. (~0.69 cm per LED; half-strip ≈ 50 cm each side.)
- **Mounting:** 12 mm aluminum LED channel with frosted diffuser (self-sourced).
  2020 extrusion was rejected as the light channel (T-slot opening ~6 mm is too
  narrow for the 12 mm strip, and bare aluminum gives no diffusion).
- **Power:** micro-USB from a power bank or 5 V wall adapter. **A 2 A supply is
  recommended.** The strip is powered through the board's 5 V passthrough pin.
  The comet animation is low-draw (~100-300 mA), so the full-strip ~8 A worst
  case does not apply — but do **not** run an all-LEDs-on white pattern on 2 A.
- **Protoboard:** the 30-pin ESP32 is too wide for one standard breadboard — it
  straddles **two breadboards** (one row of pins on each) so the outer holes stay
  free.

## Wiring

| From | To |
|---|---|
| `GPIO5` (labeled `D5`) | 470 ohm resistor -> strip DIN |
| `VIN` pin (the 5 V passthrough; *not* `3V3`) | strip 5V (+ breadboard red rail) |
| `GND` | strip GND (common ground, blue rail) |
| 1000 uF cap | across 5V/GND at strip input (stripe / short leg to GND) |

- **`VIN` is the 5 V passthrough.** On the 30-pin board the silkscreen labels it
  `VIN` (not `5V`); it carries USB 5 V out to the strip. Do not use `3V3`.
- **Level shifter (SN74HCT245) is on hand but NOT needed** — the bare 3.3 V data
  line drives the strip cleanly (no first-pixel flicker observed). Add it on the
  `GPIO5 -> DIN` line only if flicker/wrong colors appear later.
- `GPIO5` was chosen as an exposed pin on the 30-pin board. Feed the strip at the
  DIN end (follow the data-direction arrow).

### macOS USB note

The board appears as a serial port (`/dev/cu.usbserial-*` or
`/dev/cu.SLAB_USBtoUART`) only when:

1. The **CP210x VCP driver** is installed **and approved** in
   *System Settings -> Login Items & Extensions -> Driver Extensions* (macOS
   Sequoia moved the approval there; a reboot alone does not approve it).
2. A **data-capable USB cable** is used. A charge-only micro-USB cable lights the
   board's red power LED but never enumerates on USB — the single most common
   "board not detected" cause. Verify with `ls /dev/cu.*` after plugging in.

## Firmware

- **Language / libs:** Arduino C++ (`.ino`), [FastLED], ESP32 core BLE
  (`BLEDevice` / `BLEServer`) and `Preferences` (NVS). FastLED is the only
  Library Manager install; BLE + Preferences ship with the ESP32 core.
- **Board target:** "ESP32 Dev Module."
- **Partition scheme:** **"Huge APP (3MB No OTA/1MB SPIFFS)"** — BLE + FastLED
  exceed the default app partition.
- **Upload:** micro-USB, 115200 baud. Hold **BOOT** if upload stalls at
  `Connecting...`. The sketch boots straight into the continuous loop (no BLE
  needed to run); BLE is only for customization.
- **File:** `tempo_trainer/tempo_trainer.ino`.

## Animation models

Center LED (index ~71/72) = ball / impact. Each rep runs a get-ready **countdown**
(the green impact LED pulses smoothly N times, 1 s each, with the aim dots lit) and
then the stroke, auto-repeating after a rest gap. During the stroke the comet is
**bounded** by the impact and aim markers (the green/amber "rails", toggle `000e`):
it emerges from the ball and parks at the backswing point rather than overshooting.

### Putt mode (pendulum)

1. **Get-ready countdown:** the green ball pulses smoothly N times (1 s each,
   N = `round(get-ready ms / 1000)`), with the amber aim dot lit so you can set up.
2. **Backswing:** comet center -> amber with `sin` easing (fast leaving center,
   decelerating to a stop), **bounded to `[impact, amber]`** — it emerges from the
   ball and parks at the amber without overshooting either marker.
3. **Drop to impact:** comet amber -> center over `downswing ms` (the tempo
   number; ratio = backswing:downswing), shaped by `release`. Fastest at impact;
   flows **through** impact (no hitch) into the follow-through.
4. **Follow-through:** center -> `-reach` over `follow-thru ms`, decelerating.
5. **Hold** at the finish, then **rest**, repeat.

With `rails lit` (`000e`) on, the green impact and amber backswing dots stay lit
throughout as the bounding "walls"; off, the amber shows until reached (legacy look).

### Full mode (arc projected onto the strip)

A full swing's clubhead traces an arc. Projected onto the 1D strip its position
is **non-monotonic**: it reaches max width (**red**, at the strip end) around
waist height, then comes up/in to the top of backswing (**amber**, inboard of
red). The downswing retraces back out to red before dropping to impact.

```
 followRed              center            amber          backRed
   (red)                (green)          (amber)          (red)
   |---------------------|----------------|----------------|
   far end             impact         top of back       near end / max width

 1. Backswing:   center -> backRed -> backAmber       (out to max width, up to top)
    [top pause]
 2. Downswing:   backAmber -> backRed -> center        (back out, then to impact)
 3. Follow-thru: center -> followRed                   (to the far end; no follow amber)
 4. Hold, rest, repeat
```

Easing keeps velocity zero at the turnarounds (both red visits, the top) and
maximal at center (impact), so the projection reads like a real swing.

#### Aiming dot (purple, full swing, optional)

An optional **purple** secondary dot (`000f`-`0011`, per-mode, full only) gives a
weight-shift / hit-down reminder for irons. It defaults **off**; when enabled it
sits 2 LEDs to the **target side** of impact (toward the finish) and never overlaps
the ball, amber, or red dots. It **appears** at a configurable stroke moment (`0011`: 0 address / 1 top
of backswing (default) / 2 start of downswing / 3 impact) and **stays lit until the
next rep**, independent of the `rails lit` toggle. Position (`0010`) is any LED that
is not already a marker.

### Release (acceleration profile)

Per Grober's pendulum framework, the dominant acceleration lever is the **tempo
ratio** (backswing:downswing) — already exposed via the two times. The one
remaining physical degree of freedom is the **release / lag**: how the downswing's
speed builds between the top and impact. `release` (`000c`, per-mode) blends the
drop-to-impact from a **free pendulum** (0: smooth, velocity peaks gently at
impact, putting-like) to a **late lag release** (100: the comet hangs near the top,
then snaps through impact). It reshapes only the drop-to-impact segment and never
changes tempo (impact still lands on time). Defaults: putt 0, full 55.

## Profiles & persistence

- **Two profiles** (putt / full), each with its own `backswing ms`,
  `downswing ms`, `reach`, and `top-hold ms`. Switching mode restores that mode's
  settings.
- **Global settings** (shared): brightness, get-ready ms, finish-hold ms, rest ms.
- All settings are **persisted to flash (NVS)** and reload on boot. Writes are
  debounced (~1.5 s) so live preview-slider dragging doesn't wear the flash.
- `control` is **not** persisted — the device always boots into loop mode.

#### Reversing the stick (180 flip)

`0014` runs the stroke the other way down the strip, for a left-hander or a stick lying the
other way round. It mirrors the **finished frame** on its way out, `i -> NUM_LEDS-1-i`, at the
firmware's single output choke point (`showFrame()`), so every element flips together -- comet
head and tail, green ball, amber top-of-backswing, both red extents, the purple aim dot, the
get-ready pulse, the idle dot -- and nothing in the stroke math changes. Reflection about index
`(NUM_LEDS-1)/2` *is* reflection about `CENTER`, so the picture stays self-consistent; the green
ball sits half an LED above centre normally and half an LED below it reversed, which is the exact
mirror image rather than an off-by-one.

Flipping mid-stroke is safe and needs no special handling: the renderer holds no temporal state,
so frame N draws logical and frame N+1 draws mirrored, both at the same cycle time. The comet
lands at its mirror position with identical phase -- a reflection in space, never a jump in time.
Worst case it is visible one render tick later (<=12 ms while running, <=20 ms idle), plus the
BLE round trip; a phone write with response is typically another 60-130 ms.

Two states would otherwise give no feedback at all -- idle lights a single dim centre pixel, and
the rest gap lights nothing -- so for 1.2 s after a flip both paint the mode's static dot map
instead. The amber dot jumping to the other side of the ball is the answer to "which way does it
run now?".

**It is device-local.** Orientation describes how *that* stick is lying, so it is deliberately
not carried in the sync beacon: two sticks in a set can point opposite ways, and a leader can
never rotate a follower's strip. Consequence: a follower goes BLE-quiet while in a set, so set
orientation **before** joining. It persists in NVS, including across a follower's revert.

It is also the one characteristic the iOS app treats as **optional**, so a stick flashed before
`0014` existed still connects and works -- only the reverse toggle is disabled.

## BLE GATT

- **Service:** `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- **Device name:** `PuttTempo`

| UUID tail | Param | Type | Range / units | Scope | Default (putt / full) |
|---|---|---|---|---|---|
| 0002 | backswing ms | uint16 LE | 100-5000 | per-mode | 700 / 900 |
| 0003 | downswing ms (top→impact) | uint16 LE | 100-5000 | per-mode | 350 / 300 |
| 0004 | reach (amber) | uint8 | 1-71 LEDs from center | per-mode | 58 / 60 |
| 0005 | brightness | uint8 | 0-255 | global | 40 |
| 0006 | control | uint8 | 0 idle / 1 loop / 2 single / 3 preview | runtime | 1 |
| 0007 | get-ready ms | uint16 LE | 0-10000 (countdown: round(ms/1000) impact pulses) | global | 1000 |
| 0008 | finish hold ms | uint16 LE | 0-10000 | global | 600 |
| 0009 | rest ms | uint16 LE | 0-20000 | global | 3000 |
| 000a | mode | uint8 | 0 putt / 1 full | persisted | 0 |
| 000b | top-hold ms | uint16 LE | 0-2000 | per-mode | 0 / 150 |
| 000c | release | uint8 | 0-100 (0 pendular .. 100 lag) | per-mode | 0 / 55 |
| 000d | follow-thru ms | uint16 LE | 0-10000 (impact→finish) | per-mode | 350 / 300 |
| 000e | rails lit | uint8 | 0/1 (keep impact+aim dots lit) | global | 1 |
| 000f | aim2 enabled | uint8 | 0/1 (purple weight-shift dot, full only) | per-mode | 0 / 0 |
| 0010 | aim2 position | uint8 | 1-142 (absolute LED index) | per-mode | 70 / 70 |
| 0011 | aim2 when | uint8 | 0 address / 1 top / 2 downswing / 3 impact | per-mode | 1 / 1 |
| 0014 | flip | uint8 | 0 normal / 1 mirrored end-for-end | global, device-local | 0 |

uint16 values are little-endian (e.g. 700 = `BC 02`). Per-mode writes apply to
whichever mode is currently selected (`000a`); switching `000a` refreshes the
per-mode characteristics so a read returns that mode's saved values.

- **`0006` (control) supports NOTIFY** — subscribe to get device-side state
  changes pushed (e.g. a single stroke auto-resetting to idle).
- **Discovery:** the device advertises the **service UUID** (filter scans on it)
  and broadcasts the name `PuttTempo` in the scan response. iOS/CoreBluetooth
  clients should scan by service UUID, not name.

### Setting the backswing point (live preview)

1. Write `000a` = mode (`00` putt / `01` full).
2. Write `0006` = `03` (preview) -> the strip shows the static dot map.
3. Write `0004` = reach in LEDs and watch the **amber dot move live**; park it
   where the backswing should stop.
4. Write `0006` = `01` (loop) -> runs strokes; the value auto-saves ~1.5 s later.

Drive it from a phone with **nRF Connect** (scan -> connect to `PuttTempo` ->
write characteristics), or a Web Bluetooth page (TODO below).

## Clients

- **Web Bluetooth page** (`web/index.html`) — single self-contained control page.
- **iOS app** (SwiftUI + CoreBluetooth) in the
  [osstempo-ios repo](https://github.com/OSSGolf/osstempo-ios) —
  native controller with numeric tuning (tap to type or hold to adjust), per-value
  strip diagrams, a follow-through link, a live reach drag, the get-ready countdown /
  rails toggle, and the on-strip stroke visualizer. Dev-only (no App Store).

## Gotchas / lessons learned

- **GATT handle budget.** ESP32 Arduino `createService(uuid)` defaults to **15 handles**
  and *silently truncates* the characteristic table after ~7 chars, and the Arduino BLE wrapper
  does not check the error. Symptom: a client discovers only the first 6-7 characteristics and
  rejects the device as incompatible -- indistinguishable from the stale-GATT-cache symptom
  below, so you pay the debugging cost twice. Count it exactly every time you add one:
  1 (service) + chars x 2 + 1 per CCCD. Today that is `1 + 19x2 + 2 = 41`, against the 48
  reserved by `createService(BLEUUID(SVC_UUID), 48)` -- room for 3 more before it must rise.
- **`FastLED.show()` must appear exactly once**, inside `showFrame()`. That is what makes the
  180 flip total: the mirror is applied to the finished buffer, so no LED element can escape it
  -- but only while every render path latches through that one function. `FastLED.clear(true)`
  is banned for the same reason (it calls `showColor()`, a second output path that bypasses the
  frame buffer entirely), as is `FastLED.clear()` (it clears the array bound to the controller,
  which is the *physical* buffer, not the logical frame -- use `clearLeds()`).
  `firmware_tests/check_show_chokepoint.sh` enforces all of this; a compile cannot.
- **`FastLED.show()` returns before the strip is latched.** It is asynchronous, and its internal
  yield can deschedule the calling task for milliseconds. The renderer composes into `leds[]` and
  `showFrame()` copies that into a separate `physLeds[]` for FastLED, so the shared frame is never
  transiently inconsistent -- which is why the flip does not mirror `leds[]` in place.
- **iOS caches the GATT table.** After adding/removing a characteristic and re-flashing,
  iOS serves the *stale* cached service list — toggle the iPhone's Bluetooth off/on (or forget
  the device) to force fresh discovery, or you'll see the old characteristic set.
- **Brightness 0 bricks the strip across reboots.** `brightness` is persisted and was not
  floored on load, so a persisted 0 made the strip invisibly dark on every boot. `loadParams()`
  now floors it (`constrain(brightness, 8, 255)`); runtime writes can still go dimmer.
- **Comet tail direction** is inferred from `prevPos` and lags ~1-2 frames at zero-velocity
  turnarounds; harmless given the bounding clip. (Was previously a visible cosmetic issue.)
- **Full-mode follow-through has no inboard finish marker** by design.

## Tests

Host-compiled, no hardware needed:

```
cc -O2 -o /tmp/golden firmware_tests/golden_kinematics.c -lm && /tmp/golden
cc -O2 -o /tmp/flip   firmware_tests/flip_mirror.c      -lm && /tmp/flip
sh firmware_tests/check_show_chokepoint.sh
node web/check_flip_mapping.js
```

Compile gate, **both** boards (they must both build; the flip touches no board-conditional code):

```
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" --warnings all tempo_trainer
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" --warnings all \
  --build-property "compiler.cpp.extra_flags=-DBOARD_DIY_ESP32" tempo_trainer
```

### Bench checklist for the flip (not yet run)

None of the above touches the flashed binary, so these still need a stick. Run on **both**
boards -- the Dig2go and the generic ESP32:

1. Flip mid-backswing while looping: the comet reflects with no phase jump and no tearing.
2. Flip during the dark rest gap, and while idle: the dot map appears for ~1.2 s and the amber
   dot is on the opposite side of the green ball.
3. Flip during the get-ready countdown.
4. Flip while `control == 2` (single) and let the stroke auto-return to idle.
5. **Flip, then pull the power while still looping, then reboot** -- orientation must survive.
   This is the case the rest-gap commit exists for; the stick has no power button.
6. Flip, join a set as a follower, disband, and confirm orientation survived the revert.
7. Blink-to-find while looping, before and after: the strip must never be left reversed.
8. Hammer `0014` at ~50 Hz from nRF Connect for a minute while looping, then stop: the strip
   must not end up stuck reversed.
9. Connect the iOS app to a stick flashed with the *old* firmware: it must still connect, with
   only the reverse toggle disabled.

## Current state

Working sketch implements everything above. **Tested on hardware end-to-end** with the iOS
app: discovery by service UUID, full 16-characteristic sync, live reach preview, mode switch,
transport control, the get-ready countdown, and the bounded comet on the strip.

[FastLED]: https://github.com/FastLED/FastLED
