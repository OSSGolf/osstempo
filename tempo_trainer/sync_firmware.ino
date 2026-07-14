/*
 * sync_firmware.ino — ESP-NOW multi-stick sync (FW Phase 2–5, TEMPO_SYNC_PLAN.md).
 * =================================================================================
 * Additive sketch tab. Ephemeral, app-brokered roles held in RAM, default `standalone`
 * at boot (§2.1): WiFi/ESP-NOW is OFF until the app sends becomeLeader/joinSession over
 * BLE, so the lone-stick path pays ZERO coex/RAM cost. A leader broadcasts a SyncBeacon;
 * a follower renders phase-locked off the shared, epoch-anchored clock (§3.4/§4).
 *
 * Compile-verified on both boards; runtime skew/coex is bench-gated (Phase 0 GO required).
 * The shared-grid render integration + core-affinity task are staged (see syncFollowerU()).
 */

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <cstddef>            // offsetof (authTag MAC)
#include <mbedtls/md.h>       // HMAC-SHA256 for the beacon authTag (§4.2.1)
#include "sync_firmware.h"    // SyncRole enum + externs for the state the main tab reads

// ---- Ephemeral role (RAM only; default standalone at every boot). Non-static: matches the
//      extern declarations in sync_firmware.h so the main tab can read them. ----
volatile SyncRole syncRole = ROLE_STANDALONE;
static bool     wifiUp = false;
static uint8_t  chOp = 6;                       // operating channel handed by the app (§5.1)
static uint8_t  sessionKey[16] = {0};           // per-session key (RAM; authenticates the beacon, §4.2.1)
static uint16_t sessionId = 0;

static uint8_t BROADCAST_ADDR[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static uint8_t myMac[6] = {0};                  // this stick's STA MAC (follower identity for identify)

// (SyncBeacon + BEACON_FLAG_DISBAND are declared in sync_firmware.h so the main tab sees them.)

// ---- Leader beacon state ----
static uint32_t beaconSeq = 0;
int64_t         leaderEpochUs = 0;              // shared cycle epoch (leader clock) — extern (header)
static uint32_t lastBeaconMs = 0;
static const uint32_t BEACON_KEEPALIVE_MS = 250;   // 4 Hz (§4.4)
static int64_t  leaderZeroSinceUs = 0;             // when the leader last had 0 followers (self-revert backstop)

// ---- Follower clock-offset estimate (min-filter over the last N samples, §4.3) ----
static const int OFFSET_WINDOW = 16;
static int64_t offsetSamples[OFFSET_WINDOW];
static int     offsetCount = 0, offsetIdx = 0;
static int64_t clockOffsetUs = 0;               // best clock offset (round-trip θ when available)
static int64_t owdUs = 0;                       // one-way delay from the round-trip pong (§4.3)
static bool    haveRtt = false;                 // do we have a round-trip offset yet?
static int64_t lastBeaconRxUs = 0;
static uint32_t followerLastSeq = 0;            // replay guard (monotonic seq, §4.2.1)
static bool    followerEverHeard = false;       // #8: T_JOIN fast-revert if we never heard a beacon
static int64_t followerJoinUs = 0;              // when we became a follower (for the T_JOIN timeout)
static int64_t lastPingTxUs = 0;                // round-trip OWD: our last ping's tx timestamp
static uint32_t lastPingMs = 0;

// Deferred actions: SET in the RX callback (core 0), PERFORMED in loop() (core 1, the radio owner).
static volatile bool pendingRevert = false;     // #1: WiFi teardown must NOT run in the RX callback
static volatile bool identifyPending = false;   // #5: identifyBlink() (720ms) must NOT run in the callback

// Follower render state, published via a SEQLOCK: single writer (core-0 RX cb), single reader
// (core-1 loop). Prevents torn reads of the 64-bit clock + the ~24-byte kinematic set (plan §3.5).
// (FollowerPub is declared in sync_firmware.h so the auto-prototypes resolve.)
static volatile uint32_t pubSeq = 0;
static FollowerPub pub = {};
static inline void publishFollower(const FollowerPub &v) {
  uint32_t s = pubSeq; pubSeq = s + 1;                   // odd → writing (explicit store: no volatile-++ warning)
  __sync_synchronize(); pub = v; __sync_synchronize();
  pubSeq = s + 2;                                        // even → published
}
static inline bool readFollower(FollowerPub &out) {
  for (int t = 0; t < 5; t++) {
    uint32_t s0 = pubSeq; __sync_synchronize();
    if (s0 & 1) continue;
    out = pub; __sync_synchronize();
    if (pubSeq == s0) return true;
  }
  return false;
}

// ---- Follower health as the leader tracks it (for the …0012 report), §6/§12 ----
struct FollowerRec { uint8_t mac[6]; int64_t lastHelloUs; bool settled; };
static const int MAX_FOLLOWERS = 4;
static FollowerRec followers[MAX_FOLLOWERS];
static int followerN = 0;
static portMUX_TYPE followersMux = portMUX_INITIALIZER_UNLOCKED;   // #7: core-0 add vs core-1 prune

// =====================================================================================
//  Radio bring-up / tear-down (paid ONLY while a set is active).
// =====================================================================================
static void syncWifiUp() {
  if (wifiUp) return;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(chOp, WIFI_SECOND_CHAN_NONE);
  esp_wifi_get_mac(WIFI_IF_STA, myMac);                 // our identity for addressed identify (§13-ask-2)
  if (esp_now_init() == ESP_OK) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
    peer.channel = chOp; peer.encrypt = false;
    esp_now_add_peer(&peer);
    wifiUp = true;
  }
}

