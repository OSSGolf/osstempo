# OSSTempo — Multi-Stick Sync — iOS UX Design (v2.2)

**Status:** Design under review. **v2.1 PASSED L9 design review** (both original v1 blockers confirmed
substantively closed; the two v2 contradictions resolved). **v2.2 resolves the adversarial red-team's
four blockers:** the "Locked" proof was gated on an *unobservable* clock-settle (→ now an observed
per-follower `settled` flag, §13-ask-5, with an honest count-confirm fallback); `setDisbanding` could
show a false "ended" or hang (→ "ended" only on observed `F → 0`, else still-synced); an abandoned add
stranded a **zombie leader** with WiFi up (→ every abandon exit reverts the clock stick, §13-ask-6);
and the blind-state chip had no non-color-only compact form (→ a distinct slashed-antenna *shape*).
Plus three red-team nits. See §19.
**v1 failed L9 design review on two protocol-honesty blockers**
(both in the trust surface): a `1 of 2` amber "degrade" tier the app *cannot observe* (the leader
holds a coasting follower present until `T_HELLO ≥ T_ORPHAN > DEGRADE_S`, plan §6, so a symmetric
body-block would show green over a degraded stick), and a `btLostWhileSynced` state that asserted
"still in sync" while blind (the follower-status rides the dropped BLE link, and the claim is false
on a leader reset, plan §5.3-5). **v2 re-grounds the entire health model on what the app can
actually observe**: the leader's follower-count *and* per-follower last-heard-age over BLE — the
latter promoted to an explicit, non-tear-down protocol ask (§13). See §17 changelog.

**Companion to `TEMPO_SYNC_PLAN.md` (v6.2)** — that doc is the firmware/protocol LLD; this one is
the iOS app UX for the same feature. Every screen, word, and state here must be *implementable on
and honest about* the v6.2 protocol.
**Author role:** L9 editors-choice Swift/UX SWE.

**Design thesis:** Sync is a *capability the connection surface grows into*, not a new mode. A
golfer with one stick must see **nothing new**; a golfer with two sticks gets a calm, honest,
glanceable "these are locked together" — in the app's existing vocabulary (a stick, a strip, the
comet) with **zero protocol jargon** (never "leader," "follower," "ESP-NOW," "beacon," "session,"
"channel"). The hardest UX problem is not the happy path — it is **telling the truth, gently, using
only the signals the app can actually see**, when the wireless link between two physical objects
degrades, and never stranding the user in a dead end when it does.

**The v2 discipline (the rule v1 broke):** *the app may only assert what its one observation channel
— the leader's status characteristic (`…0012`) over the app↔leader BLE link — reports.* When that
channel is silent or blind, the UI says only what remains true, never what it hopes is true.

---

## 0. Scope, non-goals, success criteria

**In scope**
- The end-to-end iOS UX for forming, running, monitoring, and dissolving a synced **set** of 2–N
  sticks, layered onto the *already-shipped de-gated app* (config library home; non-blocking
  `ConnectionChip` → `ConnectionSheet`; `now-practicing` bar; `ControlView` editor).
- The **vocabulary** and **copy deck** — the most load-bearing decision, since the protocol's mental
  model (roles, clocks) must never leak.
- A **complete state & transition catalogue** for the sync surface, mapped 1:1 to *observable*
  protocol signals, every degraded/failure path landing on a **non-dead-end recovery**.
- **Status & trust**: how "locked together" reads at a glance, and how loss/degrade/dissolve are
  shown *honestly and only from observable signals* (no false green, no crying wolf, no claim while
  blind).
- **Identify** ("Blink to find") — the physical-to-digital binding for a roster of near-identical
  sticks.
- Motion, haptics (incl. **Reduce Motion**), and full **accessibility** (VoiceOver, Dynamic Type,
  no color-only state).
- Explicit **deferrals** and the **protocol asks** the UX depends on (§13), separated into
  *required* vs *fallback-if-absent*.

**Non-goals**
- No change to the firmware protocol beyond the additive asks in §13 (each flagged required/optional
  with the graceful fallback if absent). Where v2.6 can't meet a UX need, it's surfaced, not assumed.
- No per-stick naming/labeling in v1 (§14; Blink is the interim binding).
- No settings/persistence UI for sync (ephemeral is the product intent — plan §2.1).
- No visual redesign of the shipped library/editor; sync is **additive** and reuses `Theme`.

**Success criteria**
1. A one-stick user's app is **identical to today** — no new controls, labels, or chrome until they
   explicitly tap "Add a stick." (§3)
2. Across a room, a user can tell the **chip-glanceable** states apart without reading — **synced /
   forming / ended**, plus **`k of N` re-linking for N≥3** — and the glanceable signal is **never
   asserted beyond what the app observes**. A single stick re-linking in the common **N=2** set is
   *intentionally* not a chip alarm (the set is still locked sub-frame, plan §8) — it surfaces in the
   roster row, not the chip; honesty and calm outrank one more glanceable tier here. (§7)
3. Every failure path lands on a screen with a **clear next action**, never a spinner-forever or a
   dead end. (§8)
4. Adding a stick is a **≤ 3-tap**, self-explaining flow confirmed by the *truth* (follower-count),
   whose failure is indistinguishable-at-the-wire but **distinct in the UI**. (§5.2, §8)
5. Fully operable by VoiceOver at AX5 Dynamic Type, state conveyed by **text + shape**, never color
   alone. (§11)

---

## 1. The three user truths this UX is built on

Everything derives from three facts about the *physical* product:

1. **Sticks are near-identical and placed apart.** The UI cannot rely on the user knowing "which row
   is which stick" — hence **Identify / Blink to find (§6)** is the binding primitive, not a nicety.
2. **The link is wireless between two objects a body walks between.** Degrade is *normal* (a club or
   torso blocks the 2.4 GHz path mid-swing). A few seconds of coasting is **healthy** (plan §8: drift
   ≪ a frame for tens of seconds); real loss is only past `DEGRADE_S` (~15–30 s). Crying wolf destroys
   trust faster than a brief honest amber. **But — new in v2 — the app only shows amber when the
   leader's status actually reports a stale follower (§7), never on a threshold it can't see.**
3. **The set outlives the phone.** Once synced, the sticks run without the app (plan §1.0-b/§8). The
   app's sync UI is a *window onto* an independently-running set, not its controller-of-record.
   **Corollary the v2 trust rule enforces:** when the window (BLE to the leader) closes, the app must
   report only "I've lost my view," never "the set is fine" — it can't see the set to know. (§8
   `controlsReconnecting`, §9)

---

## 2. Vocabulary — the copy contract (zero jargon)

The protocol's nouns are banned. The mapping, fixed once and used everywhere:

| Protocol (plan) | UI word | Never say |
|---|---|---|
| a stick (any role) | **stick** (or "trainer" in first-run copy, matching today) | node, peripheral, device-under-role |
| leader | *(unnamed)* — "this stick" / the one you're holding | leader, master, host, source-of-truth |
| follower | *(unnamed)* — "the other stick(s)" | follower, slave, client |
| a formed set | **set** ("your set"), verb **sync** | session, group, mesh |
| follower-count = N | **"N sticks · synced"** | followers, peers |
| becomeLeader/joinSession | **"Add a stick"** | pair, provision, assign role |
| disband | **"Stop syncing"** | disband, teardown, release |
| identify probe | **"Blink to find"** / **"Blink this stick"** | identify, locate beacon |
| follower reported stale (age high) | **"Re-linking…"** (a stick) | degraded, orphan, coast |
| app↔leader BLE dropped | **"Reconnecting to your trainer"** (controls) | central disconnected |
| orphan-timeout / count→0 | **"Sync ended"** | orphaned, timed out |
| CH_OP / sessionId / key | *(never surfaced)* | channel, session id, key |

