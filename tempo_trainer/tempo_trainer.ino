/*
 * Tempo Trainer  -  ESP32 (classic WROOM-32, 30-pin) + WS2812B
 *
 * Two modes, selected over BLE:
 *
 *  PUTT  - middle-origin "comet" sweeps out from center on the backswing,
 *          decelerates to a stop at the amber target, then accelerates back
 *          through center (impact) and out the other side for follow-through.
 *
 *  FULL  - models a full swing's arc projected onto the 1D strip. The clubhead
 *          reaches its max width (red, at the strip end) on the way back, then
 *          comes up/in to the top of backswing (amber). On the downswing it
 *          retraces back out to red, then drops through center (impact) and
 *          follows through to the far red. Green = ball/impact, amber = top of
 *          backswing, red = max width (strip ends). No follow-side amber.
 *
 *          Path per rep:
 *            center -> backRed -> backAmber   (backswing: out, then up to top)
 *            [top pause]
 *            backAmber -> backRed -> center   (downswing: back out, then to impact)
 *            center -> followRed              (follow-through to the far end)
 *
 * Control is over BLE (no audio). Pair with a phone / nRF Connect / Web BLE.
 *
 * Libraries (install via Library Manager):
 *   - FastLED
 *   (BLE + Preferences come with the ESP32 Arduino core - no extra install)
 *
 * Wiring (set the board in the "Board select" block below):
 *   DIY ESP32:  GPIO5 -> 470ohm -> strip DIN; 5V pin -> strip 5V; GND -> strip GND;
 *               1000uF cap across 5V/GND at the strip input.
 *   Dig2go:     strip DIN -> LED-data terminal (GPIO16); strip 5V/GND -> the board's LED
 *               power terminals. Level shifter, data resistor and power relay are onboard.
 */

#include <FastLED.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <esp_timer.h>          // 64-bit µs clock for the analytic, stateless renderer (plan §4.1)
#include <WiFi.h>               // multi-stick sync radios (ESP-NOW); OFF until a set is formed (§2.1)
#include <esp_wifi.h>
#include <esp_now.h>

// ---------- Board select ----------
// One sketch, two builds. Pick ONE board below (or pass -DBOARD_DIG2GO / -DBOARD_DIY_ESP32
// as a compile flag). NOTE: the single-file copies under docs/ are DEPRECATED SNAPSHOTS -- they
// predate the sync module and the flip, and are not kept in step with this sketch.
#if !defined(BOARD_DIG2GO) && !defined(BOARD_DIY_ESP32)
  #define BOARD_DIG2GO           // QuinLED-Dig2go pre-assembled (the board this repo runs on now)
  //#define BOARD_DIY_ESP32      // generic ESP32 dev board + 470ohm + ceramic bulk (see pcb_build_spec.md)
#endif

// ---------- LED config (per board) ----------
#if defined(BOARD_DIG2GO)
  #define LED_PIN       16       // Dig2go level-shifted LED-data terminal (GPIO5 is the Dig2go's IR pin)
  #define LED_POWER_PIN 12       // Dig2go LED-power relay enable: the board CUTS 5V to the strip
  #define LED_POWER_ON  HIGH     // until this is asserted. Active-high; flip to LOW if the strip stays dark.
#elif defined(BOARD_DIY_ESP32)
  #define LED_PIN        5       // GPIO5 -> 470ohm -> strip DIN. No power relay: strip 5V is always live.
#endif
#define NUM_LEDS    144                // 144 LEDs/m on a 1 m strip (~0.69 cm/LED)
#define BRIGHTNESS_DEFAULT 40          // 0-255 (lowered per user feedback; was 120, too bright)
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

// ---------- Frame buffers: LOGICAL composition, PHYSICAL output (the 180 flip) ----------
// The renderer composes every frame in LOGICAL index space into leds[] (0 = the low end of the
// stroke picture). showFrame() is the ONLY thing that hands a frame to the strip, and it is where
// the 180 flip happens: with `flipped` set it copies leds[] out reversed, i -> NUM_LEDS-1-i.
//
// Reflection about index (NUM_LEDS-1)/2 IS reflection about CENTER (71.5), so the whole picture --
// comet head and tail, green ball, amber top-of-backswing, both red extents, the purple aim dot,
// the get-ready pulse, the idle dot -- mirrors together and stays mutually consistent. Because the
// mirror is applied to the FINISHED buffer, no LED element can escape it, including elements added
// later: that totality is the whole point of doing it here instead of inverting the stroke math
// (which is golden-parity-pinned in firmware_tests/ and re-implemented in the iOS app).
//
// Two buffers rather than reversing leds[] in place around show(): FastLED's show() returns while
// the strip is still being clocked out, and its internal yield can deschedule this task for
// milliseconds. An in-place mirror would leave the shared buffer transiently reversed across that
// window, where any other writer would strand it. 432 bytes buys that whole hazard class away, and
// leds[] is then never transiently wrong -- so fadeToBlackBy()'s idle IIR, which reads the buffer
// back across frames, keeps working in logical space with no changes.
CRGB leds[NUM_LEDS];                      // LOGICAL frame -- everything in the sketch writes here
CRGB physLeds[NUM_LEDS];                  // PHYSICAL frame handed to FastLED (bound in setup)

// 0 = normal, 1 = strip mirrored end-for-end. Global, persisted, and deliberately DEVICE-LOCAL:
// orientation describes how THIS stick is lying on the ground, not how a stroke is shaped, so it is
// never carried in the sync beacon and a leader can never rotate a follower's strip (see §1.4 /
// sync_firmware.ino). Written from the BLE task (core 0), read by the renderer (core 1): a single
// byte, atomic on Xtensa, feeding no multi-word state -- so `volatile` + a local snapshot is the
// same discipline brightness/railsLit/control already use, and needs no mutex.
volatile uint8_t flipped = 0;

// (clearLeds() / showFrame() are defined just below the RefDot declaration: the Arduino IDE injects
// its auto-generated prototypes ahead of the FIRST function definition in the sketch, so defining a
// function above `Profile`/`RefDot` would push those prototypes above the types they reference.)

// Center / impact index (between 71 and 72 for a 144-LED strip)
const float   CENTER = (NUM_LEDS - 1) / 2.0f;
// Max reach in LEDs from center (71 for a 144-LED strip) = the red / strip-end extent
const uint8_t HALF   = (NUM_LEDS - 1) / 2;

// ---------- Modes & per-mode profiles ----------
enum Mode : uint8_t { MODE_PUTT = 0, MODE_FULL = 1 };

// Each mode keeps its own reach + tempo (settable over BLE, persisted to flash).
struct Profile {
  uint16_t backswingMs;   // center -> top of backswing
  uint16_t downswingMs;   // top -> IMPACT (the tempo number; ratio = backswing:downswing)
  uint8_t  reachLeds;     // amber (top-of-backswing) reach in LEDs from center, 1..HALF
  uint16_t topHoldMs;     // pause at the top of backswing (full-swing transition; 0 for putt)
  uint8_t  release;       // 0 = free pendulum .. 100 = late lag release (drop-to-impact shape)
  uint16_t followMs;      // impact -> finish (follow-through duration, separate from tempo)
  uint8_t  aim2Enabled;   // FULL only: show the purple weight-shift dot (0/1)
  uint8_t  aim2Led;       // FULL only: absolute LED index of the purple dot (1..NUM_LEDS-2)
  uint8_t  aim2When;      // FULL only: 0 address / 1 top (default) / 2 downswing / 3 impact
};                        // NOTE: append new fields at the END so old NVS blobs still load.

