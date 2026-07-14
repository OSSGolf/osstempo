# Phase 1 — Analytic stateless renderer + execution model — Implementation Plan

**Status:** Design/prep, written while Phase 0 (the coex spike) is at the bench gate. **This is a plan,
not product code** — no `tempo_trainer/` source is modified until Phase 0 returns a GO *and* you
approve starting Phase 1 (the goal's binding "wait for explicit approval before the next" rule). It
translates `TEMPO_SYNC_PLAN.md` §3/§9/§10-Phase1/§11 into a concrete, file-level implementation +
verification plan grounded in the current firmware, so Phase 1 can start the instant it's unblocked.

**Phase 1 is independently valuable and low-regret:** the analytic stateless renderer is a cleaner
architecture (non-blocking, testable, no hidden temporal state) even if wireless sync were later
descoped — so this plan is not wasted by a Phase 0 no-go, unlike the ESP-NOW phases.

**Gate to exit Phase 1** (plan §10, adjusted for the no-A/B directive — see §7):
1. `positionAt(u,p)` **golden parity** vs the current segment math (zero drift).
2. **Objective still-frame diff** of the head + rails (exact) between old and new builds — replaces the
   plan's *human* A/B (dropped per your no-A/B directive); the analytic trail shape is validated by the
   head/rails diff + code review, not a subjective bless. *(Flagged divergence from plan §11 — to
   reconcile into TEMPO_SYNC_PLAN.md through the gates, not silently.)*
3. Single-stick **render-tick jitter < 1 ms** with radios idle (scope + the on-device serial jitter
   readout reused from the Phase 0 spike).
4. Both builds compile `--warnings all`; standalone behavior **behaviorally** identical to today.

---

## 1. What changes and why (the current renderer is stateful + blocking)

Confirmed in the current firmware (`tempo_trainer/tempo_trainer.ino`):
- **Blocking loops:** `runStroke()`/`runStrokeFull()` drive the animation with
  `while (millis()-t0 < ms && control != 0) { drawComet(...); delay(4); }` (`:437–469`, `:396–421`).
  The main loop is *occupied* for the whole stroke — incompatible with a grid pacer and a radio task.
- **Stateful trails:** `drawComet`/`drawCometRefs` do `fadeToBlackBy(leds,…,90)` then add
  (`:337,:350`) — an IIR of past frames; and `addComet` latches direction from `static float
  prevPos`/`static int dir` (`:316–321`). Two nodes at the same cycle-time render *different* trails.
- **Snapshot-at-stroke-start:** `Profile p = profiles[mode]` at `:425/:475` — the phase-continuity
  behavior the new epoch model must preserve.

Phase 1 replaces this with a **pure `renderFrameAt(u, p, control)`** driven by a **grid-paced render
task**, per plan §3.2–§3.5. No wireless yet — a single stick runs the new renderer off its own
`esp_timer` grid (plan §9 "standalone parity").

---

## 2. File layout (Arduino sketch reality)

The plan's `render/ grid/ sync/…` are *conceptual* modules; the Arduino sketch compiles every
`.ino/.cpp/.h` in the sketch folder. Phase 1 adds (co-located in `tempo_trainer/`):

```
tempo_trainer/
  ├─ tempo_trainer.ino     (existing — the blocking runStroke*/sweep/hold retired; loop() rewired)
  ├─ render.h / render.cpp NEW  positionAt(u,p) [ported] + renderFrameAt(u,p,control) [analytic, stateless]
  ├─ grid.h   / grid.cpp   NEW  esp_timer periodic grid → xTaskNotifyGive; render task on core 1
  └─ params.h              NEW  Params/KinematicParams structs (shared now; the wire form lands Phase 2)
```

**≤5 files this phase** (satisfies the phased-execution rule): `render.h`, `render.cpp`, `grid.h`,
`grid.cpp`, and the `tempo_trainer.ino` edits (params.h is a small header folded into render.h if the
count is tight). **Arduino prototype caveat (CLAUDE.md):** types used in signatures (`Params`,
`EaseFn`, `RefDot`) must be declared before first use — put them in `render.h`/`params.h` included at
the top of the `.ino`, not mid-file.

---

## 3. `positionAt(u, p)` — port the segment math unchanged (the golden-parity core)

A pure function: cycle-time `u ∈ [0, cycleDuration)` → head position (float LED offset). It ports the
*existing* per-segment math verbatim so the golden test passes:

| Segment | Current source | Ported form (function of segment-local phase `ph`) |
|---|---|---|
| get-ready | `countdownPulses` (still, pulsing) | position = center; brightness pulse is analytic (below) |
| backswing (putt) | `amp*sinf(ph*PI/2)` `:439` | `center + amp*sin(ph*π/2)` |
| downswing (putt) | `amp*(1 - dropEase(ph, rel))` `:449` | `center + amp*(1 - dropEase(ph, rel))` |
| follow (putt) | `-amp*easeOut(ph)` `:458` | `center - amp*easeOut(ph)` |
| hold | fixed `finishOff` `:464` | constant |
| full-swing | distance-split `dOut/dIn`, `bOut/bIn/dBack/dDown` `:508–513` | same split, evaluated at `u` |

`u` is mapped to a segment by walking the cumulative segment durations (`cycleDuration` §4). **No
`static`, no `millis()`, no `delay`.** Golden test (§7) samples `positionAt` across `u` and compares to
the current loop's emitted positions — must be bit-identical (same float ops, same order).

---

## 4. `cycleDuration(p, control)` + the epoch model (single-stick form)

- `cycleDuration = getReadyMs + backswingMs + downswingMs + followMs + holdMs (+ restMs for loop)` —
  the sum the blocking loop implicitly walked. Single (2) omits `restMs`.
- **Epoch** = start of the current cycle, in `esp_timer_get_time()` µs. Single-stick Phase 1: `epoch`
  is local (no shared clock yet). At each boundary `epoch += cycleDuration` (loop) or `epoch = now`
  (single trigger), then `u = now - epoch`. This is the *same* per-cycle re-arm the shared model uses
  (plan §4.5), so Phase 2 slots the shared clock in with no renderer change.

---

## 5. `renderFrameAt(u, p, control)` — analytic, stateless (plan §3.2)

```c
void renderFrameAt(uint32_t u, const Params& p, uint8_t control);  // clears + repaints from u; no state
```
- **Position:** `positionAt(u, p)` (§3).
- **Direction:** from **segment identity** (get-ready still; backswing +; downswing −; follow −; hold
  still) — not `static` history, not a finite difference (deterministic at `u=0`/turnarounds; plan
  §3.2 resolves the v2-review NB#4).
- **Trail:** an analytic brightness ramp trailing the head opposite `dir` — a closed-form function of
  `u`, replacing the IIR `fadeToBlackBy`. Shape is a Phase-1 tunable; validated by the head/rails diff
  (§7), since head + rails are *exact* and the trail is cosmetic.
- **Rails / aim2:** drawn from `u` (`aim2At(u,p)` per the `aim2When` boundary crossed by `u`), not a
  global side-effect (`aim2Now`).
- **Get-ready pulse:** the `countdownPulses` brightness envelope re-expressed as `f(u)` over the
  get-ready segment.

**Determinism:** identical `(u,p,control)` ⇒ identical LED buffer — the property Phase 2's sync needs.

---

## 6. Execution model — grid pacer + render task (plan §3.5)

- **`grid.cpp`:** a periodic `esp_timer` at `FRAME_US = 8000` whose callback (esp_timer TASK context)
  does only `xTaskNotifyGive(renderTaskHandle)` — never `FastLED.show()` in the callback.
- **Render task:** `xTaskCreatePinnedToCore(renderTask, …, /*core*/1)` (APP_CPU). Body:
  `ulTaskNotifyTake` → compute `u` from `epoch` → `advanceCycleAtBoundary` → `renderFrameAt` (incl.
  `FastLED.show()`). This is exactly the loop the Phase 0 spike already prototypes and measures
  (`renderTask` there), so the jitter method is proven before Phase 1.
- **`loop()` rewire:** the blocking `runStroke()`/`runStrokeFull()`/`sweep`/`holdAt` calls are
  **retired**; `loop()` (or a control task) only services BLE writes and sets `control`/`epoch`. The
  render is entirely on the grid task.
- **NVS commit gating:** `saveParams()` runs only while `control == idle` or in the dark `rest` gap —
  never mid-stroke (plan §3.5). Single-stick Phase 1 already honors this (no leader/follower yet).
- **Standalone parity:** no WiFi/ESP-NOW touched — the renderer runs off the local grid; behavior is
  *behaviorally* identical to today (criterion #5), with the new analytic trail (validated, not
  byte-identical — that would defeat statelessness, plan §17).

---

## 7. Verification (this phase) — no hardware needed for #1/#2

1. **Golden kinematics parity (host-compilable):** extract `positionAt` (pure C, no Arduino deps) into
   a tiny host harness `test/golden/dump_kinematics.c` that dumps `positionAt(u,p)` across `u` for a
   set of profiles, and compare to a reference captured from the *current* loop math. **Zero drift.**
   Runs on the dev machine (`cc dump_kinematics.c && ./a.out`) — fully agent-doable, no bench.
2. **Objective head/rails still-frame diff:** render N frames old-build vs new-build (positions +
   rail indices are exact integers) and diff them programmatically. Replaces human A/B (dropped). The
   analytic trail is reviewed for visual equivalence, not blessed subjectively.
3. **Render-tick jitter < 1 ms (radios idle):** reuse the Phase 0 spike's on-device serial jitter
   readout + scope on the render-tick GPIO, now driving the *real* `renderFrameAt`. **Bench step**, but
   trivial (one board, no radio) — staged with the same procedure as Phase 0 §3.
4. **Compile gate:** both boards, `--warnings all`, after every edit.

Items 1–2 are automatable in CI-like fashion on the dev machine; only #3 wants a board (and it's a
single-stick, no-radio measurement — the easiest bench step in the whole project).

---

## 8. Phase-1 work breakdown (each ≤5 files, verify, then pause for approval)

- **1a. Extract `positionAt` + `cycleDuration` + golden harness** (`render.h/.cpp`, `test/golden/…`).
  Verify: golden parity green; compiles. *(No behavior change yet — the old loop still drives pixels.)*
- **1b. `renderFrameAt` + grid/render task; retire the blocking loops; rewire `loop()`**
  (`render.cpp`, `grid.h/.cpp`, `.ino`). Verify: head/rails diff; jitter < 1 ms; both builds clean;
  manual standalone smoke (putt + full + preview + reach-drag) behaviorally identical.

Pause for approval between 1a and 1b (phased-execution rule).

---

## 9. Open decisions to settle at Phase-1 start

- Exact analytic **trail shape** (brightness ramp length/curve) — pick to match the current IIR look
  via the head/rails-plus-trail review (plan §14 "remaining").
- Whether `Params` in Phase 1 is the full struct or a thin `Profile`+globals view (the wire form is
  Phase 2; keep Phase 1's struct a superset so Phase 2 only adds serialization).
- `FRAME_US` final value (8000 default; tightenable toward ~6 ms if the loop budget allows — gated by
  the Phase 0 jitter result).

---

## 10. What stays blocked until the bench GO

Everything wireless (Phases 2–5: ESP-NOW, SyncBeacon, clock offset, sessions, roles, the new
`…0012/…0013` characteristics, the UX protocol asks) — and, per the goal's literal phasing, the iOS
app (plan Phase 6). Those are gated on Phase 0 *and* the earlier phases. This plan deliberately scopes
only the **hardware-risk-free, independently-valuable** renderer refactor so there's a ready, approved
starting point the moment Phase 0 returns a GO.
