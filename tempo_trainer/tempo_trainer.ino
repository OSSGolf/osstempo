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
// as a compile flag). The pinned copies under docs/ ship with the right line uncommented.
#if !defined(BOARD_DIG2GO) && !defined(BOARD_DIY_ESP32)
  #define BOARD_DIG2GO           // QuinLED-Dig2go pre-assembled (the board this repo runs on now)
  //#define BOARD_DIY_ESP32      // generic ESP32 dev board + 470ohm + 1000uF
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

CRGB leds[NUM_LEDS];

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
volatile int      aim2Now    = -1;               // full-swing: LED index of the purple dot while lit, else -1

// Comet look
const uint8_t TAIL = 6;                           // fading tail length in LEDs
const CRGB    DOT_COLOR    = CRGB(255, 255, 255); // the moving comet (white)
const CRGB    BALL_COLOR   = CRGB(0, 120, 0);     // center / ball / impact (green)
const CRGB    TARGET_COLOR = CRGB(255, 90, 0);    // top-of-backswing aim point (amber)
const CRGB    RED_COLOR    = CRGB(200, 0, 0);     // max-width extent / strip ends (red, full mode)
const CRGB    AIM2_COLOR   = CRGB(150, 70, 255);  // purple secondary aim (full-swing weight-shift dot)

// Types used in function signatures, declared up here so the Arduino IDE's auto-generated
// prototypes (inserted near the top of the sketch) can see them before first use.
typedef float (*EaseFn)(float);                   // easing function pointer
struct RefDot { int idx; CRGB col; };             // a static reference dot kept lit during a sweep

// ---------- Persistence (NVS) ----------
Preferences       prefs;
volatile bool     paramsDirty = false;           // set on BLE write; debounced-saved in loop()
volatile uint32_t lastWriteMs = 0;

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
  for (int k = 0; k < 3; k++) {
    fill_solid(leds, NUM_LEDS, CRGB(255, 255, 255)); FastLED.show(); delay(120);
    FastLED.clear(true); delay(120);
  }
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
    if (op == 1) syncBecomeLeader(sid, key, ch);
    else         syncJoinSession(sid, key, ch);
  } else if (op == 3) {
    syncDisband();
  } else if (op == 4) {
    // identify: empty target = blink this (clock) stick; else relay to the follower MAC (§13-ask-2).
    if (v.length() >= 7) {
      uint8_t mac[6];
      for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[1 + i];
      syncIdentifyFollower(mac);
    } else {
      identifyBlink();
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
    if (uuid == CH_CONTROL) { control = min((int)(uint8_t)v[0], 3); return; }   // value is 0..255, so only the upper clamp is needed
    if (uuid == CH_SESSIONCMD) { handleSessionCmd(v); return; }                 // opcode-tagged session command (§6)

    if (uuid == CH_BACKSWING)       profiles[mode].backswingMs = constrain(u16(), 100, 5000);
    else if (uuid == CH_DOWNSWING)  profiles[mode].downswingMs = constrain(u16(), 100, 5000);
    else if (uuid == CH_REACH)    { profiles[mode].reachLeds   = constrain((int)(uint8_t)v[0], 1, (int)HALF);
                                    profiles[mode].aim2Led = avoidAim2Overlap(profiles[mode].aim2Led, profiles[mode].reachLeds); }
    else if (uuid == CH_TOPHOLD)    profiles[mode].topHoldMs   = constrain(u16(), 0, 2000);
    else if (uuid == CH_RELEASE)    profiles[mode].release     = min((int)(uint8_t)v[0], 100);
    else if (uuid == CH_FOLLOW)     profiles[mode].followMs    = constrain(u16(), 0, 10000);
    else if (uuid == CH_MODE)     { mode = min((int)(uint8_t)v[0], 1); refreshModeChars(); }
    else if (uuid == CH_BRIGHTNESS){ brightness = (uint8_t)v[0]; FastLED.setBrightness(brightness); }
    else if (uuid == CH_GETREADY)   getReadyMs = constrain(u16(), 0, 10000);
    else if (uuid == CH_HOLD)        holdMs     = constrain(u16(), 0, 10000);
    else if (uuid == CH_REST)        restMs     = constrain(u16(), 0, 20000);
    else if (uuid == CH_RAILS)       railsLit   = min((int)(uint8_t)v[0], 1);
    else if (uuid == CH_AIM2EN)   profiles[mode].aim2Enabled = min((int)(uint8_t)v[0], 1);
    else if (uuid == CH_AIM2POS)  profiles[mode].aim2Led     = avoidAim2Overlap((int)(uint8_t)v[0], profiles[mode].reachLeds);
    else if (uuid == CH_AIM2WHEN) profiles[mode].aim2When    = min((int)(uint8_t)v[0], 3);
    else return;                    // unknown characteristic: nothing to persist

    paramsDirty = true;             // mark for debounced save in loop()
    lastWriteMs = millis();
  }
};