Profile profiles[2] = {
  /* PUTT */ { 700, 350, 58, 0,   0,  350,  0, 70, 1 },   // 2:1 to impact, pendular, 350ms follow
  /* FULL */ { 900, 300, 60, 150, 55, 300,  0, 70, 1 },   // 3:1 to impact, lag, 300ms follow; aim2 off, 2 LEDs follow-side, at top
};

#include "sync_firmware.h"   // multi-stick sync interface (needs Profile, declared just above)

volatile uint8_t mode    = MODE_PUTT;   // selected mode (persisted)
volatile uint8_t control = 1;           // 0 idle, 1 loop, 2 single, 3 preview (runtime only, not saved)

// ---------- Global look / timing (shared across modes, persisted) ----------
volatile uint8_t  brightness = BRIGHTNESS_DEFAULT;
volatile uint16_t getReadyMs = 1000;             // get-ready countdown length; pulses = round(ms/1000)
volatile uint16_t holdMs     = 600;              // hold at the finish
volatile uint16_t restMs     = 3000;             // dark rest gap between reps
volatile uint8_t  railsLit   = 1;                // keep impact/backswing/extent dots lit during the stroke

// Comet look
const uint8_t TAIL = 6;                           // fading tail length in LEDs
const CRGB    DOT_COLOR    = CRGB(255, 255, 255); // the moving comet (white)
const CRGB    BALL_COLOR   = CRGB(0, 120, 0);     // center / ball / impact (green)
const CRGB    TARGET_COLOR = CRGB(255, 90, 0);    // top-of-backswing aim point (amber)
const CRGB    RED_COLOR    = CRGB(200, 0, 0);     // max-width extent / strip ends (red, full mode)
const CRGB    AIM2_COLOR   = CRGB(150, 70, 255);  // purple secondary aim (full-swing weight-shift dot)

// Invalidate the render frame-skip so the next tick repaints. Anything that paints the strip
// outside the render path (the identify blink) must call this, or the skip logic still believes
// its last frame is on the strip and leaves whatever overwrote it in place. Defined next to
// `lastFrameU`, below.
void invalidateFrameSkip();

// Type used in a function signature, declared up here so the Arduino IDE's auto-generated
// prototypes (inserted near the top of the sketch) can see it before first use.
struct RefDot { int idx; CRGB col; };             // a static reference dot (preview / get-ready dot map)

// Clear the LOGICAL frame. Replaces FastLED.clear(), which clears the array bound to the
// controller -- that is physLeds[] now, which the renderer must never touch.
static inline void clearLeds() { fill_solid(leds, NUM_LEDS, CRGB::Black); }

// Latch the composed frame to the strip. THE single output choke point: FastLED.show() must appear
// exactly once in this firmware, right here, or a render path would bypass the flip and paint
// unmirrored on a flipped stick. firmware_tests/check_show_chokepoint.sh enforces that mechanically.
// (An explicit element copy rather than memcpy: FastLED overloads memcpy, making a CRGB[] copy
// ambiguous. 144 three-byte assignments are free next to the frame's own composition cost.)
static inline void showFrame() {
  const uint8_t f = flipped;              // snapshot once: core 0 can change it mid-call
  const int last = NUM_LEDS - 1;
  if (f) { for (int i = 0; i < NUM_LEDS; i++) physLeds[last - i] = leds[i]; }
  else   { for (int i = 0; i < NUM_LEDS; i++) physLeds[i]        = leds[i]; }
  FastLED.show();
}

// ---------- Persistence (NVS) ----------
Preferences       prefs;
volatile bool     paramsDirty = false;           // set on BLE write; debounced-saved in loop()
volatile uint32_t lastWriteMs = 0;

// Orientation gets its OWN dirty flag and its own one-key commit, separate from the debounced
// saveParams(). That save is gated to control == 0 so a flash cache-stall can never land mid-stroke
// -- but this stick has no power button: the normal end of a session is unplugging it, often while
// still looping. Orientation is the one setting a user changes and then walks away from, so riding
// the idle gate would lose it in the dominant flow. Instead it commits at the DARK REST GAP as well
// as in idle (loop()): the strip is already blanked there once per rep, so a stall is invisible, and
// the worst-case loss window collapses from unbounded to a single rep.
//
// Unlike saveParams() this is NOT gated on `syncRole != ROLE_FOLLOWER`. That rule exists because a
// follower's profiles[] hold the LEADER's params and must never reach its own flash; orientation is
// the one setting a follower still owns, so committing it is always correct.
volatile bool     flipDirty = false;
static void saveFlipNow() {
  prefs.begin("tempo", false);
  prefs.putUChar("flip", flipped);               // putX skips a no-op write, so this is cheap
  prefs.end();
  flipDirty = false;
}

// Orientation feedback window. In idle the strip shows a single dim LED at the centre, and in the
// dark rest gap it shows nothing at all -- so a flip in those states moves one pixel by 6.9 mm, or
// nothing, and the user taps again thinking the toggle is broken. For a short window after a flip
// write, those two states paint the mode's static dot map instead: the amber top-of-backswing dot
// jumping to the other side of the green ball is an immediate, unambiguous answer to "which way
// does the stick run now?". While a stroke is actually rendering, the moving comet already answers,
// so the window is only consulted in the two blind states.
static const uint32_t FLIP_CONFIRM_MS = 1200;
// Stored as the moment the window was ARMED, not as a deadline, and tested by elapsed time. Unsigned
// subtraction wraps correctly, so this is valid at any uptime with nothing to retire — which also
// means the reader never writes, so a flip landing on core 0 can never be clobbered by core 1
// deciding the previous window had expired. Seeded already-expired so a fresh boot shows nothing.
volatile uint32_t flipConfirmArmedMs = (uint32_t)0 - FLIP_CONFIRM_MS;
static inline bool flipConfirmActive() {
  return (uint32_t)(millis() - flipConfirmArmedMs) < FLIP_CONFIRM_MS;
}
// The confirm frame-skip tag, extracted so loop() itself never reads `flipped` — that keeps the
// "no composition site reads the flag" invariant checkable per function by the structural guard.
static inline float flipConfirmTag() { return flipped ? -3.0f : -4.0f; }

