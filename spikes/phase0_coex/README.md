# Phase 0 — Coexistence / render-timing spike (GATING)

**This is the go/no-go for the entire multi-stick sync feature.** It runs *first*, before any product
code changes, as a throwaway sketch (`phase0_coex.ino`). If it fails on either real board, the sync
feature does not proceed (or drops to a fallback per the plan's ladder). Nothing in `tempo_trainer/`
is touched by this phase. Source of truth: `TEMPO_SYNC_PLAN.md` §5.3 (exit criteria), §10 (Phase 0),
§11 (test methods).

> **Why this gates everything:** the leader runs BLE + WiFi/ESP-NOW on one radio while a WS2812 strip
> is refreshed on a tight grid, on a marginal 2 A supply. The single most likely failure is RMT
> ISR-refill starvation under WiFi (a corrupted/dropped frame), followed by heap exhaustion and
> leader brownout. All are measured here, on **both** the Dig2go and the generic-ESP32 build, before a
> line of the real renderer is written.

---

## What you need (bench)

- **Two sticks**: one `full-diy` (generic ESP32) **and** one `dig2go` — the spike must pass on both as
  *leader* (the highest-stress node). A phone (or nRF Connect) to hold a BLE connection to the leader.
- **A 2-channel oscilloscope** (authoritative for render-tick jitter and frame glitches). *Optional but
  strongly recommended* — the sketch also reports jitter on serial as a cross-check, so you can get a
  go/no-go number without a scope, but the scope is how you'll actually see a corrupted frame.
- The **README-spec 2 A supply** (the real, marginal one) for the brownout soak — not a beefy bench PSU,
  or you won't reproduce the field power condition.
- A serial monitor @ **115200** on each board.

**Scope hookup:** route `RENDER_TICK_PIN` (GPIO17) and `EVENT_PIN` (GPIO18) to the two channels. If
your board doesn't break those pins out, edit the two `#define`s at the top of the sketch to pins it
does expose. Trigger on `EVENT_PIN` to catch the BLE-subscribe flash event; free-run on
`RENDER_TICK_PIN` to watch the interval.

---

## Build & flash

Four builds must **compile clean (`--warnings all`)** — both boards × both drivers. (The agent has
already verified compilation; you just flash.) Select role/driver/board with `-D` flags:

```sh
FQBN="esp32:esp32:esp32:PartitionScheme=huge_app"

# LEADER, RMT driver, on the Dig2go:
arduino-cli compile --fqbn "$FQBN" --warnings all \
  --build-property "compiler.cpp.extra_flags=-DBOARD_DIG2GO -DROLE_LEADER=1 -DUSE_I2S_DRIVER=0" \
  spikes/phase0_coex
arduino-cli upload  --fqbn "$FQBN" -p <PORT> spikes/phase0_coex
```

Flags to vary:
- `-DBOARD_DIG2GO` **or** `-DBOARD_DIY_ESP32`
- `-DROLE_LEADER=1` (leader) **or** `-DROLE_LEADER=0` (follower)
- `-DUSE_I2S_DRIVER=0` (RMT, the default) **or** `-DUSE_I2S_DRIVER=1` (I2S/DMA — the bake-off)

For a delivery-% run you need **two boards up at once**: one leader, one follower, same driver.

---

## The five measurements (plan §5.3 exit criteria)

Run the whole matrix **twice** — once with `USE_I2S_DRIVER=0` (RMT) and once with `=1` (I2S/DMA) —
that comparison **is** the RMT-vs-I2S bake-off (criterion #4).

### 1. BLE up + ≥ 99 % beacon delivery over 10 min  — criterion #1
- Flash board A = leader, board B = follower (same driver). Connect the phone to the leader
  (`Phase0Spike`) and **subscribe** to its NOTIFY characteristic.
- Watch the **follower** serial: `[rx] beacons=… expected(seq-span)=… delivery=…%`. It flags
  `**UNDER 99%**` if it drops below.
- Watch the **leader** serial `[ble] up` stays `up` for the full 10 min (never `down`).
- **PASS:** delivery ≥ 99 % sustained, BLE never drops. Record the steady-state %.

### 2. Free heap ≥ baseline − 40 KB, stable  — criterion #2
- On the **leader**, note `[heap] baseline free = …` at boot, then watch `[heap] free=… min-ever=…`
  every 10 s for the 10-min run.
- **PASS:** `min-ever` stays **≥ baseline − 40 960 bytes**, and `free` shows no downward trend
  (no fragmentation leak). Record baseline, min-ever, and final free.

### 3. Render-tick jitter < 1 ms + no flash write mid-stroke  — criterion #3
- On the **leader**, watch `[render] … jitter us min/avg/max = … OK(<1ms)` — it flags `**OVER 1ms**`
  if `max ≥ 1000 µs`. This is the software cross-check.
- On the **scope**, free-run `RENDER_TICK_PIN`: the interval should be a rock-steady 8 ms; a WiFi/flash
  stall shows as a widened interval or a missed edge.
- **Flash-vs-render:** while scoping, **subscribe from the phone** (or toggle the subscription). The
  sketch pulses `EVENT_PIN` on the CCCD write and prints `[ble] CCCD write … watch the render tick`.
  Confirm the render interval on the other channel shows **no stall** coincident with the pulse —
  Bluedroid CCCD persistence should fire at *subscribe*, not mid-stroke.
- **PASS:** scope jitter < 1 ms under full coex load, no missed tick at a CCCD/subscribe event, serial
  never prints `**OVER 1ms**`. Record max jitter (scope + serial).

### 4. No corrupted/dropped frame — RMT-vs-I2S bake-off  — criterion #4
- Watch the LED strip **visually** on the leader under full coex for several minutes, each driver.
  Look for a flicker, a torn/garbage frame, or a stutter.
- **PASS:** clean strip, no corrupted/dropped frame. Note which driver(s) pass. **If RMT shows
  corruption and I2S does not → the fallback (plan §5.3-a) is I2S/DMA; record that as the chosen
  driver.** If both are clean, RMT is fine and simpler.

### 5. Leader power stability (10-min soak, full brightness + WiFi TX)  — criterion #5
- Leader on the **real 2 A supply**, full brightness (the sketch already sets 255), BLE connected +
  notifying, beacons at 4 Hz. Run **≥ 10 min**.
- If the board resets, on the next boot the banner prints `[boot] last reset reason:` — a
  `**BROWNOUT**` or `**…WDT**` there is a **FAIL**.
- **PASS:** zero resets over the soak (boot banner still shows `power-on`/`sw` after a manual reset,
  never brownout/WDT). If it fails → gate a supply/decoupling spec or a leader TX-power/brightness cap
  (plan §5.3-5) before proceeding.

---

## Results template (fill in — this is the gate record)

Run each row for **both** boards as leader, and for **both** drivers.

| Board | Driver | 1. Delivery % / BLE up | 2. baseline→min-ever heap (Δ) | 3. jitter max (scope/serial) | 4. frame integrity | 5. resets in 10 min |
|-------|--------|------------------------|-------------------------------|------------------------------|--------------------|---------------------|
| dig2go | RMT  |  |  |  |  |  |
| dig2go | I2S  |  |  |  |  |  |
| diy    | RMT  |  |  |  |  |  |
| diy    | I2S  |  |  |  |  |  |

**GO** if, for at least one driver, **all five pass on both boards** (that driver becomes the Phase-1
choice). **NO-GO / fallback** if no driver passes both boards — then apply the plan §5.3 ladder
(I2S already tried → serialize flash to dark window → drop FRAME_US → 2 Hz keepalive → relax BLE →
idle-only BLE writes) and re-run, or stop the feature.

Record the outcome and the chosen driver; Phase 1 (the analytic stateless renderer) is gated on a GO
here and inherits the driver decision.