void setupBLE() {
  BLEDevice::init("PuttTempo");
  BLEServer *server = BLEDevice::createServer();
  // 16 characteristics need ~34 GATT handles (2 each + 1 for control's CCCD + 1 service);
  // the default (15) silently truncates the table after ~7 chars, so allocate generously.
  BLEService *svc = server->createService(BLEUUID(SVC_UUID), 48);   // 18 chars (+…0012/…0013) need more handles

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

// ---------- Rendering ----------
// Adds the comet (bright head + fading tail) at fractional pos, CLIPPED to [loIdx, hiIdx]
// so the impact / backswing / extent markers visually "bound" it. The tail trails opposite
// travel, so it flips when the comet reverses.
static void addComet(float pos, int loIdx, int hiIdx) {
  static float prevPos = pos;
  static int   dir = 1;                    // keep last real direction through holds/turnarounds
  if      (pos > prevPos + 0.01f) dir = 1; // moving toward higher index
  else if (pos < prevPos - 0.01f) dir = -1;// toward lower index
  prevPos = pos;
  int base = (int)floorf(pos);
  for (int i = -1; i <= TAIL; i++) {      // i=-1 is the leading pixel, i>0 is the trailing tail
    int idx = base - dir * i;
    if (idx < loIdx || idx > hiIdx) continue;   // bound the comet to the active segment
    if (idx < 0 || idx >= NUM_LEDS) continue;
    float dist = fabs(idx - pos);
    uint8_t b = (uint8_t)max(0.0f, 255.0f * (1.0f - dist / (TAIL + 1)));
    leds[idx] += CRGB(scale8(DOT_COLOR.r, b), scale8(DOT_COLOR.g, b), scale8(DOT_COLOR.b, b));
  }
}

// Putt renderer: bounded comet + rails. When railsLit, the green impact and amber backswing
// dots stay lit as the bounding "walls"; otherwise the amber shows until reached (legacy look)
// only on the backswing (legacyAmber).
void drawComet(float pos, int centerIdx, int reachIdx, bool legacyAmber, int loIdx, int hiIdx) {
  fadeToBlackBy(leds, NUM_LEDS, 90);          // soft trails
  if (railsLit) {
    if (centerIdx >= 0 && centerIdx < NUM_LEDS) leds[centerIdx] = BALL_COLOR;   // impact rail
    if (reachIdx  >= 0 && reachIdx  < NUM_LEDS) leds[reachIdx]  = TARGET_COLOR; // backswing rail
  } else if (legacyAmber && reachIdx >= 0 && reachIdx < NUM_LEDS && pos < reachIdx - 0.5f) {
    leds[reachIdx] = TARGET_COLOR;            // keep the aim point lit until reached
  }
  addComet(pos, loIdx, hiIdx);
  FastLED.show();
}

// Full renderer: bounded comet over the reference dots (the bounding rails, gated by railsLit).
void drawCometRefs(float pos, const RefDot *refs, int n, int loIdx, int hiIdx) {
  fadeToBlackBy(leds, NUM_LEDS, 90);
  if (railsLit)
    for (int k = 0; k < n; k++)
      if (refs[k].idx >= 0 && refs[k].idx < NUM_LEDS) leds[refs[k].idx] = refs[k].col;
  // Purple weight-shift dot stays lit once it appears, independent of railsLit.
  if (aim2Now >= 0 && aim2Now < NUM_LEDS) leds[aim2Now] = AIM2_COLOR;
  addComet(pos, loIdx, hiIdx);
  FastLED.show();
}

// Static ball + target markers (putt get-ready phase).
void showMarkers(int centerIdx, int targetIdx, bool showTarget) {
  FastLED.clear();
  if (centerIdx >= 0 && centerIdx < NUM_LEDS) leds[centerIdx] = BALL_COLOR;
  if (showTarget && targetIdx >= 0 && targetIdx < NUM_LEDS) leds[targetIdx] = TARGET_COLOR;
  FastLED.show();
}

// Static reference dots (full get-ready phase / preview).
void showRefs(const RefDot *refs, int n) {
  FastLED.clear();
  for (int k = 0; k < n; k++)
    if (refs[k].idx >= 0 && refs[k].idx < NUM_LEDS) leds[refs[k].idx] = refs[k].col;
  FastLED.show();
}

// Get-ready countdown: pulse the green impact LED smoothly `pulses` times (1s each, a
// "3-2-1-go"), with the reference dots lit so the golfer can aim. Replaces the old 2-stage hold.
void countdownPulses(int centerIdx, const RefDot *refs, int n, int pulses) {
  for (int k = 0; k < pulses && control != 0; k++) {
    uint32_t t0 = millis();
    while (millis() - t0 < 1000 && control != 0) {
      float ph  = (millis() - t0) / 1000.0f;     // 0..1
      float lvl = sinf(ph * PI);                  // smooth 0 -> 1 -> 0
      FastLED.clear();
      for (int j = 0; j < n; j++)
        if (refs[j].idx >= 0 && refs[j].idx < NUM_LEDS) leds[refs[j].idx] = refs[j].col;
      if (centerIdx >= 0 && centerIdx < NUM_LEDS)
        leds[centerIdx] = CRGB(0, (uint8_t)(220.0f * lvl), 0);   // pulsing green impact
      FastLED.show();
      delay(8);
    }
  }
}

// Animate the comet from a -> b over `ms` with easing, bounded to [loIdx, hiIdx].
void sweep(float a, float b, uint16_t ms, EaseFn ease, const RefDot *refs, int n, int loIdx, int hiIdx) {
  if (ms == 0) return;
  uint32_t t0 = millis();
  while (millis() - t0 < ms && control != 0) {
    float ph = (millis() - t0) / (float)ms;
    drawCometRefs(a + (b - a) * ease(ph), refs, n, loIdx, hiIdx);
    delay(4);
  }
}

// Like sweep(), but shapes the drop-to-impact by the release amount (0..1) instead of a fixed ease.
void sweepDrop(float a, float b, uint16_t ms, float release01, const RefDot *refs, int n, int loIdx, int hiIdx) {
  if (ms == 0) return;
  uint32_t t0 = millis();
  while (millis() - t0 < ms && control != 0) {
    float ph = (millis() - t0) / (float)ms;
    drawCometRefs(a + (b - a) * dropEase(ph, release01), refs, n, loIdx, hiIdx);
    delay(4);
  }
}

// Hold the comet at a fixed position for ms (bounded, ref dots lit).
void holdAt(float pos, uint16_t ms, const RefDot *refs, int n, int loIdx, int hiIdx) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms && control != 0) { drawCometRefs(pos, refs, n, loIdx, hiIdx); delay(20); }
}