// ---------- BLE UUIDs ----------
//  tail  param            type     range / units                       scope
//  0002  backswing ms     uint16   100..5000                           per-mode
//  0003  downswing ms     uint16   100..5000                           per-mode
//  0004  reach (amber)    uint8    1..HALF LEDs from center            per-mode
//  0005  brightness       uint8    0..255                              global
//  0006  control          uint8    0 idle / 1 loop / 2 single / 3 prev runtime
//  0007  get-ready ms     uint16   0..10000 (hold per get-ready stage) global
//  0008  finish hold ms   uint16   0..10000                            global
//  0009  rest ms          uint16   0..20000 (gap between reps)         global
//  000A  mode             uint8    0 putt / 1 full                     persisted
//  000B  top-hold ms      uint16   0..2000 (pause at top, full mode)   per-mode
//  000C  release          uint8    0..100 (0 pendular .. 100 lag)      per-mode
//  000D  follow-thru ms   uint16   0..10000 (impact -> finish)         per-mode
//  000E  rails lit        uint8    0/1 (keep impact+aim dots lit)       global
//  000F  aim2 enabled     uint8    0/1 (purple weight-shift dot)        per-mode (full)
//  0010  aim2 position    uint8    1..NUM_LEDS-2 (absolute LED index)   per-mode (full)
//  0011  aim2 when        uint8    0 address /1 top /2 down /3 impact   per-mode (full)
//  0012  follower status  bytes    read + NOTIFY (multi-stick set health)  runtime
//  0013  session command  bytes    write, opcode-tagged (form/join/disband) runtime
//  0014  flip             uint8    0 normal / 1 mirrored end-for-end    global, device-local
#define SVC_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_BACKSWING    "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_DOWNSWING    "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_REACH        "6e400004-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_BRIGHTNESS   "6e400005-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_CONTROL      "6e400006-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_GETREADY     "6e400007-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_HOLD         "6e400008-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_REST         "6e400009-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_MODE         "6e40000a-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_TOPHOLD      "6e40000b-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_RELEASE      "6e40000c-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_FOLLOW       "6e40000d-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_RAILS        "6e40000e-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_AIM2EN       "6e40000f-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_AIM2POS      "6e400010-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_AIM2WHEN     "6e400011-b5a3-f393-e0a9-e50e24dcca9e"
#define CH_FOLLOWERSTAT "6e400012-b5a3-f393-e0a9-e50e24dcca9e"   // follower-status: read + NOTIFY (§12)
#define CH_SESSIONCMD   "6e400013-b5a3-f393-e0a9-e50e24dcca9e"   // session-command: write, opcode-tagged (§6)
#define CH_FLIP         "6e400014-b5a3-f393-e0a9-e50e24dcca9e"   // 180 flip: strip orientation (global)

// Per-mode characteristic handles, so a mode switch can refresh their stored
// values to the newly-active profile (lets a BLE client read correct values).
BLECharacteristic *chBack = nullptr, *chDown = nullptr, *chReach = nullptr, *chTop = nullptr,
                  *chRelease = nullptr, *chFollow = nullptr, *chControl = nullptr,
                  *chAim2En = nullptr, *chAim2Pos = nullptr, *chAim2When = nullptr,
                  *chFollowerStat = nullptr;    // …0012 follower-status (read + NOTIFY)

static void chSetU16(BLECharacteristic *ch, uint16_t val) {
  if (!ch) return;
  uint8_t b[2] = {(uint8_t)(val & 0xFF), (uint8_t)(val >> 8)};
  ch->setValue(b, 2);
}
static void chSetU8(BLECharacteristic *ch, uint8_t val) {
  if (!ch) return;
  ch->setValue(&val, 1);
}
void refreshModeChars() {
  chSetU16(chBack,  profiles[mode].backswingMs);
  chSetU16(chDown,  profiles[mode].downswingMs);
  chSetU8 (chReach, profiles[mode].reachLeds);
  chSetU16(chTop,   profiles[mode].topHoldMs);
  chSetU8 (chRelease, profiles[mode].release);
  chSetU16(chFollow,  profiles[mode].followMs);
  chSetU8 (chAim2En,   profiles[mode].aim2Enabled);
  chSetU8 (chAim2Pos,  profiles[mode].aim2Led);
  chSetU8 (chAim2When, profiles[mode].aim2When);
}
// Push current control state to subscribers (e.g. a single stroke auto-resetting to idle).
void notifyControl() {
  if (!chControl) return;
  uint8_t b = control;
  chControl->setValue(&b, 1);
  chControl->notify();
}

// Blink the strip briefly so a user can bind a roster row to this physical stick (Blink-to-find, §6).
void identifyBlink() {
  invalidateFrameSkip();          // the blink overwrites the strip; force the next tick to repaint
  for (int k = 0; k < 3; k++) {
    fill_solid(leds, NUM_LEDS, CRGB(255, 255, 255)); showFrame(); delay(120);
    clearLeds(); showFrame(); delay(120);
  }
}

// Session formation (…0013 ops 1/2) is deferred out of the BLE callback to loop() on core 1, as
// disband and identify already are. Two reasons beyond consistency:
//   * It must first COMMIT any debounced edit. Joining flips syncRole immediately and the debounced
//     save is gated to `syncRole != ROLE_FOLLOWER`, so an edit made inside the 1500 ms debounce
//     would never reach NVS, would be overwritten in RAM by the leader's params, and would then be
//     silently rolled back to its pre-edit value by the loadParams() on revert.
//   * That commit is an NVS write, and every other Preferences call in this sketch runs on core 1.
//     `Preferences` is a stateful object whose begin() returns false (unchecked at every call site)
//     if it is already open, so a concurrent begin/end from the BLE task would close the handle
//     mid-save and silently drop whatever core 1 was writing — including both profile blobs.
// Cost: forming a set now stalls the render loop for the NVS commit plus the radio bring-up inside
// syncBecomeLeader/syncJoinSession (WiFi.mode + esp_now_init, order of 100 ms), so a stick that is
// looping will visibly freeze and jump phase once. That is a deliberate, one-off user action; the
// alternatives are losing the user's most recent edit and corrupting a concurrent NVS write. It
// also makes bring-up and tear-down symmetric — syncWifiDown() was already on core 1.
struct PendingSession { bool pending = false; uint8_t op = 0; uint16_t sid = 0; uint8_t channel = 6; uint8_t key[16] = {0}; };
static PendingSession pendingSession;
static portMUX_TYPE sessionMux = portMUX_INITIALIZER_UNLOCKED;
static void pendingMerge();            // defined with the kinematic staging block below

// Drain it on core 1. Flush first, then change role — the order is the whole point.
static void servicePendingSession() {
  PendingSession req;
  portENTER_CRITICAL(&sessionMux);
  if (!pendingSession.pending) { portEXIT_CRITICAL(&sessionMux); return; }
  req = pendingSession; pendingSession.pending = false;
  portEXIT_CRITICAL(&sessionMux);
  // Merge staged kinematics FIRST. While the stick is running, a kinematic edit is staged into
  // pendingKin rather than written to profiles[] (§7), and loop()'s own save is gated to idle so it
  // can never observe that. This one is not, so saving without merging would persist the PRE-EDIT
  // profile and clear paramsDirty — losing exactly the edit this flush exists to rescue.
  pendingMerge();                      // no-op unless something is staged; it checks under its own lock
  // Gate on the OLD role: a stick that is already a follower holds the leader's params in RAM and
  // must never commit them to its own flash. Orientation is exempt — it is the one setting a
  // follower still owns (see saveFlipNow).
  if (paramsDirty && syncRole != ROLE_FOLLOWER) { paramsDirty = false; saveParams(); }
  if (flipDirty) saveFlipNow();
  if (req.op == 1) syncBecomeLeader(req.sid, req.key, req.channel);
  else             syncJoinSession(req.sid, req.key, req.channel);
}

