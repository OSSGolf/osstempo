# osstempo — project notes

A TempoStik-style golf tempo trainer: ESP32 (classic WROOM-32, CP2102) drives a
144-LED/m WS2812B strip; a white "comet" animates a putt or full-swing stroke,
controlled over BLE. See `README.md` for the full hardware/firmware/physics spec.

## Layout
- `tempo_trainer/tempo_trainer.ino` — the firmware (Arduino C++, FastLED + ESP32 BLE + Preferences).
- `web/index.html` — Web Bluetooth control page (single self-contained file).
- **iOS app lives in the separate [osstempo-ios repo](https://github.com/OSSGolf/osstempo-ios)** (checked out locally alongside this repo, not inside it). Native SwiftUI + CoreBluetooth controller, dev-only (no App Store). **Implemented** (Swift 6, 4-layer: Domain/BLE/App/UI, pure reducer + `@Observable` shell); talks to the same BLE GATT. See its `DESIGN.md` + `README.md`. Build/test on the **iPhone 17** simulator (iPhone 16 isn't installed for iOS 26.5) with `-parallel-testing-enabled NO`.

## Firmware build (verified)
`arduino-cli` is installed. Compile-check after any firmware edit:

```
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" --warnings all tempo_trainer
```

- Board: **ESP32 Dev Module**, Partition: **Huge APP (3MB No OTA)** — BLE+FastLED overflow the default partition.
- ESP32 core 3.3.10 (v3.x) + FastLED already installed.
- Arduino auto-generates prototypes at the top of the .ino, so **types used in function
  signatures (`EaseFn`, `RefDot`) must be declared near the top**, before the first function.

## BLE reference
- Device name `PuttTempo`; service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`.
- **16 characteristics, tails `0002`–`0011`** (see README GATT table). Scan by **service UUID** (most reliable on iOS); the name is in the scan response. `control` (`0006`) supports NOTIFY.
- Per-mode params apply to the active mode; writing `mode` (`000a`) refreshes the per-mode characteristic values.
- `000e railsLit` (global) keeps the impact/aim dots lit; `get-ready ms` is a countdown of `round(ms/1000)` smooth impact pulses; the comet is bounded by the impact/aim markers per segment.
- `000f`-`0011` are the **full-swing purple aiming dot** (per-mode): enabled, absolute LED position (skips other dots), and appear-moment (0 address / 1 top / 2 downswing / 3 impact); stays lit until the next rep, independent of `railsLit`.

## Gotchas (cost real debugging time)
- **`createService()` GATT handles:** default 15 silently truncates the table after ~7 chars. Must use `createService(BLEUUID(SVC_UUID), 40)` for 16 chars. Symptom: client sees only the first 6-7 chars → "incompatible device".
- **iOS GATT cache:** after re-flashing with a changed characteristic set, toggle the iPhone's Bluetooth (or forget the device) to drop the stale cached table.
- **`plutil -extract <key> <fmt> <file>` without `-o -` overwrites the file in place** with the extracted value — always append `-o -` to print to stdout.