**Two distinct "reconnect" verbs (v1 nit fixed):** a *stick* losing the leader is **"Re-linking"**
(the sticks re-finding each other); the *phone* losing the leader is **"Reconnecting to your
trainer"** (the app re-finding the hardware). Never the same word for both — they're different links
with different meanings, and the existing shipped chip already uses "Connecting" for BLE
service-discovery (`.syncing`), a third meaning kept distinct.

**Rule:** roles are *never* named — they're ephemeral and symmetric to the user (the stick you hold
happens to be the clock; an implementation detail). The user thinks "I synced my two sticks." This
also future-proofs against the fluid-role model (plan §2.1): any stick can lead next time.

---

## 3. Information architecture — sync lives in the connection surface

The de-gated app has exactly one place meaning "the link between this app and the hardware":
**`ConnectionChip` → `ConnectionSheet`**. Sync extends the same idea ("what hardware am I driving?"),
so it lives there — not a new tab, not a new root.

### 3.1 The invariant: one stick sees nothing new

Sync UI appears **only after** the user has one stick connected and taps "Add a stick." Until then:
- The chip reads exactly as today (`Connect` / `Connecting` / `Connected`).
- The `ConnectionSheet` connected panel shows exactly today's content (PuttTempo · Disconnect ·
  Forget) **plus one new row: "Add a stick" (§5.1)**. A user who never taps it never sees a sync
  concept. (Criterion #1.)

### 3.2 Where each piece of sync lives

| Surface | Today | With a synced set |
|---|---|---|
| **`ConnectionChip`** (nav bar) | `Connected` (green dot) | **`2 synced`** — the glanceable set signal (§7). |
| **`ConnectionSheet`** connected panel | PuttTempo · Disconnect · Forget | Becomes a **stick roster** (§4): the connected stick + each synced stick as a row with a health dot, **Blink**, and per-set **Stop syncing**. "Add a stick" lives here. |
| **`now-practicing` bar** (home) | play/stop + name + tempo | Adds a small **`⚭ 2`** badge (link-glyph + count); tap opens the roster. |
| **`ControlView`** editor | tempo/cosmetic controls | **Unchanged controls** + a slim inline note "Edits apply to your whole set" (plan §7: kinematic at boundary, cosmetic immediate). No new controls. |
| **Config library / rows** | unchanged | **Unchanged** — sync is a property of the *link*, not a saved config. |

**Why not a "Sync" tab:** a tab implies a place you persistently go; sync is ephemeral and rare
(most sessions are one stick). Co-locating with the connection keeps the one-stick app clean and
answers "is my set together?" with the same glance that already answers "am I connected?"

---

## 4. The stick roster (the connected panel, grown up)

When a set exists, the `ConnectionSheet`'s `connected` panel becomes a **roster** — the home of sync:

```
┌─ Trainer ───────────────────────────── Done ┐
│  ✓ Trainer connected                         │   header (existing pattern)
│  Your edits stream to all sticks live.       │   subtitle (set-aware)
│                                              │
│  ●  This stick            [ Blink ]          │   the stick on BLE (green dot)
│  ●  Second stick          [ Blink ]          │   a synced stick (green / amber "Re-linking")
│         2 sticks · synced                    │   set summary
│                                              │
│  [ + Add a stick ]                           │   primary affordance
│  [ Stop syncing ]                            │   secondary (destructive-tinted, not red)
│  ─────────────────────────────────────────  │
│  Disconnect        Forget this trainer       │   existing controls (unchanged)
└──────────────────────────────────────────────┘
```

- **Row health dot — driven ONLY by the leader's reported per-follower status (§13-ask-1):** green =
  the leader is hearing this follower's hellos fresh; **amber (pulsing) + "Re-linking…"** = the
  leader's last-heard-age for this follower has crossed the app threshold `T_UI_STALE` (the stick may
  be blocked/coasting — honestly "we've lost contact with it," matching the follower's own physical
  degrade); **removed from the list** = the leader dropped it from the count (it orphaned). The dot is
  *never* set from a client-side timer the app can't corroborate — that was the v1 blocker.
- **"This stick"** holds the BLE link (the clock, unnamed) — its dot is just the BLE `connected`
  green; it has no follower-health concept (the app talks to it directly).
- **Blink** is on *every* row so the user can always answer "which physical stick is this row?"
- With **one stick**, the roster collapses to today's single connected panel + the lone "Add a stick"
  row (§3.1); it renders as a *list* only at count ≥ 2.

---

## 5. Flows

### 5.1 Discover the feature

No announcement, no tooltip, no badge for a one-stick user. Discovery is a single quiet row —
**"+ Add a stick"** — at the bottom of the connected panel, styled as a secondary affordance. It's
present the moment one stick is connected (the only precondition — you need one stick to be the
clock). Progressive disclosure over feature-tour.

### 5.2 Add a stick (the core flow)

Protocol reality (plan §12): the app already holds BLE to stick A (the clock), scans for candidate B,
connects B over BLE, mints a session, sends `becomeLeader` to A and `joinSession` to B; B then
**drops BLE by design**, and **success is confirmed only when A's follower-count increments** — the
BLE drop is *identical on success and failure*. The UX must **never treat the drop as done**.

**Steps (≤ 3 taps to start):**

1. **Tap "Add a stick."** → a scan sheet (reuses the existing `scanning` list UI + `SignalBars`,
   `deviceRow`). Copy: "Bring the other stick nearby and turn it on." **The scan list excludes the
   already-connected clock stick** (filter by its peripheral id) so you can't try to add the stick
   you're on (v1 nit fixed). Each candidate row has signal bars **and a Blink** (§6) so you can
   confirm the right physical stick before committing.
2. **Tap the stick to add.** → the app connects B over BLE, provisions the set, and shows a
   **bounded, self-completing progress** state: "Linking your sticks…" with the comet motif. The
   progress is tied to the **follower-count**, not the BLE drop (state `setForming`, §8).
3. **Confirmation, in two honest beats:**
   - **Linked** — when A's follower-count rises (normally ~1–1.5 s, plan §12): the roster shows
     `2 sticks · synced`, a **`.light` haptic** confirms the join, and the chip updates to `2 synced`.
     The user is done and can practice *now*. (The single celebratory `.success` is held for the
     *lock* beat below, so the payoff fires only once the sticks are actually in step, not before —
     see §10 haptics grammar.)
   - **Locked (the proof) — gated on an *observed* settle flag, never a client timer.** The signature
     "watch them move as one" one-shot comet fires only once the app *observes* the clock has
     settled. The follower's offset uses a 16-sample min-filter at 4 Hz (plan §4.3) that needs a few
     seconds to reach `<1 ms`; firing at the ~1.5 s count-confirm could show a *visibly skewed*
     "proof." But that convergence is **follower-internal state, and the follower is BLE-off** — the
     app cannot see it directly, and a client-side "~a few seconds" timer would be exactly the
     unobservable guess §7/§13-ask-1 ban. So the proof is gated on the **leader-relayed per-follower
     `settled` flag (§13-ask-5):** the follower sets a "locked" bit in its hello once its own
     min-filter has converged, the leader relays it on `…0012`, and the app fires the proof only when
     that observed flag is set **and** the set is idle (never mid-rep). Until then the roster reads
     `synced` with a subtle "getting perfectly in step…". **Fallback if the flag isn't delivered
     (§13-ask-5):** the app fires the proof on count-confirm and drops the "locked, not skewed"
     wording — the copy becomes "here they are, moving together" with **no** sub-frame promise, so it
     never asserts a lock it can't observe. Either way the app never claims "locked" from a timer.

