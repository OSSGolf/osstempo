# OSSTempo — Multi-Stick ESP-NOW Sync — Low-Level Design (v5.1)

**Status:** Implementation-ready LLD. **Passed L9 design review and an independent adversarial
red-team.** (v5.1 fixes two internal inconsistencies the red-team found in v5's own edits: the §8
drift rate is corrected to ~20–40 µs/s (was a self-contradictory "~1 µs/s"), and §5.1/§6 add a
fixed **rendezvous channel** so pairing still meets before hopping to the scanned operating channel
— plus two test-phasing tightenings (§10/§11). v5 resolved the adversarial conditions: the **load-bearing product requirement** that
justifies wireless-over-wired is now stated and the rejected alternatives argued (§1); the
**Phase-1 coex spike is resequenced ahead of the renderer refactor** (§10) so a no-go wastes
nothing; the **render-side fallback lever (I2S/DMA WS2812 driver)** is added to the ladder (§5.3);
and beacon **authentication (keyed MAC)**, a drift-tied **degrade threshold**, hardened
**event-beacon repeats**, **GPIO0-strapping-aware provisioning**, and **channel selection** are
added. See §19.)
**Author role:** Senior (L9) embedded engineer.
**Goal:** Run two (or N) osstempo LED sticks — any mix of the `full-diy` (generic ESP32) and
`dig2go` (QuinLED-Dig2go) builds — as a **tightly time-synchronized set** so they display the
identical tempo comet at the same instant ("smart alignment sticks"). Sync is **device-to-device
over ESP-NOW**; the phone app's topology is unchanged (it talks to one stick over BLE).

> **Hard requirement:** the comets must be visually locked. Target **< 3 ms** rendered skew in
> steady-state running. Both sticks render on a **shared, epoch-anchored frame grid** (§3.4) so
> they flip to the same frame at the same shared instant; skew reduces to **clock-offset +
> render-pacer jitter**, and both terms are given explicit, defensible mechanisms (§3.5, §4.3, §13).

---

## 0. Scope, non-goals, success criteria

**In scope**
- A leader/follower ESP-NOW protocol keeping N sticks phase-locked to one shared clock.
- A refactor of the blocking, *stateful* stroke animation into a **pure, analytic, stateless
  renderer** on a shared frame grid — the enabling decision for sync (§3).
- A defined **execution model** (task/core affinity + NVS-commit scheduling) that makes the
  render-pacer jitter bound real, not asserted (§3.5).
- Race-free propagation of run-state (`control`) and parameters (kinematic vs cosmetic, §7).
- Graceful degradation on loss / leader reboot; a preserved **standalone** single-stick mode
  with WiFi/ESP-NOW off (zero added cost in the common case).
- A validation method that *measures* skew and render stalls on real hardware.

**Non-goals**
- No change to the BLE GATT contract or the phone's one-stick model (one optional additive
  characteristic for follower health — §12).
- No WiFi-AP/router, internet, cloud, OTA; one leader per set.
- No change to the **physics** (`positionAt` is ported unchanged, golden parity §11). The
  *trail/tail* rendering is deliberately **redefined analytically** (§3.2); "byte-identical
  trails to today" is **not** a goal — visual equivalence is (§11 A/B gate).

**Success criteria (all *measured*)**
1. Two sticks side by side: **< 3 ms** rendered skew at impact (§11 scope method).
2. Skew bounded over a 30-min continuous loop (no cumulative drift).
3. A single dropped/late beacon → no visible desync (stateless renderer + coast, §8).
4. **No leader-only render hitch** from an NVS commit lands during a visible stroke (§3.5).
5. An unprovisioned stick is behaviorally identical to today, WiFi off (§9).
6. A **kinematic** parameter change while running switches all sticks on the same cycle
   boundary; **cosmetic** changes (brightness/rails) apply immediately, set-wide (§7).
7. Leader steady-state free heap ≥ (measured BLE+FastLED baseline − 40 KB) (§5.2).

---

## 1. Requirement, transport choice, and rejected alternatives

### 1.0 Load-bearing product requirement (states the premise the risk budget rests on)

The sticks must sync while: **(a) untethered** — each is placed at a user-chosen angle and the
golfer steps around/between them, so no wire may connect them; **(b) phone-independent at
runtime** — a rep must stay locked even if the phone walks away or its BLE link stutters; and
**(c) continuously drift-corrected** — locked indefinitely, not just at a one-time start. This
requirement is the *sole* justification for taking on the §5 coexistence risk; it is stated here
so the choice is auditable, not assumed. If a future product accepts a tether, §1.2 applies.

### 1.1 Why ESP-NOW over app-level BLE fan-out

App-level BLE fan-out is bounded by BLE timing (sequential, jittery 15–30 ms connection intervals
→ variable 15–60 ms start offset) unless you build a clock handshake + scheduled start — a sync
protocol anyway, still capped at BLE precision on two lossy links. ESP-NOW is connectionless,
~1–2 ms one-way, low-jitter, and lets a follower **free-run a deterministic animation off a
shared clock**. Residual skew is then set by the shared grid + execution model (§3), not the
transport.

### 1.2 Rejected alternatives (and the requirement that rejects each)

- **Wired sync line (a GPIO clock/trigger wire between sticks).** On the hard requirement it
  *dominates*: sub-µs sync, **zero RF, zero coex** — it would erase the entire §5 primary risk and
  even permit a hard-started *stateful* renderer (no §3 refactor). It is rejected **only** by
  requirement (a): alignment sticks are placed at user-chosen angles and stepped around, so a
  tether is a placement/trip regression unacceptable for the product. This is the honest and only
  reason wireless is worth its risk — recorded so the trade is explicit.
- **Phone-scheduled start (both sticks followers of a phone-issued start).** After the start both
  sticks free-run the same stateless animation, so steady-state skew is also drift-bounded — a
  fair design. Rejected by requirements (b) and (c): the phone becomes a runtime dependency, and
  the *one-time* offset estimate rides jittery BLE with no continuous re-anchor, so it is
  strictly worse than ESP-NOW on offset precision **and** drift correction. (Note both rejected
  options would also remove the §5 coex risk — an honest cost of choosing ESP-NOW.)

---

## 2. Architecture

### 2.1 Roles (data, not separate images)