// Leader → a specific follower: broadcast an addressed identify; only the matching MAC blinks (§13-ask-2).
void syncIdentifyFollower(const uint8_t mac[6]) {
  if (syncRole != ROLE_LEADER || !wifiUp) return;
  uint8_t pkt[8] = { 'I', 'D', mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] };
  esp_now_send(BROADCAST_ADDR, pkt, sizeof(pkt));
}

static void syncWifiDown() {
  if (!wifiUp) return;
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  wifiUp = false;
}

// =====================================================================================
//  Follower: receive + estimate the shared clock.
// =====================================================================================
static int64_t minFilteredOffset() {
  int64_t m = offsetSamples[0];
  for (int i = 1; i < offsetCount; i++) if (offsetSamples[i] < m) m = offsetSamples[i];
  return m;
}

// Push the …0012 follower-status blob to the app (the set-health channel, §12).
void notifyFollowerStatus() {
  if (!chFollowerStat) return;
  uint8_t buf[1 + MAX_FOLLOWERS * 7];
  int n = syncFollowerStatus(buf, sizeof(buf));
  chFollowerStat->setValue(buf, n);
  chFollowerStat->notify();
}

// Leader-side: track a follower by its source MAC (any packet on our session is a "hello", §6).
static void leaderTrackFollower(const uint8_t *mac, bool settled) {
  bool added = false;
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&followersMux);                      // #7: guard the shared table vs the core-1 prune
  int i;
  for (i = 0; i < followerN; i++) {
    if (memcmp(followers[i].mac, mac, 6) == 0) { followers[i].lastHelloUs = now; followers[i].settled = settled; break; }
  }
  if (i == followerN && followerN < MAX_FOLLOWERS) {
    memcpy(followers[followerN].mac, mac, 6);
    followers[followerN].lastHelloUs = now;
    followers[followerN].settled = settled;
    followerN++; added = true;
  }
  portEXIT_CRITICAL(&followersMux);
  if (added) notifyFollowerStatus();                       // BLE notify OUTSIDE the critical section (§12)
}