**If it doesn't confirm** (count doesn't rise within the app window = follower `T_JOIN` **+ margin**,
plan §12 — not literally `T_JOIN`, so an edge-of-window join isn't falsely failed): → **`setFailed`**
(§8): "Couldn't link that stick. Make sure it's close and powered, then try again." with **[Try
again]** (re-scan) and **[Not now]**. The candidate has self-reverted to standalone and is
**immediately re-selectable** (plan §6) — Try again just works. No dead end.

**Critical cleanup — never strand a zombie leader.** The assign order sends `becomeLeader` to the
clock stick A *before* the candidate joins (plan §6), so **A raises WiFi and beacons immediately**. If
the add is then **abandoned** — `setForming` cancel, `setFailed` → **[Not now]**, or dismissing the
add sheet — A has *no follower and never will*, so the leader's own "last follower left / hellos time
out" WiFi-down trigger (plan §6) **never fires** and A would sit in the BLE+WiFi+LED brownout-prone
leader config (plan §5.3-5) indefinitely while the UI calls it "solo." So **every abandoned-add exit
sends `disband` to A**, returning it to standalone before landing on `setSolo`. Only **[Try again]**
keeps A as leader (it's about to add another candidate). A firmware backstop is also requested
(§13-ask-6: a leader with zero followers for > `T_JOIN` self-reverts to standalone) in case the app
dies before it can send `disband`.

**Why bounded progress, not a spinner:** the ~1–1.5 s typical confirm is short enough to render as a
single confident beat. The fill completes on the *event* (follower-count), holds near-full if the
event is slow ("almost there"), and fails at window end — never a faked timer percentage, never a
forever-spin.

### 5.3 Practice (synced)

Identical to today. The `now-practicing` bar's play/stop drives the whole set (plan §4.5). Additions:
- The **`⚭ 2` badge** on the bar (§3.2).
- A tempo edit lands **set-wide at the next rep boundary** (plan §7 kinematic); brightness/rails land
  **immediately, set-wide** (plan §7 cosmetic). The editor note "Edits apply to your whole set" states
  the *capability* (not a per-edit delivery claim — a re-linking/coasting follower may adopt a
  kinematic change a cycle late or once beacons return, plan §8); **no per-stick editing** (a synced
  set is one instrument — one authoritative param set, plan §7).
- **Runtime independence, stated as general truth (not a live claim):** the app teaches *once*, as a
  **secondary caption below the roster's set-summary line** (distinct from the header subtitle "Your
  edits stream to all sticks live." in §4) — "Your set stays in sync even if the phone steps away" —
  a property of the product (plan §1.0-b), shown only in the steady `setSynced` state. This is
  education, **not** a status assertion during a drop
  (see the v2 discipline: it's shown while the app can still see `synced`, so it's honest; the moment
  the app goes blind it stops asserting anything about the set — §8 `controlsReconnecting`).

### 5.4 Identify — "Blink to find" (mechanics in §6)

From any roster row (or an add-scan candidate): **Blink** flashes *that* stick's LEDs unmistakably
for ~2 s — how the user maps a row to a physical stick, and how they answer "which one is re-linking?"

### 5.5 Stop syncing (dissolve, deliberate — resolves on truth, not on send)

**"Stop syncing"** → confirm ("Stop syncing your 2 sticks? Each goes back to working on its own.") →
the app sends `disband` to the clock (plan §6: authenticated, repeated DISBAND). The UI enters
**`setDisbanding`** ("Ending sync…") and **resolves when the leader's follower-count reaches 0**
(observable), *not* on send — because DISBAND is best-effort and a follower that misses all repeats
coasts until `T_ORPHAN` (plan §6), so a fire-and-toast would claim "ended" while a stick still runs
(v1 nit fixed). On `F → 0`: roster → single-stick panel; chip → `Connected`; `.success` haptic
(the disband-complete confirmation, §10); brief "Sync ended" toast. **"Sync ended" is shown only on
an observed `F → 0` — never at `F > 0`.** The leader tears down within a few hundred ms of *hearing*
`disband` (drops WiFi → standalone → `F` reads 0, plan §6) whether or not a follower caught the
repeats — so `F` *failing* to reach 0 within a short bound means the leader **didn't hear the write**
(a BLE glitch at the tap), i.e. the set is *still fully alive*, not ending. On that timeout the app
**re-sends `disband`** (idempotent, plan §6); if the write keeps failing it routes to
`controlsReconnecting`/`setSynced`, **re-reads `F`**, and reports the set **still synced** — it never
flips to "Sync ended" over a live set (the earlier false-ended hedge is deleted), and it never hangs
in "Ending sync…" forever: the user lands on either a real teardown or an honest still-synced roster
with the button to try again. Because a synced follower had BLE off, **release must go through the
set** — the button is on the *set*, never per-row (the app can't reach a BLE-off follower; plan §6).

---

## 6. Identify / "Blink to find" — the physical binding primitive

**Problem:** N near-identical sticks are unusable without binding a row ↔ an object.
**Solution:** a **Blink** action on every roster row and every add-scan candidate.

- **Mechanics (a protocol ask — §13-ask-2):** Blink flashes a distinctive full-strip pattern (e.g. 3
  bright white pulses) for ~2 s. For **the connected/clock stick** it's a direct BLE command (trivial).
  For a **synced (BLE-off) follower**, the app can't reach it directly — it asks the clock to
  **address an identify to that follower's MAC over ESP-NOW**. *v6.2 defines no per-follower identify
  beacon.* This doc **requires** it (§13-ask-2). **Fallback if absent:** Blink is offered only on the
  clock stick and on add-scan candidates (which are still on BLE), and roster rows for BLE-off
  followers hide Blink rather than offer a dead button — honest degradation, and the common N=2 add
  flow (candidate on BLE) is unaffected.
- **UX:** tapping Blink gives a `.light` haptic; the row's label animates "Blinking…" for 2 s, then
  reverts. Fire-and-see, no modal. If the stick is currently **Re-linking** (amber), the identify may
  not arrive; the label shows "Blinking…" then **"No response"** — the same loss that ambered the dot,
  reported honestly.
- **During add:** a candidate's Blink confirms the *right* second stick before committing — critical
  when several OSSTempos are in a shop/range.

---

## 7. Status & trust — glanceable, and asserted only from observable signals

The chip and roster dots are the whole trust surface. **The v2 rule:** every signal is driven by the
leader's status characteristic over the live BLE link — the follower-count and the per-follower
last-heard-age (§13-ask-1). Nothing is asserted from a client-side timer or from hope.

**Rule A — glanceable in one look.** The chip distinguishes states without reading (count + color +
motion, never color alone):

| Set state | Chip | Dot | Roster | Driven by (observable) |
|---|---|---|---|---|
| Synced, healthy | **`2 synced`** green | steady | rows green, steady | all followers present (`F` full), ages fresh |
| Forming (adding) | **`Linking…`** amber | breathing pulse | new row grey→green on confirm | follower-count not yet incremented |
| A stick re-linking (N ≥ 3) | **`2 of 3`** amber | breathing pulse | the stale row amber "Re-linking…" | leader age(stick) > `T_UI_STALE`, count unchanged |
| A stick re-linking (N = 2) | **`2 synced`** green, roster row **amber "Re-linking…"** | row pulses | the one row amber | *see note* — chip stays green (set not lost); roster carries the nuance |
| Set ended | **`Connected`** green (back to single) | steady | roster collapses; "Sync ended" toast | follower-count `F → 0` |
| Controls lost (phone↔leader BLE down) | **`Reconnecting`** amber (full copy "Reconnecting to your trainer" spoken/in-sheet) | **slashed-antenna glyph** (distinct *shape*, §11), pulsing | *(roster unavailable — see §8)* | app↔leader BLE dropped |

**N=2 note (the honest resolution of the v1 blocker).** For the common two-stick set, the app's
*count* channel is binary: 2 (present) or the set is over. A single follower "re-linking" is surfaced
**in the roster row** (amber "Re-linking…", from the leader's per-follower age) while the **chip stays
`2 synced`** — because the set is *not lost*, the leader still holds it, and the sticks are still
locked sub-frame (plan §8 coast). We do **not** flip the chip to a scary count for a recoverable
re-link. If the age keeps growing and the leader drops the follower (orphan), *then* the set ends and
the chip returns to single-stick `Connected` with a "Sync ended" toast. This is honest at every step:
green while the leader holds the set; amber row while the leader reports one stick stale; ended only
when the leader actually drops it. **No false green over a *lost* stick** — because "lost" is exactly
when the leader stops reporting it, and the UI follows the leader.

**Rule B — never assert beyond the observable, in either direction.**
- **No false green.** The chip shows `N synced` only while the leader reports all followers present
  (`F` full). A follower the
  leader can't hear grows a stale *age* first (roster amber "Re-linking"), and is dropped from the
  *count* when it orphans (set ends) — the app tracks the leader, which is the only node that knows.
- **No crying wolf.** A normal mid-swing body-block that recovers before `T_UI_STALE` shows
  **nothing** (the leader's age never crossed the threshold; the set was genuinely fine). Amber
  "Re-linking" appears only once the leader's reported age is actually high — a real, observed stale,
  not a guess. `T_UI_STALE` is set below `T_HELLO`/`T_ORPHAN` but above a normal block, so it matches
  the follower's own `DEGRADE_S` physical state without tearing the set down (the leader *reports* the
  age without *dropping* the follower — plan §6 keeps `T_HELLO ≥ T_ORPHAN`; reporting ≠ tear-down).
- **Acknowledged residual edge (honest, bounded).** A *sustained unidirectional* RF asymmetry — the
  leader keeps hearing the follower's 1 Hz hellos while the follower misses all 4 Hz beacons for
  > `DEGRADE_S` — would leave the chip green while that follower's own LEDs physically amber-degrade.
  It is physically improbable (if the sparse hellos get through, the denser beacons largely do too),
  and the app is defensibly tracking *leader authority* (the only node with a set-wide view). We
  accept it as a rare, bounded corner rather than invent a second, client-side health estimate that
  would reintroduce the v1 unobservable-threshold problem. **The mirror corner is also acknowledged:**
  the leader's per-follower age comes from the follower→leader **hellos**, but "Re-linking…" is copy
  about the follower missing the leader→follower **beacons** — so a follower rendering in perfect sync
  (beacons fine) whose *hellos* are dropping would show a **false amber "Re-linking."** Because amber
  is the soft, self-recovering, no-haptic state (not an alarm), a brief false amber costs far less
  trust than a false green or a false "ended"; it clears itself the moment a hello lands. We accept it
  rather than gate the display on both directions (which the app can't see anyway).
- **Never claim while blind.** When the app↔leader BLE link drops, the status characteristic is
  unreachable — the app can see *nothing* about the set. It therefore asserts **only** the phone-link
  truth ("Reconnecting to your trainer"), and makes **no** claim about set health until BLE reconnects
  and it re-reads the count (§8 `controlsReconnecting` → `synced | setEnded`). This is the direct fix
  for the v1 "still in sync while blind" blocker.

**Severity grammar (coherent, learnable):** *green* = the leader holds the set; *amber* = "working on
it / re-linking / reconnecting" (recoverable, observed); *red* is **reserved for nothing routine** —
a truly ended set is shown as the calm "Sync ended" return-to-single, not an alarm, because it is not
an error, it's ephemerality doing its job. (Red is used only for the existing hard BLE errors, e.g.
incompatible device — unchanged from today.)

---

## 8. Complete state & transition catalogue

New UI states, prefixed **`set*` / `controls*`** to avoid colliding with the shipped
`ConnectionState.syncing` (which is BLE service-discovery — implementation note in §15). Each has a
next action (no dead ends), and each is driven by an **observable** signal.

> **Count convention (read before wiring the triggers).** The only observable wire value is the
> leader's **follower-count `F`** (the `…0012` characteristic, plan §12). The **displayed set size is
> `N = F + 1`** — the connected clock stick plus `F` followers. So a healthy two-stick set is `F = 1`
> / `N = 2`, and disband/dissolve completes at `F = 0`. Every "Entered when (observable)" trigger
> below is stated in **`F`** so it can be wired straight to `…0012`; the chip/roster *display* `N`.

| UI state | Entered when (observable) | Shows | Next action(s) |
|---|---|---|---|
| **`setSolo`** (today) | 1 stick connected, no set | today's connected panel + "Add a stick" | add / disconnect / practice |
| **`setScanForAdd`** | tapped "Add a stick" | scan list (clock stick filtered out) + Blink per candidate | tap a stick / cancel |
| **`setForming`** | candidate tapped; `becomeLeader`+`joinSession` sent | "Linking your sticks…" bounded progress | *(auto)* wait for follower-count; **cancel → `disband` A → `setSolo`** |
| **`setSynced`** | follower-count incremented (plan §12) | roster (§4); chip `N synced`; proof fires on the observed `settled` flag (§5.2, §10, §13-ask-5) | practice / add / blink / stop syncing |
| **`setReLinking`** | leader reports a follower's last-heard-age > `T_UI_STALE` (§13-ask-1), count unchanged | roster row amber "Re-linking…"; chip `k of N` (N≥3) or stays green (N=2, §7) | wait (auto-recovers) / Blink to check / stop syncing |
| **`setFailed`** | follower-count didn't rise within `T_JOIN`+margin | "Couldn't link that stick." | **Try again** (keeps A leader, re-scan) / **Not now → `disband` A → `setSolo`** |
| **`setEnded`** | follower-count **`F → 0`** — no viable set left (last follower orphaned / leader reboot / all left) | "Sync ended." toast; roster → `setSolo` | practice solo / add a stick again |
| **`setSynced` (shrunk)** | **`F` dropped but `F ≥ 1` remains** (N≥3, one follower orphaned) | roster drops that row; chip `k synced` (`k = F+1`); brief "One stick dropped out" toast | practice / add / blink / stop syncing |
| **`setDisbanding`** | tapped "Stop syncing" → `disband` sent | "Ending sync…"; resolves on observed **`F → 0`**, not on send (§5.5) | *(auto)* → `setSolo` on `F → 0`; **on short-bound timeout with `F > 0` → re-send; on write failure → `setSynced` (still linked), never "ended", never hang** |
| **`controlsReconnecting`** | app↔leader BLE dropped (set status now unobservable) | chip "Reconnecting to your trainer"; **no set-health claim** | wait / walk closer; on reconnect → reconcile |

**Key transitions & their honesty guarantees:**
- **`setForming → setFailed`** uses the follower-count, not the BLE drop (plan §12). The failed
  candidate is re-selectable immediately (plan §6): `setFailed → setScanForAdd → setSynced` is a clean
  retry, never a dead end.
- **`setSynced → setReLinking → setSynced`** is the *normal* body-block cycle, driven by the leader's
  reported age (observed), not a client timer. It feels like weather: **no** haptic on entering
  re-link (don't punish a normal event); a soft `.light` haptic on **recovery** (a quiet "it's back").
  For N=2 the chip stays green (§7); for N≥3 the chip reads `k of N`.
- **`setReLinking → setEnded`** when the leader finally drops the follower (its age passed
  `T_ORPHAN`/`T_HELLO`, plan §6). Honest escalation: amber "Re-linking" (leader still holds it) →
  "Sync ended" (leader dropped it). **Reboot honesty (plan §6/§8/§23):** after a *leader* reboot the
  other sticks are **BLE-off and un-selectable until they orphan-timeout (~`T_ORPHAN`)**, *then*
  re-adding is one tap. So `setEnded`'s "Add a stick" scan **may not immediately show the just-lost
  stick** — the UI says so: "The other stick will be ready to add again in a few seconds," never an
  empty scan the user reads as broken. (But note: a *leader* reboot drops the app↔leader BLE link too,
  so the app first passes through `controlsReconnecting` — see next.)
- **`controlsReconnecting → {setSynced | setEnded}` (the v1 Blocker-2 fix).** Any app↔leader BLE drop
  — phone walked away *or* leader brownout/reset (plan §5.3-5), **indistinguishable at the app** —
  enters `controlsReconnecting`. The app asserts **nothing** about the set (it's blind). The existing
  reconnect ladder re-establishes BLE; on reconnect the app **reads the follower-count `F`** and
  reconciles: `F ≥ 1` (followers still there) → `setSynced` (the phone had merely stepped away; set
  was fine); `F = 0` / leader booted standalone → `setEnded` (the leader had reset and the set dissolved). The truth is
  told *when the app can see it again*, never guessed during the blind window. **If the shipped
  3-attempt reconnect ladder exhausts** (the leader is truly dead/off), the app falls through to the
  existing disconnected UI — a non-dead-end the user already knows (Reconnect / Scan); the set is
  reported neither synced nor ended until a link is re-established and the count is read. General
  phone-independence is taught elsewhere as a product property (§5.3), not asserted here as live
  status.

**Coverage check vs plan §8 failure table:** single-beacon loss / sustained coast → invisible
(healthy, no UI); lost-sync `DEGRADE_S` → `setReLinking` (observed via leader age); leader reboot →
`controlsReconnecting → setEnded`; never-heard-first-beacon (`T_JOIN`) → `setFailed`; orphan
(`T_ORPHAN`) → `setReLinking → setEnded`; phone leaves → `controlsReconnecting` (reconciles to
`setSynced`); two sets in a room → not a UI concern (protocol drops foreign beacons); forged/spoofed
DISBAND/beacon → protocol-dropped, no UI event. Every row has a mapped, observable UI state.

---

## 9. Runtime independence & app-lifecycle behavior

The set outlives the app (plan §8):
- **Backgrounding** while synced does nothing to the set. On foreground the app re-reads the leader's
  follower-count (existing read + notify, plan §12) and restores the correct chip/roster — it never
  assumes the set is gone because it wasn't watching.
- **Force-quit / relaunch** while a set runs: the app relaunches to the library, reconnects to the
  last stick (`reconnectLast`), reads the follower-count, and rediscovers the live set → `2 synced`.
  The set was never the app's to hold. (Requires the count read to be authoritative on reconnect — it
  is, plan §12.)
- **The honest caveat:** if the app is quit *and the clock stick is power-cycled*, the set dissolves
  (ephemeral roles, plan §2.1) — on relaunch the app shows `setSolo`, correctly. No phantom set.
- **Consistency with §8:** every relaunch/foreground path resolves set truth by **reading the count**,
  the same reconcile as `controlsReconnecting`. There is exactly one way the app learns set health —
  the leader's status over BLE — and every entry point uses it.

---

## 10. Motion & haptics

Motion only where it *communicates*:
- **Snap-to-sync proof (§5.2):** on the **observed `settled` flag** (§13-ask-5, not a client timer
  and not mere count-confirm), both sticks fire one identical comet + a roster "lock" animation (the
  two dots slide together / a link-glyph draws) + `.success` haptic. One-shot, ≤ 600 ms, gated to
  idle (never mid-rep). The emotional payoff — the user *sees* and *feels* two objects become one
  instrument, genuinely locked. **Fallback (no settle flag, §13-ask-5):** fires on count-confirm with
  the softened "moving together" copy and no sub-frame claim.
- **Forming progress:** the `CometMark` motif animates during `setForming` — on-brand, reads as
  "working," not "hung" (it's the same comet the product is about).
- **Re-link / reconnect pulse:** the existing `ConnectionChip` breathing dot (`isActive` pulse,
  already in code) is reused for amber transient states — **no new motion vocabulary**.
- **Haptics grammar:** `.light` on join-confirm (the "Linked" beat, §5.2), on Blink tap, and on
  re-link→recovery; the single `.success` is reserved for **set-locked** (the observed-`settled`
  "Locked" beat, §13-ask-5, *not* mere count-confirm) and for disband-complete (at `F → 0`);
  **nothing** on entering
  re-link or `controlsReconnecting` (don't alarm on normal/uncertain events). One `.success` per
  formed set, at lock — so the celebratory payoff never fires before the sticks are in step. **In the
  §13-ask-5 *fallback* mode (no `settled` flag)** the count-confirm proof **does** carry the single
  `.success` — it is the only proof beat available, so the "not at count-confirm" rule is scoped to
  the flag-present mode; there is still exactly one `.success` per formed set. Haptics confirm, never
  alarm.

**Reduce Motion (required):** the proof becomes a **crossfade** (dots fade to synced, link-glyph fades
in) with the same `.success` haptic — meaning survives without motion. Forming becomes a static
`CometMark` + a determinate bar. The breathing dot becomes a **static amber dot + the word**
("Linking", "Re-linking", "Reconnecting to trainer") — the count/word already carries the state, so
removing the pulse loses nothing semantic. **No state is ever motion-only.**

---

## 11. Accessibility

- **No color-only state (criterion #5):** every state pairs color with a **word and/or shape** —
  chip counts are text (`2 synced`, `2 of 3`, `Reconnecting to trainer`); roster rows carry a status
  word ("Synced" / "Re-linking…" / "Blinking…"); dots differ in **fill** (solid / pulsing / hollow)
  as well as hue. Full color-blindness: reads counts and words unaided.
- **VoiceOver:**
  - Chip: `accessibilityLabel("Set: 2 sticks synced")` / `"Set: one stick re-linking"` /
    `"Reconnecting to your trainer"` — the observed truth, spoken. Hint: "Opens set and connection."
  - Roster rows: label "`Second stick, synced`" / "`Second stick, re-linking`"; **row actions** via
    `accessibilityAction` — "Blink to find", "Stop syncing" — so no action is trapped behind a swipe
    or visual-only control (mirrors the shipped `ConfigRow` pattern).
  - Async outcomes as **announcements** (`AccessibilityNotification.Announcement`): on `setSynced`
    "Two sticks synced," on `setFailed` "Couldn't link that stick," on `setEnded` "Sync ended," on
    reconnect-reconcile the resolved state — so a VoiceOver user gets outcomes without watching. The
    proof flourish is `accessibilityHidden` (decorative); the announcement carries its meaning.
- **Dynamic Type — the chip specified, not hand-waved (v1 nit fixed):** the chip is a nav-bar toolbar
  item that `.fixedSize()`s (shipped `ConnectionChip.swift:56,63`), so a long status ("2 of 3 ·
  re-linking") **cannot** grow freely at AX sizes without colliding with the title/Done button. Rule:
  the chip shows the **compact form** — dot + count only (`2`, or `2/3`) + link-glyph — and the
  **status word lives in the roster and the VoiceOver label**, not the chip, at accessibility sizes.
  At standard sizes the chip may include the short word ("synced"); at AX sizes it truncates the word
  first, never the count/dot (the count is the load-bearing glyph). Tapping always opens the roster
  where the full-width, Dynamic-Type-safe rows carry the words.
  - **The blind state (`controlsReconnecting`) has no count — so it carries a distinct *shape*, not
    just a color (fixes the color-only-dot trap).** Its full copy "Reconnecting to your trainer" (28
    chars) can't fit a `.fixedSize()` toolbar item even at standard sizes, and dropping the word per
    the rule above would leave a bare **amber dot** indistinguishable *by shape* from the green synced
    dot — a color-only distinction that fails criterion #5 at AX5. So the blind state replaces the dot
    with a **slashed-antenna glyph** (`antenna.radiowaves.left.and.right.slash`, the idiom already
    shipped in `ConnectionSheet.swift:239`) + the short word "**Reconnecting**" at standard sizes,
    truncating to the **glyph alone** at AX sizes. The glyph differs from the synced dot in *shape*,
    so the two are separable with zero color perception; the full "Reconnecting to your trainer" lives
    in the VoiceOver label and, on tap, the sheet.
- **Dynamic Type — roster rows:** use `ViewThatFits` so at AX sizes the per-row Blink moves from a
  trailing button to a stacked full-width control (never clipped), matching the app's existing
  discipline.
- **Reduce Transparency:** the roster's `.ultraThinMaterial` (shared with the now-practicing bar)
  falls back to `Theme.card` — consistent with the shipped app.

---

## 12. Copy deck (exact strings)

| Context | String |
|---|---|
| Add affordance | **Add a stick** |
| Scan header | **Bring the other stick nearby and turn it on.** |
| Forming | **Linking your sticks…** |
| Getting locked (post-count, pre-settle) | **Getting perfectly in step…** |
| Synced summary | **{N} sticks · synced** |
| Chip synced | **{N} synced** (standard) · **{N}** (AX compact) |
| Chip re-linking (N≥3) | **{k} of {N}** |
| Roster row re-linking | **Re-linking…** |
| Editor note | **Edits apply to your whole set** |
| Runtime-independence (steady-state education) | **Your set stays in sync even if the phone steps away.** |
| Controls lost (BLE drop) | **Reconnecting to your trainer** |
| Sync failed | **Couldn't link that stick. Make sure it's close and powered, then try again.** |
| Sync failed actions | **Try again** · **Not now** |
| Sync ended (reboot/orphan) | **Sync ended — each stick is on its own again.** |
| Reboot re-add hint | **The other stick will be ready to add again in a few seconds.** |
| Stop syncing (button) | **Stop syncing** |
| Stop syncing (confirm) | **Stop syncing your {N} sticks? Each goes back to working on its own.** |
| Stop syncing (write not confirmed) | **Couldn't end sync — your sticks are still linked. Tap Stop syncing again.** |
| Blink action | **Blink** → **Blinking…** → (fail) **No response** |

**Voice:** plain, second-person, physical ("stick," "close," "on its own"), never mechanical. Errors
name the *fix*, not the fault. Numbers are digits (glanceable). **No string asserts set health the
app can't see** — "Reconnecting to your trainer" is the only copy shown during a BLE drop, and it
speaks only of the phone link.

---

## 13. Protocol asks (separated: required vs graceful-fallback)

Honesty about the seams — UX requirements on the firmware, reconciled with the plan:

1. **Per-follower last-heard-age in `…0012` (REQUIRED for the amber "Re-linking" tier).** The status
   characteristic must expose, per follower, a **freshness/last-heard-age** the leader *reports
   without dropping* the follower (reporting ≠ the `T_HELLO` tear-down; plan §6 keeps `T_HELLO ≥
   T_ORPHAN`). This is what lets the app show an honest, *observed* "Re-linking…" that matches the
   follower's own `DEGRADE_S` physical state. **Fallback if absent:** the app has only the binary
   count — the roster/chip stay `N synced` green through the coast and flip straight to "Sync ended"
   when the count drops. Still fully honest (green = leader holds the set; ended = leader dropped it),
   just without the intermediate amber nuance. *The whole degrade tier degrades gracefully to binary
   — nothing lies either way.* (This is the v1-blocker-1 correction: v1 assumed a client-observable
   `DEGRADE_S`; there isn't one — it must come from the leader or the tier collapses to binary.)
2. **Per-follower addressed identify (REQUIRED for follower Blink, §6).** An addressed identify from
   the leader to a specific follower MAC over ESP-NOW (a new addressed beacon or a flag). **Fallback
   if absent:** Blink is offered only on the clock stick and on add-scan candidates (both on BLE);
   roster rows for BLE-off followers hide Blink. The common N=2 add flow is unaffected.
3. **Follower-count read authoritative on reconnect (REQUIRED — already implied by plan §12).** Every
   reconcile (§8 `controlsReconnecting`, §9 relaunch/foreground) reads the count to learn set truth;
   the read must reflect current reality within the app's reconnect window. Confirm `…0012` read
   returns live count, not a cached/stale value.
4. **Join-confirm window constant (REQUIRED — value only).** The app's `setForming → setFailed`
   timeout = follower `T_JOIN` **+ margin** (plan §12). Exact `T_JOIN` is bench-tuned (plan §14); the
   UX needs it surfaced as one app constant so app and firmware agree.
5. **Per-follower `settled`/clock-locked flag on `…0012` (REQUIRED for the "Locked" proof, §5.2/§10).**
   The follower's offset min-filter convergence (plan §4.3, the `<1 ms` figure) is **follower-internal
   and the follower is BLE-off**, so the app cannot observe it — gating the proof-comet on a client
   "~a few seconds" timer would be exactly the unobservable guess §7/§13-ask-1 ban. Ask: the follower
   sets a **"locked" bit in its hello** once its min-filter has converged, and the leader **relays a
   per-follower `settled` flag** on `…0012` (the leader participates in the §4.3 round-trip, so this is
   cheap and legitimate). The app fires the proof only on that *observed* flag. **Fallback if absent:**
   the proof fires on count-confirm with **no** "locked, not skewed" claim — copy becomes "here they
   are, moving together" (§5.2) — so the app never asserts a lock it can't see; it simply loses the
   sub-frame guarantee on the first fire.
6. **Leader zero-follower self-revert backstop (RECOMMENDED — belt-and-suspenders for §5.2).** The app
   sends `disband` to the clock stick on every abandoned add (§5.2), but if the *app itself dies*
   between `becomeLeader` and that cleanup, the leader would sit WiFi-up with no follower and no
   WiFi-down trigger (plan §6 fires only on last-follower-leave / hello-timeout, neither of which
   happens when no follower ever joined). Ask: a leader with **zero followers for > `T_JOIN`
   self-reverts to standalone** (drops WiFi), independent of the app. **Fallback if absent:** the app's
   `disband`-on-abandon covers the common case; the residual risk is a zombie leader only across an
   app crash mid-add — recoverable by the user power-cycling the stick, but the backstop removes even
   that. (Also directly serves plan §5.3-5's leader-brownout concern.)

Items 1, 2, and 5 limit a v1 *feature* if undelivered (the amber tier, follower Blink, the
locked-proof) — each degrades gracefully to an honest, smaller UX rather than a broken or lying one.
Item 6 is a robustness backstop the app-level `disband` already mostly covers.

---

## 14. Explicit deferrals (and why)

- **Naming/renaming individual sticks.** Deferred: ephemeral roles + typical N=2 make "This stick /
  Second stick" + **Blink** sufficient; names need persistence the product avoids (plan §2.1).
  Revisit at N > 3.
- **Multi-select "add several at once."** Deferred: the protocol adds followers one BLE connection at
  a time (plan §12); a batch UI would mask per-stick failures. Add sequentially in v1.
- **Persistent "my usual set" / auto-rejoin UI.** Deferred to match the plan's off-by-default
  persisted auto-rejoin (plan §14); if bench data flips that default (leaders reset in the field), a
  "reconnect my set automatically" toggle lands here.
- **Per-stick tuning.** Never (a design stance, not a deferral): a synced set is one instrument (one
  authoritative param set, plan §7).
- **Signal-strength / RF diagnostics in the roster.** Deferred to `DiagnosticsView`: the roster shows
  *health as meaning* (synced / re-linking), not dBm; RF numbers belong in diagnostics.

---

## 15. Implementation note — state naming (avoid the `.syncing` collision)

The shipped `ConnectionState` (AppState.swift:16) **already has a `.syncing` case** — it is the BLE
GATT service-discovery phase, rendered as "Connecting" (`ConnectionChip.swift:23,32`). This feature is
*named* "sync," so all new UI states are prefixed **`set*`** (`setSolo`, `setForming`, `setSynced`,
`setReLinking`, `setFailed`, `setEnded`, `setDisbanding`) and **`controls*`** (`controlsReconnecting`)
to prevent a name collision with the existing enum when the §8 machine is layered on. The user-facing
words never collide (the internal `.syncing` shows "Connecting," never "syncing").

**A second state axis is required, not a reuse of the existing enum.** The shipped `ConnectionState`
(AppState.swift:11–17) is a *single* machine where `.scanning` **replaces** `.connected` — you cannot
be `.connected` to A and `.scanning` for B in it. So `setScanForAdd`/`setForming` (§5.2 "reuses the
existing `scanning` list UI") must run on a **parallel `set*` state variable** held alongside the
live `.connected`, plus a **second `CBPeripheral`** (the shipped model has one `lastConnectedID` and
one connection; the add flow needs a concurrent candidate connection — CoreBluetooth supports it,
plan §12). "Reuse" refers to the scan *list view*, not the state machine driving it; the sync surface
is a distinct axis composed with the existing connection state, not a new case inside it.

---

## 16. Traceability — every claim to a protocol section

| UX decision | Plan basis |
|---|---|
| Join confirmed by follower-count, not BLE drop | §12, §10-P5, §23 |
| Amber "Re-linking" only from the leader's *reported* stale age, not a client timer | §6 (`T_HELLO ≥ T_ORPHAN`), §8; §13-ask-1 |
| Body-block coast is healthy → invisible unless the leader reports stale | §8, §4.3 (drift ~20–40 µs/s) |
| N=2 chip stays green during a re-link; ends only on count-drop | §6, §8 (leader holds set until orphan) |
| Never assert set health while app↔leader BLE is down | §5.3-5 (leader reset ≈ phone-away at the app), §1.0-b |
| `setEnded` reboot → wait `T_ORPHAN` before re-add | §6, §8, §23 |
| Release must go through the set (no per-follower BLE) | §6 (followers BLE-off) |
| Set outlives phone; reconcile by reading the count | §1.0-b, §8, §12 |
| Proof-comet gated on an *observed* `settled` flag (not a client timer); honest fallback if absent | §4.3 (min-filter is follower-internal); §13-ask-5 |
| Abandoned add reverts the clock stick (no zombie leader) | §6 (leader WiFi-down triggers), §5.3-5; §13-ask-6 |
| Disband shows "ended" only on observed `F → 0`; else still-synced, never hang | §6 (leader tears down on hearing disband) |
| Kinematic set-wide at boundary / cosmetic immediate | §7 |
| Two-threshold `T_JOIN` (fast fail) vs `T_ORPHAN` (full coast) | §6, §8, §22 |
| Follower Blink needs an addressed identify | §13-ask-2 (gap vs §4.2 beacon set) |
| No jargon; roles unnamed | §2.1 (roles ephemeral/symmetric) |

---

## 17. v1 → v2 changelog (L9-review resolutions)

The L9 review returned **FAIL on two protocol-honesty blockers** (both in the trust surface) plus six
strong nits. All resolved:

- **Blocker 1 — unobservable degrade tier (§7/§8).** v1 triggered `1 of 2` amber on "hello aged >
  `DEGRADE_S`," but the leader holds a coasting follower present until `T_HELLO ≥ T_ORPHAN >
  DEGRADE_S` (plan §6) — so a symmetric body-block would show green over a degraded stick (false
  green). v2 re-grounds the degrade tier on the **leader's *reported* per-follower last-heard-age**
  (a report, not a tear-down — §13-ask-1), makes **N=2 chip-green-through-coast, end-on-count-drop**
  (binary is honest and cleaner), and gives a graceful **binary fallback** if the firmware can't
  report age. The app never claims a threshold it can't see.
- **Blocker 2 — claiming set health while blind (§5.3/§7/§8/§12).** v1's `btLostWhileSynced` said
  "your set is still in sync," but the follower-status rides the dropped BLE link (app is blind), and
  the claim is false on a leader reset (plan §5.3-5). v2's **`controlsReconnecting`** asserts **only**
  the phone-link truth ("Reconnecting to your trainer") and **reconciles on reconnect by reading the
  count** → `setSynced` or `setEnded`. Runtime-independence is taught as a product property in
  steady state (while the app can see `synced`), never asserted during a drop.
- **Nit — proof fires before clock settles (§5.2/§10).** The snap-to-sync comet now waits for
  **clock-settle** (16-sample min-filter, plan §4.3), not the ~1.5 s count-confirm, so the proof is
  actually locked, not skewed; a two-beat "Linked → Locked" copy covers the gap.
- **Nit — add-scan didn't exclude the clock stick (§5.2).** The candidate list now filters out the
  already-connected stick.
- **Nit — three "Reconnecting" meanings (§2/§8).** Distinct verbs: sticks re-finding each other =
  **"Re-linking"**; phone re-finding the hardware = **"Reconnecting to your trainer"**; BLE
  service-discovery keeps "Connecting."
- **Nit — Dynamic Type chip hand-waved (§11).** Specified: the toolbar chip shows the compact
  dot+count at AX sizes (word drops first, count/dot never), with the status word carried by the
  roster and the VoiceOver label — no free growth in a `.fixedSize()` toolbar item.
- **Nit — disband resolved on send (§5.5).** `setDisbanding` now resolves on the leader's
  **count→0**, not on send. *(v2.2 note: the interim "hedged toast" this bullet originally described
  was removed — see §19 Blocker 2; "Sync ended" now shows only on observed `F → 0`.)*
- **Nit — `.syncing` state-name collision (§15).** New states prefixed `set*` / `controls*` to avoid
  colliding with the shipped `ConnectionState.syncing` (BLE service-discovery).

## 18. v2 → v2.1 changelog (L9 re-review resolutions)

The re-review confirmed **both original blockers substantively and correctly closed** (degrade tier
now observable via leader-reported age; blind-window discipline airtight) and all six nits addressed
— but flagged **two fresh contradictions v2's rewrite introduced** (a FAIL under the no-new-
contradiction rule) plus four consistency notes. All resolved:

- **New-1 — criterion #2 vs the §7 N=2 design (§0).** Criterion #2 promised "synced / forming /
  re-linking / ended" tellable *without reading*, but §7 deliberately keeps N=2 re-linking chip-green
  (roster-only) so as not to cry wolf while the set is still locked. v2.1 rewords criterion #2 to
  list only the **chip-glanceable** states (synced / forming / ended, plus `k of N` re-linking for
  N≥3) and states that N=2 re-linking is intentionally roster-surfaced — the gate now matches the
  honest §7 design.
- **New-2 — double-placed `.success` haptic (§5.2/§10).** The `.success` was specified both at
  count-confirm ("Linked") and clock-settle ("Locked"). v2.1 puts a **`.light`** at Linked and
  reserves the single **`.success` for Locked** (clock-settle), matching §10's proof-gating intent —
  the celebration never fires before the sticks are in step.
- **Nit — N≥3 partial loss (§8).** Added a **`setSynced` (shrunk)** transition: for N≥3, one follower
  orphaning drops to a smaller *healthy* set (`k synced`), not "Sync ended"; `setEnded` now triggers
  only when the count falls **below 2**.
- **Nit — reconnect-ladder exhaustion (§8).** `controlsReconnecting` now states the fall-through to
  the shipped disconnected UI if the 3-attempt ladder exhausts — a known non-dead-end; the set is
  reported neither synced nor ended until a link is re-established and the count is read.
- **Nit — roster subtitle collision (§4/§5.3).** The runtime-independence line is now a **secondary
  caption** below the set-summary, distinct from the header subtitle "Your edits stream to all sticks
  live." — two different strings, no longer both "the subtitle."
- **Nit — unidirectional-asymmetry edge (§7).** Added an explicit acknowledgement of the rare,
  bounded corner where the leader hears hellos but the follower misses beacons (chip green while the
  stick physically degrades) — accepted as a bounded corner rather than reintroduce a client-side
  health estimate.
- **Post-PASS hardening — count vs follower-count convention (§8/§7/§5.5).** The re-review PASSed but
  flagged an implementation footgun: §8's triggers were phrased in total-stick `N` while the
  observable `…0012` value is the **follower-count `F = N − 1`**, so "count below 2" could be
  mis-wired to trip `setEnded` on a healthy two-stick set. Defined **`N = F + 1`** once (§8 preamble)
  and restated every observable trigger in `F` (`setEnded` at `F = 0`; `setSynced` (shrunk) while
  `F ≥ 1`; reconcile on `F`); the chip/roster still *display* `N`. Also aligned §5.5's disband haptic
  to the §10 `.success` grammar.

## 19. v2.1 → v2.2 changelog (adversarial red-team resolutions)

The red-team returned **FAIL on four blockers** (plus three non-blocking notes), affirming the two
original protocol-honesty blockers stayed closed. All four resolved, so the design asserts nothing it
can't observe:

- **Blocker 1 — proof gated on an unobservable state (§5.2/§10/§13/§16).** v2.1's "Locked" proof
  waited for "clock-settle," but the follower's min-filter convergence is **follower-internal and the
  follower is BLE-off** — so it could only be a client timer, the exact unobservable guess the doc
  bans; a poor-RF join would fire a *visibly skewed* "proof." v2.2 gates the proof on a **leader-
  relayed per-follower `settled` flag (§13-ask-5)** — the follower sets a "locked" bit in its hello
  once converged — with an **honest fallback:** fire on count-confirm with no "locked, not skewed"
  claim ("here they are, moving together"). Never a lock-claim from a timer.
- **Blocker 2 — `setDisbanding` false "ended" / hang (§5.5/§8/§12).** v2.1 hedged to "Sync ended" if
  `F` didn't reach 0 in a short bound — but the leader tears down within a few hundred ms of *hearing*
  `disband`, so `F > 0` means the write was **lost** and the set is *still alive*; showing "ended" was
  a false-ended, and the honest reading hung forever. v2.2 shows "Sync ended" **only on observed
  `F → 0`**; on timeout it re-sends `disband`, and on write failure routes to
  `controlsReconnecting`/`setSynced` reporting **still synced** — no false-ended, no hang. Copy-deck
  hedge string replaced.
- **Blocker 3 — zombie leader on abandoned add (§5.2/§8/§13).** `becomeLeader` raises the clock
  stick's WiFi *before* the candidate joins; on cancel/**Not now**/dismiss nothing reverted it, and
  with no follower ever joining the leader's WiFi-down trigger never fires — it sat in the brownout-
  prone leader config (plan §5.3-5) while the UI said "solo." v2.2 makes **every abandoned-add exit
  send `disband` to the clock stick** before `setSolo` (only **Try again** keeps it leader), and adds
  **§13-ask-6** (a leader self-reverts if it has zero followers past `T_JOIN`) as an app-crash backstop.
- **Blocker 4 — blind-state chip color-only at AX (§7/§11/§12).** `controlsReconnecting` has no count
  to fall back to; dropping the word left a bare **amber dot** distinguishable from the green synced
  dot **by color alone** (fails criterion #5 at AX5). v2.2 gives the blind state a distinct **shape** —
  a slashed-antenna glyph (the shipped `antenna.radiowaves.left.and.right.slash` idiom) — separable
  with zero color perception, plus a specified standard-size abbreviation ("Reconnecting").
- **Nit — false-amber from hello-path asymmetry (§7).** Acknowledged the mirror of the false-green
  corner: a follower rendering fine but whose *hellos* drop shows a brief false "Re-linking" — accepted
  because amber is the soft, self-clearing, no-alarm state (costs far less trust than a false green).
- **Nit — "Editing all N sticks" over-claim (§3.2/§5.3/§12).** Softened to the *capability* statement
  "Edits apply to your whole set" (a re-linking follower may adopt a kinematic change a cycle late).
- **Nit — parallel state axis, not enum reuse (§15).** Made explicit that `setScanForAdd`/`setForming`
  run on a **parallel `set*` state variable + a second `CBPeripheral`** alongside the live
  `.connected` — the shipped single-connection enum can't be `.connected` and `.scanning` at once;
  "reuse" is of the scan *list view*, not the state machine.

## 20. Open UX questions (for review)

1. N≥3 degrade chip: `k of N` (count-of-healthy) vs `N · re-linking` (count-of-set + word)? Leaning
   `k of N` for glanceability, roster carrying the nuance — flagged.
2. Proof-comet on confirm: worth auto-firing the strip, or does an involuntary animation surprise a
   user mid-setup? Leaning yes (it's the proof-of-sync), gated to idle + the observed `settled` flag.
3. `setEnded` after a leader reboot: auto-attempt re-add once the stick reappears, or always require
   the explicit tap? Leaning explicit (matches ephemeral intent; auto-rejoin is the §14 deferral),
   with a possible "we found it — re-add?" middle ground.