// Parse an opcode-tagged …0013 session command (matches the iOS CoreBluetoothTransport contract).
//   1 becomeLeader [1][key:16][CH_OP:1]   2 joinSession [2][key:16][CH_OP:1]   3 disband   4 identify
void handleSessionCmd(const String &v) {
  if (v.length() < 1) return;
  uint8_t op = (uint8_t)v[0];
  if ((op == 1 || op == 2) && v.length() >= 17) {
    uint8_t key[16];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)v[1 + i];
    uint8_t ch = (v.length() >= 18) ? (uint8_t)v[17] : 6;
    uint16_t sid = (uint16_t)key[0] | ((uint16_t)key[1] << 8);   // both sides derive sessionId from the key
    // DEFER to core 1 (see PendingSession): forming a session must first commit any debounced edit,
    // and NVS access belongs on the render core with every other Preferences call.
    portENTER_CRITICAL(&sessionMux);
    pendingSession.op = op; pendingSession.sid = sid; pendingSession.channel = ch;
    memcpy(pendingSession.key, key, 16);
    pendingSession.pending = true;
    portEXIT_CRITICAL(&sessionMux);
  } else if (op == 3) {
    syncDisband();
  } else if (op == 4) {
    // identify: empty target = blink this (clock) stick; else relay to the follower MAC (§13-ask-2).
    if (v.length() >= 7) {
      uint8_t mac[6];
      for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[1 + i];
      syncIdentifyFollower(mac);
    } else {
      // Defer to loop() on core 1 rather than blinking here: identifyBlink() blocks for 720 ms and
      // writes leds[] + latches the strip, and this runs on the BLE task (core 0). The remote path
      // has always deferred for exactly that reason (sync_firmware.ino, `identifyPending`); this
      // local path calling it inline was an oversight, and it is a concurrent writer to the buffer
      // showFrame() owns.
      syncDeferIdentifySelf();
    }
  }
}

// Keep the purple dot off any marker LED (impact / amber / red extents) for a given
// reach. Searches outward from the requested index. Enforced here too (not just in the
// app) so a raw BLE client can't park it on the ball or aim dot.
static bool aim2Occupied(int i, int centerIdx, int amberIdx) {
  return i == centerIdx || i == amberIdx || i == 0 || i == NUM_LEDS - 1;
}
int avoidAim2Overlap(int idx, int reachLeds) {
  idx = constrain(idx, 1, NUM_LEDS - 2);
  int centerIdx = (int)roundf(CENTER);
  int amberIdx  = (int)roundf(CENTER + reachLeds);
  if (!aim2Occupied(idx, centerIdx, amberIdx)) return idx;
  for (int step = 1; step < NUM_LEDS; step++) {
    int up = idx + step, dn = idx - step;
    if (up <= NUM_LEDS - 2 && !aim2Occupied(up, centerIdx, amberIdx)) return up;
    if (dn >= 1          && !aim2Occupied(dn, centerIdx, amberIdx)) return dn;
  }
  return idx;
}

// ---------- Kinematic staging (plan §7) ----------
// A running cycle must be phase-stable: kinematic edits (the timeline that feeds positionAt /
// cycleDuration — backswing/downswing/reach/release/follow/topHold/getReady/hold/rest/mode/aim2When)
// are STAGED here on a BLE write and merged into the live globals only at the next cycle boundary, so
// the comet never teleports mid-stroke and the leader matches what its followers render (they merge at
// the same boundary). Cosmetic edits (brightness/rails/aim2 enable+position) stay immediate. Restores
// the pre-refactor snapshot-at-stroke-start. onWrite runs on the BLE task (core 0); the merge runs in
// loop() (core 1) — so the buffer is guarded by a portMUX.
struct PendingKin {
  bool     dirty = false;
  uint8_t  targetMode = 0;                 // the mode the staged Profile fields apply to
  uint16_t backswingMs = 0, downswingMs = 0, topHoldMs = 0, followMs = 0;
  uint8_t  reachLeds = 0, release = 0, aim2When = 0;
  uint8_t  modeVal = 0;
  uint16_t getReadyVal = 0, holdVal = 0, restVal = 0;
};
static PendingKin pendingKin;
static portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;

// Seed the pending buffer from the current live kinematic state on the first stage of a cycle. Caller
// holds pendingMux. Subsequent stages this cycle overwrite individual fields, so the boundary merge is
// field-level (only touches kinematic fields → a live cosmetic edit can never be reverted, §7).
static void pendingSeed() {
  if (pendingKin.dirty) return;
  pendingKin.targetMode  = mode;
  pendingKin.backswingMs = profiles[mode].backswingMs;
  pendingKin.downswingMs = profiles[mode].downswingMs;
  pendingKin.topHoldMs   = profiles[mode].topHoldMs;
  pendingKin.followMs    = profiles[mode].followMs;
  pendingKin.reachLeds   = profiles[mode].reachLeds;
  pendingKin.release     = profiles[mode].release;
  pendingKin.aim2When    = profiles[mode].aim2When;
  pendingKin.modeVal     = mode;
  pendingKin.getReadyVal = getReadyMs;
  pendingKin.holdVal     = holdMs;
  pendingKin.restVal     = restMs;
  pendingKin.dirty       = true;
}

// Apply staged kinematic fields to the live globals at a cycle boundary (or when the stick stops).
static void pendingMerge() {
  portENTER_CRITICAL(&pendingMux);
  if (pendingKin.dirty) {
    Profile &a = profiles[pendingKin.targetMode];
    a.backswingMs = pendingKin.backswingMs;
    a.downswingMs = pendingKin.downswingMs;
    a.topHoldMs   = pendingKin.topHoldMs;
    a.followMs    = pendingKin.followMs;
    a.reachLeds   = pendingKin.reachLeds;
    a.release     = pendingKin.release;
    a.aim2When    = pendingKin.aim2When;
    mode       = pendingKin.modeVal;
    getReadyMs = pendingKin.getReadyVal;
    holdMs     = pendingKin.holdVal;
    restMs     = pendingKin.restVal;
    pendingKin.dirty = false;
  }
  portEXIT_CRITICAL(&pendingMux);
}

