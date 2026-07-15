# Cross-build sync — dig2go ↔ generic ESP32

**Requirement (user hardware):** one QuinLED-Dig2go (`BOARD_DIG2GO`) + one generic ESP32
(`BOARD_DIY_ESP32`) must sync as a set — either can be leader or follower — at both the firmware and
iOS levels. This doc records *why* that holds and what guards it, and is the artifact for the L9 review.

## The core claim

The two builds differ **only** in three board `#define`s used **exclusively in `setup()`**:
`LED_PIN` (16 vs 5), `LED_POWER_PIN` (12, dig2go only) and `LED_POWER_ON`. Everything the sync feature
touches — the wire format, the clock, the renderer, the role state machine — is **board-independent
source compiled identically**. So a dig2go and a generic ESP32 running their respective builds produce
byte-identical beacons and, given the same shared cycle-time `u`, byte-identical LED buffers.

## Firmware — why it's cross-build-correct

1. **Identical wire format.** `SyncBeacon` (and the hello/ping/pong/identify packets) have **no
   board-conditional fields** (`sync_firmware.h`). Both builds compile the same packed struct with the
   same embedded `Profile`, so `sizeof` and byte layout match. Guarded by
   `static_assert(sizeof(SyncBeacon) <= 250, …)` (also the ESP-NOW payload limit). The HMAC `authTag`
   is computed over the same bytes with the same app-minted `sessionKey`, so a dig2go verifies a
   generic's beacon and vice-versa.
2. **Identical rendered frame.** `positionAt` / `renderFrameAt` / `gridQuantize` use `NUM_LEDS = 144`,
   `CENTER = 71.5`, `LED_TYPE = WS2812B`, `COLOR_ORDER = GRB` — all board-independent, identical on
   both. The head LED index for a given `u` is therefore identical, so both 1 m strips show the comet
   at the same LED at the same shared instant. `LED_PIN` only selects the output pin in
   `FastLED.addLeds()` (setup) — it does not enter the frame math.
3. **Role symmetry.** Roles are ephemeral RAM state assigned by the app; both builds carry the *full*
   sync module (leader + follower + standalone). So dig2go-leader/generic-follower and the reverse are
   both valid, and either reverts to a lone stick unchanged.
4. **dig2go strip powered in every role.** The dig2go LED-power relay (`LED_POWER_PIN`) is asserted in
   `setup()` regardless of role, so a dig2go **follower** lights its strip (it renders via the same
   `FastLED.show()` path as a leader).
5. **Same operating channel.** The app hands `CH_OP = 6` in both `becomeLeader` and `joinSession`;
   both builds `esp_wifi_set_channel(6)`. No board-specific radio config.
6. **Independent ±20 ppm clocks — the whole point of the sync layer.** The dig2go and generic crystals
   drift independently; the min-filter + round-trip OWD offset + shared epoch grid correct exactly this
   (plan §4.3). Nothing here assumes matched hardware clocks.
7. **Clean revert cross-build.** A follower's RAM `profiles`/globals are *mirrored from the leader*
   while synced; on disband/orphan `syncRevertToStandalone()` now `loadParams()`-restores the follower's
   **own** saved config — so a dig2go that followed a generic leader returns to its own settings, not
   the generic's.

## iOS — why it's cross-build-correct

The app is board-agnostic: it speaks the GATT, which both builds present identically.
1. **Both boards advertise** as `PuttTempo` on service `6e40…dcca9e`; the scan filters by service UUID,
   so both appear.
2. **Add flow is symmetric.** The user connects to *either* board (it becomes the leader on `add`); the
   add-scan lists the *other* board (the connected one is filtered out by peripheral id) and connects it
   as the transient candidate. `becomeLeader`→connected, `joinSession`→candidate — both boards handle
   the `…0013` opcodes identically.
3. **`…0012` follower-status is board-independent** (`[count][mac:6][flags:1]`); the app pads the 6-byte
   MAC into a stable `PeripheralID` regardless of which board reported it.
4. So the roster, join-by-count confirmation, re-link, disband, identify and reconcile flows all work
   whichever physical board is leader vs follower.

## Verification status

- **Both builds compile clean** (`--warnings all`, zero project warnings): dig2go + generic ESP32.
- **iOS suite green** (145 tests) over the mock transport (board-agnostic by construction).
- **Bench-gated (needs the two real sticks):** the *runtime* two-stick skew, coex on each board as
  leader, and the visual "both comets locked" check — staged in `spikes/phase0_coex/README.md` and the
  §11 skew method. Compile + protocol-level correctness is verified; the physical two-board run is yours.

## Follower BLE behavior (§2.1) and re-add
While synced, a follower is **BLE-quiet**: the transient add-flow candidate link is dropped by the app,
and the follower does **not** re-advertise (`ServerCB::onDisconnect` suppresses re-advertising while
`syncRole == ROLE_FOLLOWER`). Its BLE stack stays *initialized* (not a full deinit — lighter and
lets it re-advertise instantly), so its coex load is lower than the leader's active BLE+WiFi but not
literally zero. On disband/orphan, `syncRevertToStandalone` re-`loadParams()`s the follower's own
config and **re-starts advertising**, so a reverted stick is immediately re-addable (without this it
would stay undiscoverable until a reboot).

## The one honest asymmetry
The **leader** carries active BLE (the phone link) + WiFi simultaneously (higher coex/power stress);
the follower carries WiFi + an idle BLE stack. Either board *can* lead, but if one proves marginal as
leader on the bench (Phase 0 §5.3-5 brownout/coex), make the sturdier board the leader — a bench
decision, not a code constraint.