// ---------- PUTT stroke ----------
void runStroke() {
  Profile p = profiles[mode];                  // snapshot so mid-rep BLE writes don't tear
  float amp = p.reachLeds;
  int centerIdx = (int)roundf(CENTER);
  int targetIdx = (int)roundf(CENTER + amp);
  int followIdx = (int)roundf(CENTER - amp);

  // Get-ready countdown: N smooth green-impact pulses with the aim dots lit.
  RefDot grefs[] = { { centerIdx, BALL_COLOR }, { targetIdx, TARGET_COLOR } };
  countdownPulses(centerIdx, grefs, 2, (int)roundf(getReadyMs / 1000.0f));

  // Backswing: center -> target, bounded to [impact, amber] (emerges from the ball, parks at amber).
  uint32_t t0 = millis();
  while (millis() - t0 < p.backswingMs && control != 0) {
    float ph = (millis() - t0) / (float)p.backswingMs;
    float off = amp * sinf(ph * PI / 2.0f);              // fast at center, slow at top
    drawComet(CENTER + off, centerIdx, targetIdx, true, centerIdx, targetIdx);
    delay(4);
  }

  // Drop to impact: top -> center, flowing THROUGH impact (bounded to the envelope).
  float rel = p.release / 100.0f;
  t0 = millis();
  while (millis() - t0 < p.downswingMs && control != 0) {
    float ph = (millis() - t0) / (float)p.downswingMs;
    float off = amp * (1.0f - dropEase(ph, rel));        // +amp -> 0 (impact, fastest)
    drawComet(CENTER + off, centerIdx, targetIdx, false, followIdx, targetIdx);
    delay(4);
  }

  // Follow-through: center -> -amp, bounded to the envelope.
  t0 = millis();
  while (millis() - t0 < p.followMs && control != 0) {
    float ph = (millis() - t0) / (float)p.followMs;
    float off = -amp * easeOut(ph);                      // 0 -> -amp (decel)
    drawComet(CENTER + off, centerIdx, targetIdx, false, followIdx, targetIdx);
    delay(4);
  }

  // Hold at the finish (follow-end, or impact/center if followMs == 0), then clear.
  float finishOff = (p.followMs > 0) ? -amp : 0.0f;
  t0 = millis();
  while (millis() - t0 < holdMs && control != 0) {
    drawComet(CENTER + finishOff, centerIdx, targetIdx, false, followIdx, targetIdx);
    delay(20);
  }
  fadeToBlackBy(leds, NUM_LEDS, 60); FastLED.show();
}