| Role | Radios | BLE→phone | ESP-NOW | Behavior |
|---|---|---|---|---|
| **standalone** (default) | BLE only | yes | **off** | Behaviorally identical to today; zero coex/RAM cost. |
| **leader** | BLE + ESP-NOW | yes | broadcasts | Source of truth; owns the clock. |
| **follower** | ESP-NOW only | **off** | receives | Renders phase-locked to the leader. |

Only **provisioning** (§6) moves a stick off standalone → the common single-stick path never
turns WiFi on (criterion #5).

### 2.2 Data flow

```
        BLE (settings, control)            ESP-NOW broadcast (SyncBeacon @ event + 4 Hz)
Phone ──────────────────────►  LEADER  ═══════════════════════════════►  FOLLOWER(s)
(app)  ◄── notify(control) ──  (clock authority,                        (BLE off; render
                               render core 1 / radios core 0)            on shared grid)
```

Nothing is streamed per-frame; a lost beacon is coasted through (stateless renderer, §8).

---

## 3. The linchpin: an analytic, stateless renderer on a shared frame grid

### 3.1 Why the current renderer can't be shared as-is

Today's render is **stateful** — the buffer at cycle-time `u` depends on prior frames:
- `drawComet`/`drawCometRefs` do `fadeToBlackBy(leds,…,90)` then **add** the comet
  (`tempo_trainer.ino:337,350`) — "soft trails" are a temporal IIR of past frames.
- `addComet` latches tail direction from `static float prevPos`/`static int dir` with 0.01
  hysteresis, holding direction through turnarounds (`:316–321`) — a function of call history.
- Segment cadences differ (get-ready `delay(8)`, sweeps `delay(4)`, holds `delay(20)`).

So two nodes reaching the same `u` from different histories render different trails / opposite
tail direction. "Agree on `u` ⇒ identical frame" is false for this renderer. §3.2 fixes it.

### 3.2 Decision: redefine comet/trail/direction analytically & stateless

```c
// Draw one frame for absolute cycle-time u ∈ [0, cycleDuration). Clears + repaints from u.
// NO fadeToBlackBy, NO static state, NO delay, NO millis(). Pure in (u, p, control).
void renderFrameAt(uint32_t u, const Params& p, uint8_t control);
```

- **Position** from the *unchanged* pure `positionAt(u, p)` (ported segment/easing math:
  `easeInOut`/`easeOut`/`dropEase`, full-mode distance-split `:506–528`) — golden parity §11.
- **Direction** from the **segment identity**, not history and not a bare finite difference:
  each segment has a defined travel sign (get-ready = still; backswing = +; downswing = −;
  follow = −; hold = still). At `u = 0`, segment edges, and zero-velocity turnarounds the sign
  is taken from the *segment*, so it is fully deterministic on every node (no `static`, no
  finite-difference ambiguity at `dv/du = 0`). (Resolves v2 re-review NB#4.)
- **Trail** is an analytic brightness ramp trailing the head opposite `dir` — visually
  equivalent to the IIR "soft trail," a closed-form function of `u`. The IIR fade is dropped
  (validated equivalent, §11).
- **Rails / aim2** drawn from `u`: `aim2Now → aim2At(u, p)` (function of the aim2When boundary
  crossed by `u`), not a global side-effect.

**Determinism guarantee (now true):** `renderFrameAt` is a pure function of `(u, p, control)`,
so identical arguments ⇒ identical LED buffer on every node, regardless of history/start-phase
/re-anchor. This also removes the re-anchor glitch: a follower that corrects its clock renders
the correct frame next tick — no accumulated trail to disagree about.

### 3.3 Non-blocking main loop (render task)

```c
void renderLoop() {                          // the loopTask body; pinned to APP_CPU (§3.5)
  for (;;) {
    waitForGridTick();                       // block on a task-notify posted by the grid esp_timer (§3.5)
    serviceInbound();                        // drain BLE/ESP-NOW state deltas already parsed on core 0
    advanceCycleAndParamsAtBoundary(nowU());  // per-cycle epoch re-arm + boundary kinematic swap (§4.5,§7)

    uint8_t  c   = activeControl;
    uint32_t u_q = sharedGridU(c);           // §3.4 shared, epoch-anchored grid
    renderFrameAt(u_q, activeParams, c);     // includes FastLED.show(); runs ON the render core
  }
}
```

### 3.4 Shared frame grid (makes `show()` + quantization common-mode)

Both nodes render on a **grid of period `FRAME_US` anchored at the shared cycle epoch**:

```
k   = (nowShared − epochShared) / FRAME_US    // identical integer on all nodes at a shared instant
u_q = k · FRAME_US                            // the frame time both nodes render
```

- `nowShared` = leader clock; leader offset 0, follower `esp_timer + offset` (§4.3). Same origin
  ⇒ **all nodes compute the same `k` and render the same frame** — content-identical, not
  "within one frame."
- Each node schedules its next render at `epoch + (k+1)·FRAME_US` (local clock) → all flip to
  frame `k+1` at the same shared instant.
- `FRAME_US > show()+compute+wake`. For 144 WS2812B, `show()` ≈ 144×24×1.25 µs ≈ **4.32 ms**;
  `FRAME_US = 8000` (125 Hz), tightenable toward ~6 ms if the loop budget allows.
- **`show()` is common-mode latency, not skew:** both nodes start `show()` for frame `k` at the
  same shared instant, so its 4.32 ms cost cancels between nodes. (This is why v1's budget was
  wrong.) Smoothness (~8 ms period) is unchanged from today; both sticks step *together*.

### 3.5 Execution model — task & core affinity + NVS scheduling (makes pacer jitter real)

After §3.4, the *entire* residual budget is `offset error + render-pacer jitter` (§13). Jitter
is only sub-ms if the render is protected from the radio path and from flash stalls. This is
specified, not assumed:

- **Core affinity.** The **render task (`renderLoop`, incl. `FastLED.show()`) is pinned to
  APP_CPU (core 1)**; **WiFi/Bluedroid/ESP-NOW run on PRO_CPU (core 0)** — the Arduino-ESP32
  default, made explicit and load-bearing here. Radio TX/RX callbacks and beacon parsing run on
  core 0 and only hand *parsed deltas* (epoch/offset/params/control) to the render task via a
  small lock-free mailbox; the render task never blocks on the radio. **Mailbox discipline:** the
  ~40-byte `Params`/epoch payload cannot be swapped atomically, so the mailbox is a **double
  buffer with an atomic generation index** (or a seqlock): core 0 writes the inactive slot then
  publishes the index; the render task reads the published slot. This prevents a torn read (which
  would otherwise cost one wrong frame — bounded and self-correcting under the stateless renderer,
  but avoided outright). Do not roll an unsynchronized mailbox.
- **The grid pacer only *wakes* the render task.** A one-shot `esp_timer` armed for the next
  grid boundary fires a callback (esp_timer task, high priority) that does nothing but
  `xTaskNotifyGive` the render task. `FastLED.show()` runs on the render task on core 1, **never
  in the timer callback**. Jitter = notify + scheduler latency on a lightly-loaded core 1 → sub-ms.
- **NVS commits are scheduled off the visible render (fixes the leader-only stall).** A flash
  write disables the instruction cache on **both** cores for its duration (tens of ms), stalling
  any non-IRAM render code — a WS2812-timing hazard, and leader-only (BLE writes originate
  there), so it would inject an *asymmetric* skew transient mid-stroke ~1.5 s after a tuning
  change (`:606`). Mitigation: **`saveParams()` runs only while `control == idle` or inside the
  dark `rest` gap between reps — never during get-ready/backswing/downswing/follow/hold.** The
  leader owns commit timing, so it defers a dirty-param commit to the next rest/idle window;
  a stall in a dark window is invisible and does not perturb the synced comet. **`restMs == 0`
  case:** a continuous loop can have no dark rest gap, so the commit is deferred all the way to
  `control == idle` (the params stay correct in RAM and are beacon-propagated meanwhile); the
  leader **never** falls back to a mid-stroke commit when no rest window exists. Followers hold
  live params in RAM from beacons and do **not** commit them, so they have no such stall.
- **Standalone** uses the same render task/core model minus the radios (criterion #5).

This is the mechanism behind the §13 "pacer jitter < 1 ms," and its failure mode
(render-stall/pacer-miss) is part of the Phase-0 coex-spike go/no-go and fallback ladder (§5.3).

---

## 4. Shared clock & the sync protocol

### 4.1 Time base
**All timestamps are `esp_timer_get_time()` (signed 64-bit µs)** — not `micros()` (32-bit,
wraps ~71 min) nor `millis()`. 64-bit removes wrap concerns for any session length.

### 4.2 `SyncBeacon` (ESP-NOW payload, ≤ ~90 B; limit 250 B)

```c
struct SyncBeacon {                  // packed, little-endian, versioned
  uint8_t  magic[2];                 // 'O','T'
  uint8_t  version;
  uint16_t groupId;                  // set identity; foreign groups ignored (§6)
  uint32_t seq;                      // monotonic; resets on leader reboot (loss/reorder/reboot detect)
  int64_t  txEspTimer;               // leader esp_timer µs captured just before esp_now_send (§4.3)
  uint8_t  control;                  // 0 idle / 1 loop / 2 single / 3 preview
  int64_t  epochEspTimer;            // leader-clock start of the CURRENT cycle/stroke (§4.5)
  Params          active;            // full authoritative params in effect THIS cycle (kinematic + cosmetic)
  KinematicParams pending;           // ONLY the kinematic fields to merge at the NEXT boundary (§7)
  uint8_t         hasPending;        // 0/1
  uint8_t         authTag[8];        // keyed MAC over all preceding bytes (§4.2.1) — replaces a bare CRC
};
```

**§4.2.1 Authentication (broadcast is unauthenticated by default).** ESP-NOW's built-in
encryption is unicast-only, so a broadcast design is unauthenticated on the wire: `groupId` is
cleartext, and a sniffer could spoof beacons carrying it to inject a bad `epoch`/`params` and
desync or hijack a neighboring set (plausible in a shop with several sets, or a prankster). So the
beacon carries a truncated **keyed MAC** (e.g., HMAC-SHA256/CMAC → 8 bytes) over all preceding
bytes, keyed by a **pairing-derived group key** minted at pairing (§6) and stored in NVS alongside
`groupId`. Receivers drop any beacon whose tag doesn't verify — this both replaces the integrity
CRC and closes spoofing. (`seq` is included under the tag; a replay of an old beacon fails the
monotonic-`seq`/epoch checks.) The MAC is cheap on ESP32 (hardware SHA); verify it on core 0
before the beacon touches the render mailbox.

`Params` = compact wire form of the active `Profile` + globals, partitioned into two field
groups: **`KinematicParams`** (feed `positionAt`/`cycleDuration`/timeline: backswing, downswing,
reach, release, follow, topHold, getReady, hold, rest, mode, aim2When) and **cosmetic** fields
(brightness, rails, aim2 position/enabled). `pending` carries **only the kinematic group** — it
is *not* a full `Params` — so a staged kinematic change can never overwrite a live cosmetic edit
at the boundary (§7). ~30–40 B total. **Full params every beacon** ⇒ a follower is
self-describing; no separate param provisioning, no "missed-update" state.

### 4.3 Clock-offset estimation (min-filter + round-trip calibration)
`sample = (txEspTimer + OWD) − rxEspTimer` (`rxEspTimer` stamped in the RX callback, core 0).
- The RX callback runs in the WiFi/lwip task (not a hard ISR), so a coex-delayed beacon inflates
  its apparent delay. Keep a **16-sample window and select the minimum-delay sample** (NTP
  trick) → offset error bounded by minimum-delay jitter.
- **`OWD` is calibrated** via a lightweight **round-trip ping-pong** at pairing + periodically
  (follower→leader→echo), estimating one-way delay as `RTT/2` over min-filtered samples,
  removing the leader-vs-follower nominal bias. **Caveat (honest floor):** leader→follower and
  follower→leader traverse different coex conditions (leader carries BLE, follower doesn't), so
  `RTT/2` retains a small asymmetry bias; min-filtering bounds it but it is a *floor*, not zero.
- Chosen offset applied with **slew** (small: ramp over frames; large / `seq` reset: snap at a
  cycle boundary). Stateless renderer ⇒ a snap yields the correct frame with no artifact.

Inter-beacon drift ~5 µs (±20 ppm × 250 ms) — followers coast for seconds within budget.

### 4.4 Beacon cadence
- **Keepalive:** 250 ms (4 Hz).
- **Event beacons:** on `control` change, staged (kinematic) param change, **cosmetic param
  change**, or boundary crossing — immediate, **repeated 4–5× over a few hundred ms** (still ≪ one
  cycle). A kinematic-change beacon that is *jointly* lost desyncs a follower for a **full rep**
  (hundreds of ms of comet divergence, §8), so the repeat count is sized to drive joint-loss
  probability far down, not just to survive a single drop. (Cosmetic changes are event-triggered
  too, so a follower sees them within ~one OWD, not just at the next keepalive — consistent with
  the sub-100 ms figure in §7.)

### 4.5 Epoch model — per-cycle re-arm
Epoch = start of the current cycle, re-armed each boundary (matching today's snapshot-at-stroke
-start `:425`), **not** a fixed origin with `now % T`:
- **Loop (1):** at each boundary `epoch += cycleDuration(activeParams,1)`, then apply any pending
  kinematic swap (§7). Each cycle starts at `u = 0` under the then-active params → phase is
  continuous across changes; no `u % T` discontinuity.
- **Single (2):** `epoch = now` on trigger; every node renders one stroke and **independently**
  returns to idle at `cycleDuration(_,2)` (no rest) — robust to losing the trailing idle beacon.
  Leader also notifies the phone.
- **Preview (3) / Idle (0):** static; epoch irrelevant.

All nodes share `epoch` + `activeParams` ⇒ identical boundary instants ⇒ swap together.

---

## 5. Radio coexistence — the primary risk (leader only)

Leader runs **BLE (Bluedroid) + WiFi/ESP-NOW** on one radio. **Gated by a mandatory Phase-0
spike (run first, as a throwaway sketch — §10)** before any protocol or renderer work.

### 5.1 Known-good leader config
`WiFi.mode(WIFI_STA)` without AP; `WiFi.setSleep(false)` + `esp_wifi_set_ps(WIFI_PS_NONE)`;
`esp_wifi_set_channel(CH, WIFI_SECOND_CHAN_NONE)`; `esp_now_init()` + broadcast peer `FF:…:FF` on
`CH`. **Channel selection (two-channel scheme, so pairing still has a rendezvous):** a **fixed
rendezvous channel `CH_PAIR` (default 1)** is used **only** for the pairing handshake (§6) — both
nodes are guaranteed to meet there. Pairing then agrees an **operating channel `CH_OP`** (picked
by a brief least-busy scan at that moment, avoiding 1/6/11 where possible) and both store it in NVS
with the group; after pairing, all nodes of the set hop to `CH_OP` for the beacon traffic. This
keeps steady-state off the busiest default while leaving pairing a deterministic meeting point
(the v5 "channel = hash(groupId)" idea was circular — `groupId` is minted *during* pairing — so it
is dropped). Software coex on by default; BLE conn interval relaxed, beacons small/infrequent.
Radios on core 0, render on core 1 (§3.5).

### 5.2 Memory
Bluedroid (~60–100 KB) + WiFi (~50 KB + buffers) on a WROOM-32 (~300 KB usable heap) is heavy;
flash is fine (Huge APP 3 MB; WiFi in-SDK). **Method:** record **today's steady-state free heap**
(BLE+FastLED, no WiFi) as baseline; the spike measures the WiFi/ESP-NOW **delta** and requires
≥ 40 KB residual, no fragmentation trend. Followers carry only WiFi.

### 5.3 Spike exit criteria (go/no-go) — real Dig2go AND generic ESP32 as leader
1. Live BLE + 4 Hz beacons for 10 min: **BLE stays up, ≥ 99 % beacons delivered**.
2. Free heap ≥ baseline − 40 KB, stable.
3. **Render-timing under load (the v2-review gate):** with radios on core 0 and render on core 1,
   the grid pacer holds `FRAME_US` with **< 1 ms jitter**, and **no flash write of any kind lands
   during a visible stroke**. Instrument this broadly — trap all `spi_flash`/`esp_partition`
   writes (app `saveParams()` *and* any Bluedroid bond-key / CCCD-subscription persistence, which
   normally fire at connect/subscribe before running, not mid-stroke) — and confirm none coincide
   with a visible stroke, rather than only asserting `saveParams()` deferral. Measure render-tick
   jitter directly (GPIO toggle per grid tick on a scope).
4. No visible FastLED flicker or **corrupted/dropped frame** from radio/flash stalls. **Named
   hazard:** on the classic WROOM-32 (no RMT DMA) FastLED's clockless driver refills the RMT
   buffer via ISR mid-transmission (144×24 = 3456 bits ≫ the ~512-item RMT memory, so refill is
   mandatory); a refill delayed by WiFi housekeeping corrupts a frame or misses a grid tick =
   an ~8 ms one-stick skew transient. This is the single most likely no-go and is measured here.

Fallbacks if any fail, in order — **the first is the render-side lever the v4 review lacked:**
(a) switch the WS2812 output to FastLED's **I2S/DMA clockless driver** (`FASTLED_ESP32_I2S`) — DMA
on the classic ESP32 is immune to ISR-refill starvation under WiFi, directly curing the named
hazard (an RMT-vs-I2S bake-off is part of the spike); (b) serialize **all** flash writers to the
dark rest/idle window (note the WS2812 render path is not IRAM-safe, so "move render to IRAM" is
*not* a usable mitigation); (c) drop `FRAME_US` to a lower refresh (fewer refills/less airtime
contention); (d) 2 Hz keepalive + coast; (e) relax BLE conn interval/MTU; (f) last resort — leader
accepts BLE writes only while idle (also removes the mid-stroke commit path). **No Phase 2+ until
the spike passes**, render-timing gate included.

---

## 6. Provisioning: role, group, channel (button + NVS)

- **Factory default:** `role = standalone`, WiFi off (today's behavior).
- **Pairing (on the fixed rendezvous channel `CH_PAIR`, §5.1):** both nodes switch to `CH_PAIR`
  for pairing. Hold GPIO0 on the **leader** → pair-advertise (mints `groupId` + group key, scans
  and picks `CH_OP`, broadcasts them on `CH_PAIR`); hold GPIO0 on the **follower** → pair-listen on
  `CH_PAIR` → adopts the `groupId` + group key + `CH_OP`, sets `role = follower`, stores all in NVS,
  then both hop to `CH_OP` and run the round-trip OWD calibration (§4.3). Leader flips to
  `role = leader` on hearing a follower `hello`; zero followers = standalone-with-WiFi (harmless).
- **`groupId`** (16-bit) + **group key** (for the beacon `authTag`, §4.2.1), both minted at first
  pairing, namespace a set; foreign/unauthenticated beacons dropped.
- **Follower `hello`** (~1 Hz): MAC + role + link health → leader reports to phone (§12); off the
  sync path.
- **Factory reset:** long button hold clears NVS role/group → standalone.
- **Physical-control reality (both boards):** the only exposed control is **BOOT / GPIO0**, a
  **strapping pin** (`docs/dig2go/README.md`). It must be read as a **runtime press** (held
  *after* boot) — a hold *at power-on* enters the ROM bootloader and cannot be a pairing gesture.
  So all button gestures above are runtime GPIO0 holds; the earlier "held-boot-pin pairing" idea is
  dropped. The board-agnostic fallback for pairing is a **one-time BLE command on the leader**
  (mint group/key, set channel) plus proximity/first-heard adoption on the follower via a runtime
  GPIO0 hold. Per-board button GPIO stays a `#define` beside `BOARD_*` for any board that adds a
  dedicated button.

---

## 7. Parameter changes while running — kinematic vs cosmetic

Two classes, handled differently so live brightness isn't regressed (v2-review NB#2) — and,
critically, the boundary operation is a **field-level merge**, never a whole-struct swap, so a
staged kinematic change cannot silently revert a live cosmetic edit (v3-review blocker #1):

- **Kinematic params** (feed `positionAt` / `cycleDuration` / the timeline: backswing, downswing,
  reach, release, follow, topHold, getReady, hold, rest, mode, aim2When): a BLE write **stages
  only the kinematic fields into `pending`** (a `KinematicParams`, §4.2). At the next cycle
  boundary (§4.5) every node — **including the leader** — performs
  `mergeKinematicFields(active, pending)`: it copies **only the kinematic fields** from `pending`
  into `active`, leaving every cosmetic field in `active` untouched. Shared `epoch` +
  `activeParams` ⇒ same boundary, same cycle; phase-continuous; mirrors today's
  snapshot-at-stroke-start (`:425`); no `u % T_new` jump. Latency ≤ one cycle (change lands next
  rep; reach is normally tuned in preview/stop).
- **Cosmetic params** (do **not** affect `u`/phase: brightness, rails, aim2 position/enabled):
  **never staged.** A BLE write mutates the cosmetic field **in place in `active`** immediately
  and it is beacon-propagated (carried in `active`), preserving today's live brightness (`:234`).
  Because cosmetic fields live only in `active` and are never in `pending`, the boundary merge
  cannot touch them. A follower may show a cosmetic change up to ~one beacon later than the
  leader — a sub-100 ms overlay difference, not a timeline desync.
- **The reverting bug this avoids (was v3 §7):** change tempo (stages kinematic `pending`), then
  change brightness within the same rep (mutates `active.brightness` live). A whole-struct
  `active ← pending` at the boundary would restore the *stale* brightness snapshot. The
  field-level merge leaves `active.brightness` alone, so the live brightness survives. (§11 tests
  this exact interaction.)
- **`control` transitions are immediate** (idle/preview static; Run/Once reset `epoch = now` via
  event beacon), never boundary-gated.
- **Follower ingestion (mirror of the fix on the receive side):** a follower adopts the beacon's
  **cosmetic** fields from `active` immediately, but changes **kinematic** fields only at a cycle
  boundary (via the same `mergeKinematicFields` from its held `pending`, or the §8 late-reconcile,
  which is boundary-timed). A follower must **not** copy `beacon.active` wholesale each beacon —
  that would apply a kinematic change mid-cycle and reintroduce the fixed bug on the receive side.
- **Preview/idle (static, not time-synced):** there is no cycle boundary, so staging has no
  trigger — **both** kinematic and cosmetic changes apply **immediately** in preview (3) and idle
  (0). Boundary-staging applies only to the running loop (1) / single (2) modes. This preserves
  the hardware-tested live reach-preview workflow (README "Setting the backswing point").
- **Cross-core safety:** every in-place `active` mutation (cosmetic write, or the boundary
  kinematic merge) is published to the render task via the §3.5 double-buffer/atomic-generation
  mailbox — never a raw write to the slot the render task is reading.

---

## 8. Failure modes & degraded behavior

| Condition | Detection | Behavior |
|---|---|---|
| Single beacon lost/late | seq gap / interval miss | **Coast** on last `(epoch, offset, params)`; stateless ⇒ no artifact, drift µs-scale. |
| Sustained loss (a body/club blocks the set mid-swing) | beacon-age timer | **Keep coasting** — relative drift between two independent ±20 ppm clocks is **~20–40 µs/s** (consistent with §4.3's ±20 ppm × 250 ms ≈ 5 µs/beacon), so the set stays locked to well under a frame for *tens of seconds* (e.g. 30 s × 40 µs/s ≈ 1.2 ms ≪ the 8 ms frame). Do not degrade prematurely. |
| Lost sync (beacon-age > **DEGRADE_S**, tied to the drift budget: ~15–30 s, **not** 2 s) | beacon-age timer | **Degraded visual** (slow amber breathing / dim) so real loss is *visible*. **Derive `DEGRADE_S` from the ~20–40 µs/s relative-drift rate** (at 40 µs/s, 30 s ≈ 1.2 ms &lt; the 3 ms budget) — not from a mis-stated rate — so a normal mid-swing block never trips a false "lost sync." |
| Kinematic-change beacon fully lost before the boundary | follower's locally-applied `active` ≠ beacon `active` next cycle | Follower may swap **one cycle late**; self-corrects at the next boundary from the beacon's `active` (which every beacon carries). Low-probability, bounded, non-permanent. |
| Beacon returns | fresh beacon | Re-anchor: slew small offset; snap at boundary for large; stateless ⇒ correct frame at once. |
| Leader reboot | `seq` reset / epoch jump | Large discontinuity → re-anchor at next boundary. |
| Two sets in one room | `groupId` mismatch + channel spread (§5.1) | Foreign beacons dropped; sets tend to different channels. |
| Spoofed / replayed beacon (unauthenticated broadcast) | `authTag` verify fails (§4.2.1) / stale `seq`,`epoch` | Dropped on core 0 before the render mailbox; coast. Closes hijack/desync attacks. |
| NVS / any flash write vs render | writes gated to idle/rest (§3.5, §5.3-3) | Cache stall lands in a dark window → invisible. |
| RMT-refill starvation under WiFi | corrupted/missed frame (Phase-0 spike, §5.3) | Cured by the I2S/DMA driver fallback (§5.3-a); measured before Phase 2. |
| Clock wrap | 64-bit `esp_timer` µs | Non-issue. |
| Corrupt beacon | `authTag` + ESP-NOW CRC | Dropped; coast. |
| Cross-build float/scale8 divergence | — | Pin identical ESP32-core + FastLED versions across a set (§14). |

---

## 9. Firmware module / file changes

```
tempo_trainer/tempo_trainer.ino
  ├─ render/        NEW  positionAt(u,p) [ported, golden-parity] + renderFrameAt(u,p,c)
  │                       [analytic, STATELESS: no fadeToBlackBy, no static dir, direction from
  │                        segment identity, aim2At(u)].
  ├─ grid/          NEW  esp_timer base; sharedGridU(); one-shot grid esp_timer → xTaskNotifyGive
  │                       the render task (render + show() pinned to APP_CPU/core 1) (§3.4/§3.5).
  ├─ sync/          NEW  SyncBeacon (de)serialize; offset min-filter + round-trip OWD calib;
  │                       slew/anchor; per-cycle epoch re-arm; boundary kinematic swap; scheduler.
  ├─ espnow/        NEW  WiFi/ESP-NOW init on core 0; broadcast peer; tx/rx callbacks.
  ├─ provisioning/  NEW  role/group/channel in NVS; button state machine; factory reset.
  └─ (existing)          BLE server, Preferences, FastLED. BLE write handler now stages kinematic
                        params / applies cosmetic immediately (§7); saveParams() gated to idle/rest
                        (§3.5); blocking runStroke()/runStrokeFull() retired for renderFrameAt.
```

- **Standalone parity:** links the sync code but never initializes WiFi/ESP-NOW; drives
  `renderFrameAt` from its own `esp_timer` grid on core 1 → *behaviorally* identical to today
  (criterion #5). Trail is the new analytic one (§3.2), validated equivalent (§11), **not** the
  old IIR trail byte-for-byte (that would defeat statelessness — §17).
- Per-board `#define`s (button GPIO) beside `BOARD_*`. Compile both variants each phase:
  `arduino-cli compile --fqbn "...PartitionScheme=huge_app" --warnings all tempo_trainer`.

---

## 10. Phased rollout

The spike is deliberately **first** (before the renderer refactor): it needs no product-code
change, so a no-go wastes nothing and never risks a visual regression in the shipped standalone
product. (This resequencing is the v5 fix for the adversarial review's phase-ordering finding.)

- **Phase 0 — Coex + memory + render-timing spike (GATING, throwaway sketch).** A minimal sketch —
  WiFi/ESP-NOW 4 Hz beacons on core 0 + a *dummy* FastLED animation on core 1 + a live BLE
  connection — exercises §5.3's exit criteria (BLE stability, ≥ 99 % delivery, heap delta,
  render-tick jitter, **RMT-vs-I2S/DMA bake-off**, no flash write mid-frame) on **both real
  boards**. No `renderFrameAt` refactor required. **Go/no-go for the whole approach.**
- **Phase 1 — Analytic stateless renderer + execution model.** (Only if Phase 0 passes.) Blocking
  stateful stroke → `renderFrameAt`/`positionAt`, render task on core 1, `esp_timer` grid pacer
  via task-notify, NVS commit gated to idle/rest. **Gate:** (a) `positionAt` golden parity; (b)
  A/B visual equivalence (§11); (c) single-stick render-tick jitter < 1 ms (radios idle).
- **Phase 2 — Steady-state sync.** Beacon (`authTag` present but keyed by a **fixed placeholder
  key** — real pairing-derived keying lands in Phase 5, so Phases 2–4 are *not yet*
  spoof-resistant, which is fine for bench dev) + offset min-filter + round-trip OWD + shared grid.
  **Gate:** measured skew **< 3 ms** at impact on a running loop.
- **Phase 3 — State machine.** `control` transitions; deterministic independent stops.
- **Phase 4 — Param propagation.** Boundary kinematic swap + immediate cosmetic. **Gate:** a
  running kinematic change switches all sticks on the same cycle; brightness changes live.
- **Phase 5 — Provisioning.** Button/NVS role/group/channel; standalone preserved.
- **Phase 6 — Robustness + app.** Coast/degraded/anchor-slew; optional phone "N synced" UI.
- **Phase 7 — Validation + docs.** Full measurement suite (§11) + `docs/sync/` guide.

---

## 11. Testing & validation (must *measure*)

- **Kinematics golden parity (Phase 1):** sample `positionAt(u,p)` vs pre-refactor math (reuse
  `dump_kinematics.c` golden). Zero drift.
- **Visual A/B (Phase 1):** trail/tail is analytically redefined → **human side-by-side A/B**
  (old vs new build) **blessed by the product owner**, with an **objective still-frame diff of
  the head + rails** (which are exact) as the backstop. Re-baseline the golden trail reference.
- **Render-tick jitter:** GPIO toggle per grid tick on a scope; **< 1 ms** under coex load (Phase
  0 spike, §5.3-3) and with radios idle (Phase 1).
- **Involuntary-flash-vs-render (Phase 0 spike):** the spike has only a *dummy* animation and no
  gated `saveParams()`, so it tests the **involuntary** writers that exist without app code —
  Bluedroid bond-key/CCCD persistence and PHY-calibration — confirming they fire at
  connect/subscribe/boot, **not** mid-frame. **RMT-vs-I2S bake-off:** confirm the chosen WS2812
  driver shows no corrupted/missed frame under WiFi load (§5.3-a).
- **`saveParams()` deferral (Phase 1/4, once the real stroke state machine + gating exist):** force
  a mid-stroke tuning change; confirm the app NVS commit is deferred to the rest/idle window and no
  flash write lands during a visible stroke (scope the render-tick GPIO); include the `restMs == 0`
  loop case (deferred to idle).
- **Cosmetic-survives-boundary (Phase 4):** change tempo (stages a kinematic `pending`), then
  change brightness within the same rep; verify the brightness change persists **through** the
  next cycle boundary on the leader and all followers (the field-level merge, §7) — the specific
  interaction the whole-struct swap would have reverted.
- **Skew (Phase 2/4, core proof):** each stick toggles a GPIO when its shared-grid `k` crosses
  the impact frame; two GPIOs on a 2-channel scope read edge-to-edge skew. **Target < 3 ms.**
- **Drift stability:** 30-min loop; sampled skew bounded.
- **Loss resilience:** attenuate/inject drops; coast → degraded → clean recovery; verify the
  one-cycle-late kinematic-swap corner (§8) self-corrects.
- **Coex/memory (Phase 0):** §5.3.
- **Spoof/replay (Phase 6):** inject a foreign-key beacon and a replayed old beacon; verify both
  are dropped on `authTag`/`seq` (§4.2.1) and never reach the render mailbox.
- **Regression:** standalone manual + BLE unchanged; both variants compile `--warnings all`.

---

## 12. App-side impact (minimal, additive)

Phone still connects to one `PuttTempo` (the leader); the de-gated app needs no structural
change. **Optional** `…0012` follower-status characteristic (read+notify): leader reports
follower count/health (§6 hellos); app shows a "2 sticks · synced" chip. Off the sync path.
Future app-driven pairing (set `groupId` over BLE) is an open item (§14).

---

## 13. Precision budget (why < 3 ms holds — with mechanisms)

| Source | Magnitude | Mechanism that bounds it |
|---|---|---|
| Clock offset error | **< 1 ms (measured Phase 2)** | min-filtered, round-trip-calibrated OWD (§4.3); residual asymmetry is a small floor. |
| Render-pacer jitter | **< 1 ms (measured: Phase 0 under load, Phase 1 idle)** | render+`show()` pinned to core 1; radios on core 0; grid `esp_timer` only *wakes* the task; NVS/flash gated to idle/rest; I2S/DMA driver if RMT starves (§3.5, §5.3). **No longer asserted.** |
| `FastLED.show()` (4.32 ms) | **0 skew** | common-mode: both nodes start `show()` for frame `k` at the same shared instant. |
| Frame quantization | **0 skew** | both compute the same `u_q = k·FRAME_US` from the same epoch. Contributes coarseness (~3 LED steps at peak, same as today), not skew. |
| Inter-beacon drift | ~5 µs | ±20 ppm × 250 ms; re-anchored each beacon. |
| Param staleness (steady state) | 0 | full params/beacon + boundary swap (§7). |

Steady-state skew ≈ offset + pacer jitter ≈ **< 2 ms**, target < 3 ms with margin — below human
perception, sub-2-LED at peak comet speed. Both terms are now measured, not assumed; the grid
makes `show()`/quantization common-mode; the execution model (§3.5) makes the pacer bound real.

---

## 14. Open decisions (resolved defaults + remaining)

**Resolved:** spike-first phasing (Phase 0 throwaway coex/RMT go/no-go before the refactor);
analytic stateless renderer (direction from segment identity) + shared `esp_timer` frame grid +
**render/core-1, radios/core-0 execution model with NVS-gated-to-idle/rest** (I2S/DMA driver if RMT
starves); role via NVS + **runtime GPIO0** (default standalone); event (×4–5) + 4 Hz keepalive;
offset via min-OWD 16-sample window + round-trip calibration (asymmetry floor acknowledged),
slew/anchor; **kinematic params at cycle boundaries, cosmetic immediately**; 16-bit `groupId` +
**pairing-derived group key (beacon `authTag`)** + **pairing-chosen channel** (not fixed 1),
broadcast; **`DEGRADE_S` tied to the drift budget (~15–30 s, not 2 s)**; followers BLE-off;
`FRAME_US = 8 ms` (tightenable).

**Remaining (do not block Phase 0):** whether a dedicated button is added on a future board vs
runtime-GPIO0 / BLE-command provisioning; final `FRAME_US`, `DEGRADE_S`, `OWD` cadence, and MAC
algorithm from bench; ship the follower-status BLE characteristic now or defer; exact analytic
trail shape (Phase 1 A/B); the pinned ESP32-core + FastLED versions for a set.

---

## 15. First step

**Phase 0 (the throwaway coex/memory/render-timing spike, incl. the RMT-vs-I2S bake-off) is the
true go/no-go and must run first** — it needs no product-code change, so a no-go costs nothing and
never risks the shipped standalone product. Only on a pass does Phase 1 (the analytic stateless
renderer + execution model, with its golden-parity + A/B gates) begin. Everything downstream is
gated on Phase 0.

---

## 16. v1 → v2 changelog (first L9-review resolutions)

- **Renderer determinism (v1 #1,#5):** current renderer is stateful (IIR trails + static-latched
  direction + per-segment cadence) → "pure function of `u`" and "byte-identical" were mutually
  exclusive. v2 redefined comet/trail/direction **analytically & stateless** (§3.2), dropped
  "byte-identical," re-baselined golden to kinematics + a visual A/B gate.
- **Budget & frame alignment (v1 #2,#3):** v1 treated `delay(4)` as the frame period and ignored
  `show()` (~4.32 ms) + independent pacers. v2 introduced the **shared epoch-anchored frame grid**
  → `show()`/quantization common-mode (zero skew).
- **Param swap (v1 #4):** v1's fixed-epoch `u % T` swap caused a phase discontinuity. v2 →
  **per-cycle epoch re-arm** + **cycle-boundary** param application.
- **Time base / OWD / heap / cross-build:** `esp_timer` 64-bit µs; round-trip OWD calibration;
  heap baseline-then-delta; pin core+FastLED versions.

## 17. v2 → v3 changelog (second L9-review resolutions)

- **Render-timing guarantee (v2 re-review blocker):** after the grid insight, skew =
  `offset + pacer jitter`; v2 asserted "pacer jitter < 1 ms" with no mechanism and ignored two
  stalls. v3 adds the **execution model (§3.5)**: render+`show()` pinned to **APP_CPU (core 1)**,
  radios on **PRO_CPU (core 0)**, the grid `esp_timer` **only wakes** the render task (never runs
  `show()` in the callback), and **NVS commits gated to the idle/rest window** so a flash
  cache-stall never lands mid-stroke on the leader. The render-timing failure mode is added to the
  **coex-spike go/no-go and fallback ladder (§5.3-3)** and to the budget as a *measured* term (§13).
- **Cosmetic vs kinematic params (NB#2):** brightness/rails apply **immediately** (today's live
  brightness preserved); only kinematic params are boundary-gated (§7).
- **Kinematic-change-beacon-loss corner (NB#1):** explicit §8 row — one-cycle-late swap that
  self-corrects from the beacon's `active`.
- **OWD asymmetry (NB#3):** `RTT/2` acknowledged as a floor, not exact (§4.3).
- **Direction at `u=0`/turnarounds (NB#4):** taken from **segment identity**, fully deterministic,
  no finite-difference ambiguity (§3.2).
- **A/B subjectivity (NB#5):** A/B gate **blessed by the product owner** with an **objective
  head/rails still-frame diff** backstop (§11).

## 18. v3 → v4 changelog (third L9-review resolutions)

- **Param-merge self-contradiction (v3 re-review blocker):** v3's cosmetic/kinematic split paired
  a per-field cosmetic rule with a **whole-struct** boundary swap (`Params pending`, `active ←
  pending`), so a brightness edit made after a staged tempo change was **reverted** at the
  boundary — breaking criterion #6. v4 makes **`pending` kinematic-only** (`KinematicParams`,
  §4.2) and the boundary a **field-level merge** (`mergeKinematicFields`, §7); cosmetic fields are
  never staged and are mutated in place in `active`. Added the §11 cosmetic-survives-boundary test.
- **Cross-core mailbox discipline (NB#1):** named as a **double-buffer + atomic generation index**
  (or seqlock) to prevent a torn `Params` read (§3.5).
- **Flash-writer trap breadth (NB#2):** §5.3-3 / §11 now trap **all** `spi_flash`/`esp_partition`
  writes (incl. Bluedroid bond/CCCD), not just `saveParams()`.
- **`restMs == 0` (NB#3):** commit deferred to `idle` when no rest gap exists; never a mid-stroke
  commit fallback (§3.5).
- **Fallback reword (NB#4):** §5.3 fallback (a) is now "serialize all flash writers to the dark
  window"; dropped the incorrect "move render to IRAM" lever (FastLED WS2812 path isn't IRAM-safe).
- **Post-PASS hardening (v4-review NBs, folded in before adversarial review):** explicit
  **follower ingestion** rule (adopt cosmetic immediately, kinematic only at a boundary — the
  receive-side mirror of the merge fix); **preview/idle** apply-immediately rule (staging is
  running-mode only, preserving live reach preview); **cosmetic changes are event-beacon
  triggers** (§4.4, so the sub-100 ms figure holds); and cosmetic in-place writes **republish via
  the §3.5 mailbox** (no unsynchronized cross-core write). All in §7/§4.4.

## 19. v4 → v5 changelog (adversarial red-team resolutions)

The adversarial reviewer returned **PASS conditional on three items**; v5 resolves all three plus
the serious mitigable risks it raised, so the PASS is unconditional.

- **Premise stated (the review's biggest gap):** §1.0 now states the **load-bearing product
  requirement** (untethered, phone-independent at runtime, continuously drift-corrected) and §1.2
  explicitly rejects the **wired sync line** (dominates on sync but fails the no-tether
  requirement — the sole honest reason to accept the §5 coex risk) and the **phone-scheduled
  start** (fails phone-independence + continuous re-anchor). Both alternatives are noted to also
  eliminate the coex risk — an honest cost of choosing ESP-NOW.
- **Phase resequencing:** the **coex/RMT/memory spike is now Phase 0**, run *before* the renderer
  refactor as a throwaway sketch (§10/§15), so a no-go wastes nothing and never risks the shipped
  standalone product's appearance.
- **Render-side fallback lever:** §5.3 fallback (a) is now the **I2S/DMA WS2812 driver**
  (`FASTLED_ESP32_I2S`), which cures the *named* most-likely failure — RMT-refill ISR starvation
  under WiFi on the classic WROOM-32 (an RMT-vs-I2S bake-off is in the Phase-0 spike). §8 adds the
  RMT-starvation row.
- **Security:** ESP-NOW broadcast is unauthenticated; §4.2.1 adds a **pairing-derived keyed MAC
  (`authTag`)** over each beacon (replacing the bare CRC) + `seq`/epoch replay checks, closing
  spoof/hijack. §8 adds the spoof/replay row; §11 adds a spoof/replay test.
- **Degrade threshold:** §8's "lost sync" trip is retied to the **actual drift budget (~15–30 s,
  not 2 s)** so a normal mid-swing body-block never manufactures a false "lost sync."
- **Event-beacon robustness:** repeats raised to **4–5× over a few hundred ms** (§4.4) — a joint
  loss desyncs a *full rep*, so the count targets joint-loss probability, not single-drop.
- **Provisioning reality:** §6 accounts for the only physical control being **BOOT/GPIO0** (a
  strapping pin) — gestures are **runtime** GPIO0 holds (a power-on hold enters the bootloader);
  the held-boot-pin pairing idea is dropped in favor of runtime-GPIO0 + a BLE-command fallback.
- **Channel:** §5.1 no longer fixes all sets to the busiest default (1); the operating channel is
  chosen at pairing (least-busy scan) and stored with the group.

## 20. v5 → v5.1 changelog (adversarial re-review — fixing v5's own edits)

The re-review confirmed all three conditions substantively closed and the architecture affirmed,
but flagged two fresh inconsistencies introduced by v5's edits (a FAIL under the "no new
contradiction" rule) plus two test-phasing tightenings. All fixed:

- **§8 drift rate corrected:** v5 wrote "~1 µs/s (±20 ppm)", which is self-contradictory (±20 ppm =
  20 µs/s) and contradicted §4.3. Relative drift between two ±20 ppm clocks is **~20–40 µs/s**; §8
  now states that and derives `DEGRADE_S` (~15–30 s → ~0.6–1.2 ms &lt; 3 ms) from it, not from the
  wrong rate.
- **Pairing rendezvous restored:** v5 made the channel dynamic (§5.1) but left §6 pairing with no
  channel to meet on (circular: channel = hash(groupId), groupId minted *during* pairing). §5.1/§6
  now use a **fixed rendezvous channel `CH_PAIR`** for the handshake, then hop to the scanned
  operating channel `CH_OP`; the group key is agreed at pairing too.
- **Test phasing tightened:** the Phase-0 spike (dummy animation, no gated `saveParams()`) now
  tests only **involuntary** flash writers (Bluedroid bond/CCCD, PHY-calib); the `saveParams()`
  deferral + `restMs==0` assertion moves to Phase 1/4 where the code exists (§11).
- **Auth-key staging made explicit:** §10 Phase 2 notes the beacon `authTag` uses a **placeholder
  key** until pairing-derived keying lands in Phase 5, so Phases 2–4 are not yet spoof-resistant
  (fine for bench dev; the spoof/replay test correctly lives in Phase 6).