// ---------- BLE write handler ----------
// Per-mode params write into the *active* profile (profiles[mode]).
class CharCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String uuid = c->getUUID().toString();
    String v = c->getValue();
    if (v.isEmpty()) return;

    auto u16 = [&](void) -> uint16_t {
      if (v.length() >= 2) return (uint8_t)v[0] | ((uint8_t)v[1] << 8);
      return (uint8_t)v[0];
    };

    // control is runtime-only (not persisted) so it never boots into preview.
    if (uuid == CH_CONTROL) { control = min((int)(uint8_t)v[0], 3); if (syncRole == ROLE_LEADER) leaderMarkEvent(); return; }
    if (uuid == CH_SESSIONCMD) { handleSessionCmd(v); return; }                 // opcode-tagged session command (§6)

    bool running  = (control == 1 || control == 2);   // §7: stage kinematic edits mid-run, apply live otherwise
    bool cosmetic = false;                            // did we change a beacon-carried cosmetic field?

    // Stage a kinematic field while running (merged at the next boundary), else write it live. `live`
    // is volatile& so it binds both the volatile timing globals and the non-volatile profiles[] fields.
    auto stageU16 = [&](volatile uint16_t &live, uint16_t &pend, uint16_t val) {
      if (running) { portENTER_CRITICAL(&pendingMux); pendingSeed(); pend = val; portEXIT_CRITICAL(&pendingMux); }
      else live = val;
    };
    auto stageU8 = [&](volatile uint8_t &live, uint8_t &pend, uint8_t val) {
      if (running) { portENTER_CRITICAL(&pendingMux); pendingSeed(); pend = val; portEXIT_CRITICAL(&pendingMux); }
      else live = val;
    };

    if (uuid == CH_BACKSWING)     { uint16_t val = constrain(u16(), 100, 5000); stageU16(profiles[mode].backswingMs, pendingKin.backswingMs, val); chSetU16(c, val); }
    else if (uuid == CH_DOWNSWING){ uint16_t val = constrain(u16(), 100, 5000); stageU16(profiles[mode].downswingMs, pendingKin.downswingMs, val); chSetU16(c, val); }
    else if (uuid == CH_REACH)    { uint8_t r = constrain((int)(uint8_t)v[0], 1, (int)HALF);
                                    stageU8(profiles[mode].reachLeds, pendingKin.reachLeds, r);   // reach = kinematic
                                    profiles[mode].aim2Led = avoidAim2Overlap(profiles[mode].aim2Led, r);   // aim2 pos = cosmetic
                                    chSetU8(c, r); chSetU8(chAim2Pos, profiles[mode].aim2Led); cosmetic = true; }
    else if (uuid == CH_TOPHOLD)  { uint16_t val = constrain(u16(), 0, 2000);  stageU16(profiles[mode].topHoldMs, pendingKin.topHoldMs, val); chSetU16(c, val); }
    else if (uuid == CH_RELEASE)  { uint8_t val = min((int)(uint8_t)v[0], 100); stageU8(profiles[mode].release, pendingKin.release, val); chSetU8(c, val); }
    else if (uuid == CH_FOLLOW)   { uint16_t val = constrain(u16(), 0, 10000); stageU16(profiles[mode].followMs, pendingKin.followMs, val); chSetU16(c, val); }
    else if (uuid == CH_MODE)     { uint8_t m = min((int)(uint8_t)v[0], 1);
                                    if (running) { portENTER_CRITICAL(&pendingMux); pendingSeed(); pendingKin.modeVal = m; portEXIT_CRITICAL(&pendingMux); }
                                    else { mode = m; refreshModeChars(); }      // live mode switch refreshes per-mode chars
                                    chSetU8(c, m); }
    else if (uuid == CH_GETREADY) { uint16_t val = constrain(u16(), 0, 10000); stageU16(getReadyMs, pendingKin.getReadyVal, val); chSetU16(c, val); }
    else if (uuid == CH_HOLD)     { uint16_t val = constrain(u16(), 0, 10000); stageU16(holdMs, pendingKin.holdVal, val); chSetU16(c, val); }
    else if (uuid == CH_REST)     { uint16_t val = constrain(u16(), 0, 20000); stageU16(restMs, pendingKin.restVal, val); chSetU16(c, val); }
    else if (uuid == CH_AIM2WHEN) { uint8_t w = min((int)(uint8_t)v[0], 3);    stageU8(profiles[mode].aim2When, pendingKin.aim2When, w); chSetU8(c, w); }
    // --- cosmetic (immediate; beacon-propagated, §7) ---
    else if (uuid == CH_BRIGHTNESS){ brightness = (uint8_t)v[0]; FastLED.setBrightness(brightness); chSetU8(c, brightness); cosmetic = true; }
    else if (uuid == CH_RAILS)    { railsLit = min((int)(uint8_t)v[0], 1); chSetU8(c, railsLit); cosmetic = true; }
    else if (uuid == CH_AIM2EN)   { uint8_t e = min((int)(uint8_t)v[0], 1); profiles[mode].aim2Enabled = e; chSetU8(c, e); cosmetic = true; }
    else if (uuid == CH_AIM2POS)  { uint8_t pos = avoidAim2Overlap((int)(uint8_t)v[0], profiles[mode].reachLeds); profiles[mode].aim2Led = pos; chSetU8(c, pos); cosmetic = true; }
    // --- orientation (immediate, device-local; NOT beacon-carried, NOT staged) ---
    // Cosmetic in the §7 sense -- it feeds neither positionAt nor cycleDuration, so it applies live
    // rather than at the next cycle boundary. Flipping mid-stroke is safe precisely because the
    // renderer holds no temporal state: frame N draws logical, frame N+1 draws mirrored, both at the
    // same u, so the comet lands at its mirror position with identical phase -- a reflection in
    // space, never a jump in time. Deliberately does NOT set `cosmetic`: that flag bursts a beacon
    // carrying the cosmetic GLOBALS, and orientation is per-stick and never shared. It also takes
    // its own save path rather than paramsDirty (see saveFlipNow).
    else if (uuid == CH_FLIP)     { uint8_t f = min((int)(uint8_t)v[0], 1);
                                    flipped = f; chSetU8(c, f);            // echo the clamped value back
                                    flipDirty = true;
                                    flipConfirmArmedMs = millis();
                                    return; }
    else return;                    // unknown characteristic: nothing to persist

    paramsDirty = true;             // mark for debounced save in loop() (staged edits flush before the save fires)
    lastWriteMs = millis();
    // Cosmetic changes take effect immediately, so burst a beacon now; kinematic changes are staged and
    // propagate when the leader bursts at its own cycle boundary (§4.4/§7), keeping the set phase-locked.
    if (cosmetic && syncRole == ROLE_LEADER) leaderMarkEvent();
  }
};

// Re-advertise on disconnect so a stick stays reconnectable after the phone (or the add-flow's transient
// candidate link) drops — EXCEPT a synced follower, which deliberately goes BLE-quiet while in a set
// (plan §2.1). It re-advertises on revert (syncRevertToStandalone) so it can be re-added.
class ServerCB : public BLEServerCallbacks {
  void onDisconnect(BLEServer *s) override {
    if (syncRole != ROLE_FOLLOWER) BLEDevice::startAdvertising();
  }
};

void setupBLE() {
  BLEDevice::init("PuttTempo");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  // GATT handle budget -- the default (15) SILENTLY truncates the table after ~7 characteristics,
  // and the Arduino BLE wrapper does not check the error, so the symptom is a client discovering a
  // short table and rejecting the device as incompatible. Count it exactly whenever a char is added:
  //   1 (service) + 19 chars x 2 + 2 CCCDs (…0006 control, …0012 follower-status) = 41.
  // 48 reserved leaves room for 3 more characteristics before this must be raised.
  BLEService *svc = server->createService(BLEUUID(SVC_UUID), 48);   // 19 chars (…0002-…0014) = 41 handles

  auto mk = [&](const char *uuid, uint16_t init, bool twoByte, bool notify = false) {
    uint32_t props = BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE;
    if (notify) props |= BLECharacteristic::PROPERTY_NOTIFY;
    BLECharacteristic *ch = svc->createCharacteristic(uuid, props);
    ch->setCallbacks(new CharCB());
    if (notify) ch->addDescriptor(new BLE2902());   // CCCD so clients can subscribe
    if (twoByte) { uint8_t b[2] = {(uint8_t)(init & 0xFF), (uint8_t)(init >> 8)}; ch->setValue(b, 2); }
    else { uint8_t b = (uint8_t)init; ch->setValue(&b, 1); }
    return ch;
  };

  chBack  = mk(CH_BACKSWING,  profiles[mode].backswingMs, true);
  chDown  = mk(CH_DOWNSWING,  profiles[mode].downswingMs, true);
  chReach = mk(CH_REACH,      profiles[mode].reachLeds,   false);
  mk(CH_BRIGHTNESS, brightness,                 false);
  chControl = mk(CH_CONTROL,  control,           false, true);   // NOTIFY: pushes state changes
  mk(CH_GETREADY,   getReadyMs,                 true);
  mk(CH_HOLD,       holdMs,                     true);
  mk(CH_REST,       restMs,                     true);
  mk(CH_MODE,       mode,                       false);
  chTop     = mk(CH_TOPHOLD,  profiles[mode].topHoldMs,   true);
  chRelease = mk(CH_RELEASE,  profiles[mode].release,     false);
  chFollow  = mk(CH_FOLLOW,   profiles[mode].followMs,    true);
  mk(CH_RAILS,      railsLit,                   false);
  chAim2En   = mk(CH_AIM2EN,   profiles[mode].aim2Enabled, false);
  chAim2Pos  = mk(CH_AIM2POS,  profiles[mode].aim2Led,     false);
  chAim2When = mk(CH_AIM2WHEN, profiles[mode].aim2When,    false);
  chFollowerStat = mk(CH_FOLLOWERSTAT, 0, false, true);   // …0012 READ + NOTIFY (follower-status)
  mk(CH_SESSIONCMD, 0, false);                            // …0013 WRITE (opcode-tagged session command)
  mk(CH_FLIP,       flipped, false);                      // …0014 strip orientation (global, device-local)

  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);

  // Put the name in the scan response: the 128-bit service UUID fills most of the
  // 31-byte advertising packet, so this keeps name-based discovery reliable (and
  // iOS/CoreBluetooth clients should still scan by service UUID, which is in the adv).
  BLEAdvertisementData scanResp;
  scanResp.setName("PuttTempo");
  adv->setScanResponseData(scanResp);
  adv->setScanResponse(true);

  BLEDevice::startAdvertising();
}