// HMAC-SHA256 over the beacon (all bytes before authTag), keyed by the per-session key, truncated to
// 8 bytes — spoof/replay rejection (§4.2.1). ESP32 mbedtls uses the hardware SHA accelerator.
static void computeAuthTag(const SyncBeacon &b, uint8_t out[8]) {
  uint8_t full[32] = {0};
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info && mbedtls_md_hmac(info, sessionKey, 16, (const uint8_t *)&b,
                              offsetof(SyncBeacon, authTag), full) == 0) {
    memcpy(out, full, 8);
  } else {
    memset(out, 0, 8);
  }
}

// Single recv callback for both roles: a leader hears follower hellos; a follower hears beacons.
static void onSyncRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < 3) return;
  if (syncRole == ROLE_LEADER) {
    if (data[0] == 'H' && data[1] == 'L') { leaderTrackFollower(info->src_addr, data[2] != 0); return; }
    if (data[0] == 'P' && data[1] == 'G' && len >= 10) {          // round-trip OWD ping → echo a pong (§4.3)
      int64_t leaderRx = esp_timer_get_time(), followerTx;
      memcpy(&followerTx, data + 2, 8);
      int64_t leaderTx = esp_timer_get_time();
      uint8_t pong[26] = { 'P', 'O' };
      memcpy(pong + 2, &followerTx, 8);
      memcpy(pong + 10, &leaderRx, 8);
      memcpy(pong + 18, &leaderTx, 8);
      esp_now_send(BROADCAST_ADDR, pong, sizeof(pong));
    }
    return;
  }
  // Round-trip OWD pong: NTP four-timestamp offset θ + one-way delay δ/2 (§4.3). #3.
  if (syncRole == ROLE_FOLLOWER && len >= 26 && data[0] == 'P' && data[1] == 'O') {
    int64_t followerTx, leaderRx, leaderTx, followerRx = esp_timer_get_time();
    memcpy(&followerTx, data + 2, 8);
    memcpy(&leaderRx, data + 10, 8);
    memcpy(&leaderTx, data + 18, 8);
    if (followerTx == lastPingTxUs) {
      clockOffsetUs = ((leaderRx - followerTx) + (leaderTx - followerRx)) / 2;   // offset θ (bias-cancelled)
      owdUs         = ((followerRx - followerTx) - (leaderTx - leaderRx)) / 2;   // one-way delay
      haveRtt = true;
    }
    return;
  }
  // Addressed identify: blink only if the target MAC is ours — DEFERRED to loop (720 ms blocking). #5.
  if (syncRole == ROLE_FOLLOWER && len >= 8 && data[0] == 'I' && data[1] == 'D') {
    if (memcmp(data + 2, myMac, 6) == 0) identifyPending = true;
    return;
  }
  if (syncRole != ROLE_FOLLOWER || len < (int)sizeof(SyncBeacon)) return;
  SyncBeacon b; memcpy(&b, data, sizeof(b));
  if (b.magic[0] != 'O' || b.magic[1] != 'T' || b.sessionId != sessionId) return;
  uint8_t tag[8]; computeAuthTag(b, tag);                 // spoof/replay: drop any beacon whose MAC fails (§4.2.1)
  if (memcmp(tag, b.authTag, 8) != 0) return;
  if (b.seq && b.seq < followerLastSeq && (followerLastSeq - b.seq) < 256) return;   // #4: small replay window
  followerLastSeq = b.seq;
  if (b.flags & BEACON_FLAG_DISBAND) { pendingRevert = true; return; }   // #1: DEFER teardown out of the callback

  int64_t rx = esp_timer_get_time();
  offsetSamples[offsetIdx] = b.txEspTimer + owdUs - rx;   // #3: sample = tx + OWD − rx (§4.3)
  offsetIdx = (offsetIdx + 1) % OFFSET_WINDOW;
  if (offsetCount < OFFSET_WINDOW) offsetCount++;

  FollowerPub v;                                          // #6: publish the whole delta atomically (seqlock)
  v.epochUs    = b.epochEspTimer;
  v.offsetUs   = haveRtt ? clockOffsetUs : minFilteredOffset();   // #3: round-trip wins; min-filter is fallback
  v.params     = b.active;
  v.control    = b.control;
  v.mode       = b.mode;                                  // #2: adopt the leader's mode/getReady/hold/rest
  v.getReadyMs = b.getReadyMs; v.holdMs = b.holdMs; v.restMs = b.restMs;
  v.brightness = b.brightness; v.railsLit = b.railsLit;
  publishFollower(v);
  lastBeaconRxUs = rx;
  followerEverHeard = true;
}