// ---------- FULL swing (arc projected onto the strip) ----------
void runStrokeFull() {
  Profile p = profiles[mode];                  // snapshot
  float amp  = p.reachLeds;                     // amber (top of backswing) offset from center

  float center    = CENTER;
  float backRed   = NUM_LEDS - 1;               // near end (strip end) = max width
  float backAmber = CENTER + amp;
  float folRed    = 0;                          // far end (strip end)

  int centerIdx    = (int)roundf(center);
  int backRedIdx   = (int)roundf(backRed);      // NUM_LEDS-1
  int folRedIdx    = (int)roundf(folRed);       // 0
  int backAmberIdx = (int)roundf(backAmber);

  const RefDot refs[] = {
    { centerIdx,    BALL_COLOR   },   // [0] ball / impact
    { backRedIdx,   RED_COLOR    },   // red: backswing max width
    { folRedIdx,    RED_COLOR    },   // red: follow-through end
    { backAmberIdx, TARGET_COLOR },   // amber drawn last so it stays visible at max reach
  };
  const int NREF = 4;

  // Purple weight-shift dot (full-swing aid): appears at a chosen moment and stays lit
  // until the next rep. Off during get-ready.
  bool aim2On  = p.aim2Enabled != 0;
  int  aim2Idx = constrain((int)p.aim2Led, 1, NUM_LEDS - 2);
  aim2Now = -1;

  // Get-ready countdown: N smooth green-impact pulses with the full dot map lit for aiming.
  countdownPulses(centerIdx, refs, NREF, (int)roundf(getReadyMs / 1000.0f));
  if (aim2On && p.aim2When == 0) aim2Now = aim2Idx;   // 0 = from address (lit before the takeaway)

  // Split backswing/downswing time across their sub-segments by distance (float math
  // avoids uint16 overflow). dOut = center<->red; dIn = red<->amber.
  float dOut = backRed - center;                // center -> backRed
  float dIn  = backRed - backAmber;             // backRed -> backAmber
  uint16_t bOut = (uint16_t)(p.backswingMs * (dOut / (dOut + dIn)));
  uint16_t bIn  = p.backswingMs - bOut;
  uint16_t dBack = (uint16_t)(p.downswingMs * (dIn / (dIn + dOut)));   // backAmber -> backRed
  uint16_t dDown = p.downswingMs - dBack;                              // backRed -> center

  // Backswing: out to red, bounded [impact, red]; then up to amber, bounded [amber, red].
  sweep(center,    backRed,   bOut, easeInOut, refs, NREF, centerIdx,    backRedIdx);
  sweep(backRed,   backAmber, bIn,  easeInOut, refs, NREF, backAmberIdx, backRedIdx);
  if (aim2On && p.aim2When == 1) aim2Now = aim2Idx;   // 1 = top of backswing (before the pause)
  holdAt(backAmber, p.topHoldMs, refs, NREF, backAmberIdx, backRedIdx);     // pause at the top

  // Downswing: amber -> red, bounded [amber, red]; then red -> center flowing THROUGH impact.
  if (aim2On && p.aim2When == 2) aim2Now = aim2Idx;   // 2 = start of downswing
  sweep(backAmber, backRed,   dBack, easeInOut, refs, NREF, backAmberIdx, backRedIdx);
  sweepDrop(backRed, center,  dDown, p.release / 100.0f, refs, NREF, folRedIdx, backRedIdx);

  // Follow-through: center -> far end (red), bounded to the strip.
  if (aim2On && p.aim2When == 3) aim2Now = aim2Idx;   // 3 = impact
  sweep(center,    folRed,    p.followMs, easeOut, refs, NREF, folRedIdx, backRedIdx);
  holdAt(p.followMs > 0 ? folRed : center, holdMs, refs, NREF, folRedIdx, backRedIdx);

  aim2Now = -1;   // reset for the next rep
  fadeToBlackBy(leds, NUM_LEDS, 60); FastLED.show();
}