// ---------- Easing (phase 0..1 -> eased 0..1) ----------
float easeInOut(float ph) { return 0.5f - 0.5f * cosf(ph * PI); }      // 0 vel at both ends (turnarounds)
float easeOut  (float ph) { return sinf(ph * PI / 2.0f); }            // fast -> slow (decelerate to a stop)

// Drop-to-impact progress 0..1. Blends the free-pendulum profile (release 0) toward a
// back-loaded late "lag" release (release 1): clubhead hangs, then snaps through impact.
// Both ends start at zero velocity (ph=0) and peak at impact (ph=1), so tempo is unchanged.
float dropEase(float ph, float release01) {
  float pendular = 1.0f - cosf(ph * PI / 2.0f);   // free pendulum (quarter-sine accel)
  float lag      = ph * ph * ph * ph;             // back-loaded late release
  return pendular + (lag - pendular) * release01;
}

// ---------- Static-marker rendering (preview / dot maps) ----------
// (The blocking comet renderers — addComet/drawComet/drawCometRefs and the runStroke* stroke drivers —
//  were retired by the analytic stateless renderer above; only these static-dot helpers remain, used
//  by the preview branch of loop().)

// Static ball + target markers (putt get-ready phase).
void showMarkers(int centerIdx, int targetIdx, bool showTarget) {
  clearLeds();
  if (centerIdx >= 0 && centerIdx < NUM_LEDS) leds[centerIdx] = BALL_COLOR;
  if (showTarget && targetIdx >= 0 && targetIdx < NUM_LEDS) leds[targetIdx] = TARGET_COLOR;
  showFrame();
}

// Static reference dots (full get-ready phase / preview).
void showRefs(const RefDot *refs, int n) {
  clearLeds();
  for (int k = 0; k < n; k++)
    if (refs[k].idx >= 0 && refs[k].idx < NUM_LEDS) leds[refs[k].idx] = refs[k].col;
  showFrame();
}

// ---------- Persistence helpers ----------
void loadParams() {
  prefs.begin("tempo", true);           // namespace, read-only
  mode       = prefs.getUChar ("mode", mode);
  brightness = prefs.getUChar ("bri",  brightness);
  getReadyMs = prefs.getUShort("ready", getReadyMs);
  holdMs     = prefs.getUShort("hold",  holdMs);
  restMs     = prefs.getUShort("rest",  restMs);
  railsLit   = prefs.getUChar ("rails", railsLit);
  flipped    = prefs.getUChar ("flip",  flipped);
  prefs.getBytes("prof0", &profiles[0], sizeof(Profile));   // unchanged if key absent
  prefs.getBytes("prof1", &profiles[1], sizeof(Profile));
  prefs.end();

  // Defensive: clamp anything loaded from flash to valid ranges (guards a corrupt/foreign blob).
  mode       = constrain((int)mode, 0, 1);
  // Floor brightness on load so a persisted 0/very-low value can never make the strip appear
  // permanently "dead" across reboots. Runtime BLE writes can still go dimmer for live tuning;
  // a power-cycle always recovers to a visible level.
  brightness = constrain((int)brightness, 8, 255);
  getReadyMs = constrain(getReadyMs, 0, 10000);
  holdMs     = constrain(holdMs, 0, 10000);
  restMs     = constrain(restMs, 0, 20000);
  railsLit   = constrain((int)railsLit, 0, 1);
  flipped    = constrain((int)flipped, 0, 1);
  for (int m = 0; m < 2; m++) {
    profiles[m].backswingMs = constrain(profiles[m].backswingMs, 100, 5000);
    profiles[m].downswingMs = constrain(profiles[m].downswingMs, 100, 5000);
    profiles[m].reachLeds   = constrain((int)profiles[m].reachLeds, 1, (int)HALF);
    profiles[m].topHoldMs   = constrain(profiles[m].topHoldMs, 0, 2000);
    profiles[m].release     = constrain((int)profiles[m].release, 0, 100);
    profiles[m].followMs    = constrain(profiles[m].followMs, 0, 10000);
    profiles[m].aim2Enabled = constrain((int)profiles[m].aim2Enabled, 0, 1);
    profiles[m].aim2Led     = avoidAim2Overlap((int)profiles[m].aim2Led, profiles[m].reachLeds);
    profiles[m].aim2When    = constrain((int)profiles[m].aim2When, 0, 3);
  }
}

void saveParams() {
  prefs.begin("tempo", false);          // read-write; putX skips no-op writes (saves flash)
  prefs.putUChar ("mode", mode);
  prefs.putUChar ("bri",  brightness);
  prefs.putUShort("ready", getReadyMs);
  prefs.putUShort("hold",  holdMs);
  prefs.putUShort("rest",  restMs);
  prefs.putUChar ("rails", railsLit);
  prefs.putUChar ("flip",  flipped);
  prefs.putBytes("prof0", &profiles[0], sizeof(Profile));
  prefs.putBytes("prof1", &profiles[1], sizeof(Profile));
  prefs.end();
}

void setup() {
  Serial.begin(115200);
#ifdef LED_POWER_PIN                     // Dig2go only: close the LED-power relay so the strip gets 5V
  pinMode(LED_POWER_PIN, OUTPUT);
  digitalWrite(LED_POWER_PIN, LED_POWER_ON);
#endif
  loadParams();                         // restore saved settings (or first-boot defaults)
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(physLeds, NUM_LEDS);   // PHYSICAL buffer, not leds[]
  FastLED.setBrightness(brightness);
  clearLeds(); showFrame();
  setupBLE();
  Serial.println("Tempo Trainer ready - pair over BLE");
}