// Follower: send a ~1 Hz hello so the leader can count/health-track it. Called from loop().
static uint32_t lastHelloMs = 0;
void followerHelloTick() {
  if (syncRole != ROLE_FOLLOWER || !wifiUp) return;
  uint32_t nowMs = millis();
  if (nowMs - lastHelloMs < 1000) return;
  lastHelloMs = nowMs;
  uint8_t settled = (offsetCount >= OFFSET_WINDOW) ? 1 : 0;   // clock-locked once the min-filter fills (§13-ask-5)
  uint8_t hello[3] = { 'H', 'L', settled };
  esp_now_send(BROADCAST_ADDR, hello, sizeof(hello));
}

// Follower: ~0.5 Hz round-trip ping so the leader can echo timestamps for OWD calibration (§4.3).
void followerPingTick() {
  if (syncRole != ROLE_FOLLOWER || !wifiUp) return;
  uint32_t nowMs = millis();
  if (nowMs - lastPingMs < 2000) return;
  lastPingMs = nowMs;
  lastPingTxUs = esp_timer_get_time();
  uint8_t pkt[10] = { 'P', 'G' };
  memcpy(pkt + 2, &lastPingTxUs, 8);
  esp_now_send(BROADCAST_ADDR, pkt, sizeof(pkt));
}

// §7 follower ingestion: cosmetic (brightness/rails/aim2) applies IMMEDIATELY; kinematic (mode,
// get-ready/hold/rest, and the Profile timeline) merges only at a cycle boundary (u wrap) so a
// mid-cycle change can't jump the comet's phase. Reads the seqlock snapshot; runs on core 1.
static float followerLastU = 1e9f;
static bool  followerKinInit = false;
static void applyFollowerParamsFrom(const FollowerPub &fp, float u) {
  if (brightness != fp.brightness) { brightness = fp.brightness; FastLED.setBrightness(brightness); }
  railsLit = fp.railsLit;
  if (!followerKinInit || u < followerLastU) {                 // first synced frame, or a cycle wrap
    mode = fp.mode; getReadyMs = fp.getReadyMs; holdMs = fp.holdMs; restMs = fp.restMs;
    profiles[mode] = fp.params;                                // whole timeline (kinematic + aim2) at boundary
    followerKinInit = true;
  } else {
    profiles[mode].aim2Enabled = fp.params.aim2Enabled;        // aim2 position/enable are cosmetic → immediate
    profiles[mode].aim2Led     = fp.params.aim2Led;
  }
  followerLastU = u;
}

// Perform actions deferred out of the RX callback, on core 1 (the radio owner). #1/#5.
void syncDrainDeferred() {
  if (pendingRevert)   { pendingRevert = false;   syncRevertToStandalone(); }
  if (identifyPending) { identifyPending = false; identifyBlink(); }
}

// One follower render tick: send hello/ping, snapshot the shared state (seqlock), render off the
// shared clock. Called from loop() (core 1). Stateless renderer ⇒ agree on u ⇒ identical frame (§3.4).
void followerRenderStep() {
  followerHelloTick();
  followerPingTick();
  FollowerPub fp;
  if (!readFollower(fp)) return;                               // torn read this tick — retry next
  int64_t sharedNow = esp_timer_get_time() + fp.offsetUs;
  float u = (sharedNow < fp.epochUs) ? 0.0f : (sharedNow - fp.epochUs) / 1000.0f;
  applyFollowerParamsFrom(fp, u);
  if (fp.control == 1 || fp.control == 2) {
    if (u < cycleDuration(profiles[mode])) renderFrameAt(gridQuantize(u));   // shared frame grid (§3.4)
    else { FastLED.clear(); FastLED.show(); }                 // rest/idle gap
  } else {
    fadeToBlackBy(leds, NUM_LEDS, 40);
    leds[(int)roundf(CENTER)] = CRGB(0, 40, 0);
    FastLED.show();
  }
}