// Run one stroke in the active mode.
void runActiveStroke() {
  if (mode == MODE_FULL) runStrokeFull();
  else                   runStroke();
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
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(brightness);
  FastLED.clear(true);
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
  FastLED.clear();
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
  drawCometAnalytic(positionAt(u, p), strokeDirAt(u, p), 0, NUM_LEDS - 1);
  FastLED.show();
}

// Cycle epoch for the non-blocking render loop (esp_timer µs at the current cycle's u=0).
static int64_t cycleEpochUs = 0;
static uint8_t  renderPrevControl = 0xFF;

void loop() {
  // Debounced persist, GATED TO IDLE so a flash cache-stall never lands mid-stroke (plan §3.5).
  // Followers hold live params in RAM and never commit (plan §3.5), so gate on standalone/leader.
  if (paramsDirty && control == 0 && syncRole != ROLE_FOLLOWER && millis() - lastWriteMs > 1500) {
    paramsDirty = false; saveParams();
  }

  leaderBeaconTick();        // leader: broadcast the SyncBeacon at 4 Hz (no-op otherwise)
  syncServiceTimeouts();     // follower: orphan-timeout revert to standalone (no-op otherwise)

  // FOLLOWER: render phase-locked off the shared clock (§3.4) — the stateless renderer makes
  // "agree on u ⇒ identical frame" true, so the follower just needs the shared u.
  if (syncRole == ROLE_FOLLOWER) {
    followerHelloTick();                             // ~1 Hz hello so the leader counts/health-tracks us (§6)
    float u = syncFollowerU();
    applyFollowerParamsAtRender(u);                  // §7: cosmetic immediate, kinematic at the boundary
    if (followerControl == 1 || followerControl == 2) {
      if (u < cycleDuration(profiles[mode])) renderFrameAt(gridQuantize(u));   // shared frame grid (§3.4)
      else { FastLED.clear(); FastLED.show(); }       // rest/idle gap
    } else {
      fadeToBlackBy(leds, NUM_LEDS, 40);
      leds[(int)roundf(CENTER)] = CRGB(0, 40, 0);
      FastLED.show();
    }
    delay(4);
    return;
  }

  uint8_t c = control;
  if (c == 1 || c == 2) {               // loop / single — NON-BLOCKING, stateless analytic render
    int64_t now = esp_timer_get_time();
    if (renderPrevControl != c) { cycleEpochUs = now; renderPrevControl = c; }   // (re)start on control change
    if (syncRole == ROLE_LEADER) leaderEpochUs = cycleEpochUs;   // beacon carries the render epoch (shared grid)
    uint32_t cd = cycleDuration(profiles[mode]);
    uint32_t total = cd + (c == 1 ? restMs : 0);           // loop adds the dark rest gap; single does not
    float u = (now - cycleEpochUs) / 1000.0f;              // ms since this cycle's u=0
    if (u >= total) {
      if (c == 1) { cycleEpochUs = now; renderFrameAt(0); }               // next rep (per-cycle epoch re-arm)
      else { control = 0; renderPrevControl = 0xFF; notifyControl(); }    // single done → idle
    } else if (u >= cd) {
      FastLED.clear(); FastLED.show();                     // dark rest gap (loop only)
    } else {
      renderFrameAt(gridQuantize(u));                      // shared frame grid (§3.4)
    }
    delay(4);                           // ~250 Hz service; the grid-paced core-1 render task lands with sync (plan §3.5)
  } else if (c == 3) {                  // preview: live dot map for the active mode
    renderPrevControl = 0xFF;
    float amp = profiles[mode].reachLeds;
    if (mode == MODE_FULL) {
      // Include the purple dot when enabled so it can be positioned live in preview.
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
    delay(30);
  } else {                              // idle: dim center marker so you can line up
    renderPrevControl = 0xFF;
    fadeToBlackBy(leds, NUM_LEDS, 40);
    leds[(int)roundf(CENTER)] = CRGB(0, 40, 0);
    FastLED.show();
    delay(20);
  }
}