// ============================================================================
//  Analytic, STATELESS renderer (Phase 1 — the linchpin for sync, plan §3.2).
//  Pure functions of cycle-time `u` (ms) and the active params: identical `u`
//  ⇒ identical frame on any node, with NO static state, NO fadeToBlackBy IIR,
//  NO millis(). `positionAt` is golden-parity-verified (firmware_tests/).
// ============================================================================

// Total rendered cycle time (ms), excluding the dark rest gap (handled by the loop epoch).
uint32_t cycleDuration(const Profile &p) {
  return (uint32_t)getReadyMs + p.backswingMs + p.topHoldMs + p.downswingMs + p.followMs + holdMs;
}

// Shared frame grid (plan §3.4): quantize cycle-time to FRAME_MS so all nodes compute the SAME frame
// index k at a shared instant → identical LED buffer, making show()/quantization common-mode (0 skew).
static const float FRAME_MS = 8.0f;                       // 125 Hz grid; tightenable toward ~6 ms
static inline float gridQuantize(float u) { return floorf(u / FRAME_MS) * FRAME_MS; }

// Head position (fractional LED index) at cycle-time u. Ported verbatim from the pre-refactor
// segment math (runStroke / runStrokeFull) and verified in firmware_tests/golden_kinematics.c.
float positionAt(float u, const Profile &p) {
  float amp = p.reachLeds;
  if (mode == MODE_FULL) {
    float center = CENTER, backRed = NUM_LEDS - 1, backAmber = CENTER + amp, folRed = 0.0f;
    float dOut = backRed - center, dIn = backRed - backAmber;
    uint32_t bOut = (uint32_t)(p.backswingMs * (dOut / (dOut + dIn))), bIn = p.backswingMs - bOut;
    uint32_t dBack = (uint32_t)(p.downswingMs * (dIn / (dIn + dOut))), dDown = p.downswingMs - dBack;
    if (u < getReadyMs) return center;                                                   u -= getReadyMs;
    if (u < bOut)  { float ph = u / (float)bOut;  return center    + (backRed - center)    * easeInOut(ph); } u -= bOut;
    if (u < bIn)   { float ph = u / (float)bIn;   return backRed   + (backAmber - backRed) * easeInOut(ph); } u -= bIn;
    if (u < p.topHoldMs) return backAmber;                                               u -= p.topHoldMs;
    if (u < dBack) { float ph = u / (float)dBack; return backAmber + (backRed - backAmber)* easeInOut(ph); } u -= dBack;
    if (u < dDown) { float ph = u / (float)dDown; return backRed   + (center - backRed)   * dropEase(ph, p.release / 100.0f); } u -= dDown;
    if (u < p.followMs) { float ph = u / (float)p.followMs; return center + (folRed - center) * easeOut(ph); }
    return (p.followMs > 0 ? folRed : center);
  } else {
    if (u < getReadyMs) return CENTER;                                                   u -= getReadyMs;
    if (u < p.backswingMs) { float ph = u / (float)p.backswingMs; return CENTER + amp * sinf(ph * PI / 2.0f); } u -= p.backswingMs;
    if (u < p.downswingMs) { float ph = u / (float)p.downswingMs; return CENTER + amp * (1.0f - dropEase(ph, p.release / 100.0f)); } u -= p.downswingMs;
    if (u < p.followMs)    { float ph = u / (float)p.followMs;    return CENTER - amp * easeOut(ph); }
    return CENTER + (p.followMs > 0 ? -amp : 0.0f);
  }
}

// Travel direction from SEGMENT IDENTITY (not history / not a finite difference) — deterministic
// at u=0, edges, and zero-velocity turnarounds (plan §3.2). +1 higher index, -1 lower, 0 still.
int strokeDirAt(float u, const Profile &p) {
  float amp = p.reachLeds;
  if (mode == MODE_FULL) {
    float dOut = (NUM_LEDS - 1) - CENTER, dIn = (NUM_LEDS - 1) - (CENTER + amp);
    uint32_t bOut = (uint32_t)(p.backswingMs * (dOut / (dOut + dIn))), bIn = p.backswingMs - bOut;
    uint32_t dBack = (uint32_t)(p.downswingMs * (dIn / (dIn + dOut))), dDown = p.downswingMs - dBack;
    if (u < getReadyMs) return 0;      u -= getReadyMs;
    if (u < bOut) return 1;            u -= bOut;
    if (u < bIn) return -1;            u -= bIn;
    if (u < p.topHoldMs) return 0;     u -= p.topHoldMs;
    if (u < dBack) return 1;           u -= dBack;
    if (u < dDown) return -1;          u -= dDown;
    if (u < p.followMs) return -1;
    return 0;
  } else {
    if (u < getReadyMs) return 0;      u -= getReadyMs;
    if (u < p.backswingMs) return 1;   u -= p.backswingMs;
    if (u < p.downswingMs) return -1;  u -= p.downswingMs;
    if (u < p.followMs) return -1;
    return 0;
  }
}

// The full-swing purple dot lights from its aim2When boundary onward (plan §4.5), as a function of u.
static bool aim2LitAt(float u, const Profile &p) {
  uint32_t tTop = (uint32_t)getReadyMs + p.backswingMs;         // top of backswing
  uint32_t tDown = tTop + p.topHoldMs;                          // start of downswing
  uint32_t tImpact = tDown + p.downswingMs;                     // impact
  switch (p.aim2When) {
    case 0: return u >= getReadyMs;   // address (after get-ready)
    case 1: return u >= tTop;
    case 2: return u >= tDown;
    case 3: return u >= tImpact;
  }
  return false;
}

// Analytic comet: bright head + fading tail trailing OPPOSITE `dir`. Closed-form in (pos,dir),
// replacing the IIR "soft trail" — visually equivalent, stateless (plan §3.2).
static void drawCometAnalytic(float pos, int dir, int loIdx, int hiIdx) {
  if (dir == 0) dir = 1;                                        // still: default trail orientation
  int base = (int)floorf(pos);
  for (int i = -1; i <= TAIL; i++) {                            // i=-1 leading pixel, i>0 trailing tail
    int idx = base - dir * i;
    if (idx < loIdx || idx > hiIdx || idx < 0 || idx >= NUM_LEDS) continue;
    uint8_t b = (i <= 0) ? 255 : (uint8_t)(255.0f * (1.0f - (float)i / (TAIL + 1)));
    leds[idx] += CRGB(b, b, b);
  }
}

// Draw the whole frame for cycle-time u. Full clear + repaint (no temporal state).
void renderFrameAt(float u) {
  const Profile &p = profiles[mode];
  float amp = p.reachLeds;
  int centerIdx = (int)roundf(CENTER);
  clearLeds();
  if (railsLit) {
    if (centerIdx >= 0 && centerIdx < NUM_LEDS) leds[centerIdx] = BALL_COLOR;
    int amberIdx = (int)roundf(CENTER + amp);
    if (amberIdx >= 0 && amberIdx < NUM_LEDS) leds[amberIdx] = TARGET_COLOR;
    if (mode == MODE_FULL) { leds[NUM_LEDS - 1] = RED_COLOR; leds[0] = RED_COLOR; }
  }
  if (mode == MODE_FULL && p.aim2Enabled && aim2LitAt(u, p)) {
    int a = constrain((int)p.aim2Led, 1, NUM_LEDS - 2);
    leds[a] = AIM2_COLOR;
  }
  // Get-ready countdown (u < getReadyMs): pulse the green impact LED smoothly at 1 Hz ("3-2-1-go")
  // over the lit aim dots — no moving comet yet. Closed-form in u (was the blocking countdownPulses),
  // so every node renders the identical pulse at the shared instant.
  if (u < getReadyMs) {
    float local = u - floorf(u / 1000.0f) * 1000.0f;      // ms into the current 1 s pulse
    float lvl   = sinf((local / 1000.0f) * PI);           // smooth 0 -> 1 -> 0
    if (centerIdx >= 0 && centerIdx < NUM_LEDS) leds[centerIdx] = CRGB(0, (uint8_t)(220.0f * lvl), 0);
    showFrame();
    return;
  }
  drawCometAnalytic(positionAt(u, p), strokeDirAt(u, p), 0, NUM_LEDS - 1);
  showFrame();
}