// =====================================================================================
//  Leader: broadcast the beacon on the shared grid.
// =====================================================================================
static void fillBeacon(SyncBeacon &b, uint8_t flags) {
  memset(&b, 0, sizeof(b));                              // zero Profile padding → deterministic HMAC (#12)
  b.magic[0] = 'O'; b.magic[1] = 'T'; b.version = 1;
  b.sessionId = sessionId; b.flags = flags;
  b.seq = ++beaconSeq;
  b.txEspTimer = esp_timer_get_time();
  b.control = control;
  b.epochEspTimer = leaderEpochUs;
  b.active = profiles[mode];
  b.mode = mode; b.getReadyMs = getReadyMs; b.holdMs = holdMs; b.restMs = restMs;   // KINEMATIC globals (§7)
  b.brightness = brightness; b.railsLit = railsLit;      // cosmetic globals (§7)
  computeAuthTag(b, b.authTag);                          // keyed MAC over all preceding bytes (§4.2.1)
}

static volatile bool leaderEventPending = false;         // #9: control/param/boundary change → burst now
void leaderMarkEvent() { leaderEventPending = true; }     // set from any context; sent from loop (core 1)

void leaderBeaconTick() {
  if (syncRole != ROLE_LEADER || !wifiUp) return;
  uint32_t nowMs = millis();
  bool event = leaderEventPending;
  if (event) leaderEventPending = false;
  if (!event && nowMs - lastBeaconMs < BEACON_KEEPALIVE_MS) return;   // else keepalive throttle
  lastBeaconMs = nowMs;
  int repeats = event ? 4 : 1;                            // event beacons repeated 4× over a few hundred µs (§4.4)
  for (int i = 0; i < repeats; i++) {
    SyncBeacon b; fillBeacon(b, 0);                       // fresh seq/tx each repeat (feeds the offset filter)
    esp_now_send(BROADCAST_ADDR, (const uint8_t *)&b, sizeof(b));
    if (i + 1 < repeats) delayMicroseconds(800);
  }
}

// =====================================================================================
//  App-brokered lifecycle — called by the …0013 BLE session-command handler.
// =====================================================================================
void syncBecomeLeader(uint16_t sid, const uint8_t key[16], uint8_t channel) {
  sessionId = sid; memcpy(sessionKey, key, 16); chOp = channel;
  syncRole = ROLE_LEADER;
  leaderEpochUs = esp_timer_get_time();
  followerN = 0;
  leaderZeroSinceUs = esp_timer_get_time();             // start the zero-follower backstop clock (§13-ask-6)
  syncWifiUp();
  esp_now_register_recv_cb(onSyncRecv);                 // also hears follower hellos (count/health)
}

void syncJoinSession(uint16_t sid, const uint8_t key[16], uint8_t channel) {
  sessionId = sid; memcpy(sessionKey, key, 16); chOp = channel;
  syncRole = ROLE_FOLLOWER;
  offsetCount = 0; offsetIdx = 0; clockOffsetUs = 0; owdUs = 0; haveRtt = false;
  followerLastSeq = 0;                                  // #4: don't carry a prior session's seq (reboot/rejoin)
  followerEverHeard = false; followerJoinUs = esp_timer_get_time();   // #8
  followerKinInit = false; followerLastU = 1e9f;        // re-merge kinematic params on the first synced frame
  lastBeaconRxUs = esp_timer_get_time();
  syncWifiUp();
  esp_now_register_recv_cb(onSyncRecv);
  // BLE is dropped by the app side after this write; the stick now renders off the beacon.
}

void syncDisband() {
  if (syncRole == ROLE_LEADER && wifiUp) {
    for (int i = 0; i < 5; i++) {                         // repeated DISBAND (§4.4/§6)
      SyncBeacon b; fillBeacon(b, BEACON_FLAG_DISBAND);
      esp_now_send(BROADCAST_ADDR, (const uint8_t *)&b, sizeof(b));
      delay(30);
    }
  }
  syncRevertToStandalone();
}

void syncRevertToStandalone() {
  syncWifiDown();
  syncRole = ROLE_STANDALONE;
  portENTER_CRITICAL(&followersMux); followerN = 0; portEXIT_CRITICAL(&followersMux);   // #7
}

// Two-threshold never-stuck revert (§6/§8): a follower that loses beacons past T_ORPHAN
// reverts to standalone (re-enables BLE). Called from loop().
static const int64_t T_ORPHAN_US = 20 * 1000000LL;       // a few s beyond DEGRADE_S (bench-tuned)
static const int64_t T_HELLO_US   = 20 * 1000000LL;      // ≥ T_ORPHAN so leader teardown ≥ follower revert (§6)
static const int64_t T_JOIN_BACKSTOP_US = 8 * 1000000LL; // leader-with-zero-followers self-revert (§13-ask-6)
static const int64_t T_JOIN_US    = 4 * 1000000LL;       // #8: never-heard-a-beacon fast revert (§6/§8)
void syncServiceTimeouts() {
  int64_t now = esp_timer_get_time();
  if (syncRole == ROLE_FOLLOWER) {
    // #8: never synced (app crashed between joinSession and becomeLeader) → fast revert, don't sit dark 20 s.
    if (!followerEverHeard && now - followerJoinUs > T_JOIN_US) { syncRevertToStandalone(); return; }
    if (now - lastBeaconRxUs > T_ORPHAN_US) { syncRevertToStandalone(); return; }   // was-synced → full coast
    return;
  }
  if (syncRole == ROLE_LEADER) {
    // Prune followers whose hello timed out (they orphaned) and re-report the count (§6/§12).
    int before = followerN;
    portENTER_CRITICAL(&followersMux);                    // #7: guard vs the core-0 add
    for (int i = 0; i < followerN; ) {
      if (now - followers[i].lastHelloUs > T_HELLO_US) {
        for (int j = i; j < followerN - 1; j++) followers[j] = followers[j + 1];
        followerN--;
      } else i++;
    }
    int after = followerN;
    portEXIT_CRITICAL(&followersMux);
    if (after != before) notifyFollowerStatus();          // BLE notify OUTSIDE the critical section
    // Zero-follower backstop: a leader that never gets (or loses all) followers reverts to standalone
    // so the app crashing mid-add can't strand a WiFi-up zombie leader (§13-ask-6 / plan §5.3-5).
    if (followerN > 0) leaderZeroSinceUs = 0;
    else {
      if (leaderZeroSinceUs == 0) leaderZeroSinceUs = now;
      else if (now - leaderZeroSinceUs > T_JOIN_BACKSTOP_US) syncRevertToStandalone();
    }
  }
}

// Serialize the …0012 follower-status blob: [count][ (handle:6 mac)(flags:1) ]* (§12).
// Snapshots the table under the lock (#7) — memcpy only, no BLE inside the critical section.
int syncFollowerStatus(uint8_t *out, int cap) {
  if (cap < 1) return 0;
  int64_t now = esp_timer_get_time();
  int n = 0;
  portENTER_CRITICAL(&followersMux);
  out[n++] = (uint8_t)followerN;
  for (int i = 0; i < followerN && n + 7 <= cap; i++) {
    memcpy(out + n, followers[i].mac, 6); n += 6;
    int64_t age = now - followers[i].lastHelloUs;
    uint8_t flags = 0;
    if (age > 15 * 1000000LL) flags |= 0x01;             // re-linking (stale age, §13-ask-1)
    if (followers[i].settled)  flags |= 0x02;            // clock locked (§13-ask-5)
    out[n++] = flags;
  }
  portEXIT_CRITICAL(&followersMux);
  return n;
}