// The active mode's static dot map (green ball, amber top-of-backswing, red extents, purple aim
// dot). Extracted from the preview branch of loop() so the orientation-confirmation window can
// reuse it: it is exactly the picture that answers "which way does the stick run now?".
void renderDotMap() {
  float amp = profiles[mode].reachLeds;
  if (mode == MODE_FULL) {
    const RefDot refs[] = {
      { (int)roundf(CENTER),       BALL_COLOR   },
      { (int)roundf(CENTER + amp), TARGET_COLOR },
      { NUM_LEDS - 1,              RED_COLOR    },
      { 0,                         RED_COLOR    },
      { constrain((int)profiles[mode].aim2Led, 1, NUM_LEDS - 2), AIM2_COLOR },
    };
    showRefs(refs, profiles[mode].aim2Enabled ? 5 : 4);
  } else {
    showMarkers((int)roundf(CENTER), (int)roundf(CENTER + amp), true);
  }
}

// Cycle epoch for the non-blocking render loop (esp_timer µs at the current cycle's u=0).
static int64_t cycleEpochUs = 0;
// Frame-skip: the last quantized u rendered. Negative values are sentinels — -1 nothing painted,
// -2 rest gap blanked, -3/-4 the orientation-confirm dot map, tagged with the orientation it was
// painted at so a second flip inside the window still repaints. File scope (not a function static)
// so anything that paints the strip out of band can invalidate it.
static float lastFrameU = -1.0f;

void invalidateFrameSkip() { lastFrameU = -1.0f; syncInvalidateFollowerFrameSkip(); }
static uint8_t  renderPrevControl = 0xFF;

void loop() {
  // Debounced persist, GATED TO IDLE so a flash cache-stall never lands mid-stroke (plan §3.5).
  // Followers hold live params in RAM and never commit (plan §3.5), so gate on standalone/leader.
  if (paramsDirty && control == 0 && syncRole != ROLE_FOLLOWER && millis() - lastWriteMs > 1500) {
    paramsDirty = false; saveParams();
  }

  servicePendingSession();   // BLE-callback-deferred session formation, incl. its NVS flush (core 1)
  syncDrainDeferred();       // perform RX-callback-deferred revert/identify here on core 1 (#1/#5)
  leaderBeaconTick();        // leader: broadcast the SyncBeacon at 4 Hz (no-op otherwise)
  syncServiceTimeouts();     // follower: T_JOIN/orphan revert; leader: prune + backstop (no-op otherwise)

  // FOLLOWER: render phase-locked off the shared clock (§3.4). All state read via the seqlock inside.
  if (syncRole == ROLE_FOLLOWER) {
    followerRenderStep();
    delay(4);
    return;
  }

  uint8_t c = control;
  // Not running (idle/preview): flush any staged kinematic edits to the live globals so preview/next-run
  // and the debounced NVS save see the latest values (§7).
  if (c != 1 && c != 2 && pendingKin.dirty) pendingMerge();

  if (c == 1 || c == 2) {               // loop / single — NON-BLOCKING, stateless analytic render
    int64_t now = esp_timer_get_time();
    if (renderPrevControl != c) {                          // (re)start on control change
      cycleEpochUs = now; renderPrevControl = c; lastFrameU = -1.0f;
      pendingMerge();                                       // start the cycle from the latest staged params
      if (syncRole == ROLE_LEADER) leaderMarkEvent();       // #9: burst a beacon so followers switch in sync
    }
    if (syncRole == ROLE_LEADER) leaderEpochUs = cycleEpochUs;   // beacon carries the render epoch (shared grid)
    uint32_t cd = cycleDuration(profiles[mode]);
    uint32_t total = cd + (c == 1 ? restMs : 0);           // loop adds the dark rest gap; single does not
    float u = (now - cycleEpochUs) / 1000.0f;              // ms since this cycle's u=0
    if (u >= total) {
      if (c == 1) {                                                       // next rep: advance epoch by exactly
        cycleEpochUs += (int64_t)total * 1000;                            // one cycle (keep the grid; §4.5, #11)
        if (now - cycleEpochUs > (int64_t)total * 2000) cycleEpochUs = now;   // resync if we fell >1 cycle behind
        pendingMerge();                                                   // §7: apply staged kinematic edits at the boundary
        if (syncRole == ROLE_LEADER) leaderMarkEvent();                   // #9: merged params + new epoch → tell followers now
        renderFrameAt(0); lastFrameU = 0.0f;
      } else { control = 0; renderPrevControl = 0xFF; notifyControl();    // single done → idle
               if (syncRole == ROLE_LEADER) leaderMarkEvent(); }          // #11: burst the idle transition to followers
    } else if (u >= cd) {
      // Dark rest gap. The dot map is static, so paint it ONCE on entry rather than every 4 ms tick
      // — a 144-LED latch takes ~4.3 ms, so repainting at loop rate would latch back to back. The
      // sentinel carries the orientation so a second flip inside the window still forces a repaint.
      if (flipConfirmActive()) {
        const float tag = flipConfirmTag();
        if (lastFrameU != tag) { lastFrameU = tag; renderDotMap(); }                      // orientation answer
      } else if (lastFrameU != -2.0f) { lastFrameU = -2.0f; clearLeds(); showFrame(); }   // blank once
      // Commit a pending orientation AFTER the paint, so the NVS stall lands on an already-dark
      // strip. This is the only safe point that recurs while the stick keeps looping (see
      // saveFlipNow); with restMs == 0 there is no gap and the commit waits for idle.
      if (flipDirty) saveFlipNow();
    } else {
      float qu = gridQuantize(u);                          // shared frame grid (§3.4); skip duplicate frames
      if (qu != lastFrameU) { lastFrameU = qu; renderFrameAt(qu); }
    }
    delay(4);                           // ~250 Hz service; the grid-paced core-1 render task lands with sync (plan §3.5)
  } else if (c == 3) {                  // preview: live dot map for the active mode
    renderPrevControl = 0xFF;
    if (flipDirty) saveFlipNow();
    renderDotMap();                     // includes the purple dot when enabled, so it can be placed live
    delay(30);
  } else {                              // idle: dim center marker so you can line up
    renderPrevControl = 0xFF;
    if (flipDirty) saveFlipNow();
    if (flipConfirmActive()) {          // just flipped: show the dot map, not a single dim pixel
      renderDotMap();
    } else {
      fadeToBlackBy(leds, NUM_LEDS, 40);
      leds[(int)roundf(CENTER)] = CRGB(0, 40, 0);
      showFrame();
    }
    delay(20);
  }
}
