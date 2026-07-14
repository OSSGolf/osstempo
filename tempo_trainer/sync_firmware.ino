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
static int64_t clockOffsetUs = 0;               // add to local esp_timer to get leader time
static int64_t followerEpochUs = 0;             // shared epoch in LEADER time (from the beacon)
uint8_t        followerControl = 0;             // extern (header)
Profile        followerParams;                  // extern (header)
static int64_t lastBeaconRxUs = 0;
static uint32_t followerLastSeq = 0;            // replay guard (monotonic seq, §4.2.1)
static uint8_t  beaconBrightness = 128;         // latest cosmetic globals from the beacon (§7)
static uint8_t  beaconRails = 1;
static bool     followerKinInit = false;        // has the follower merged kinematic params at least once?
static int64_t  lastPingTxUs = 0;               // round-trip OWD: our last ping's tx timestamp
static uint32_t lastPingMs = 0;

// ---- Follower health as the leader tracks it (for the …0012 report), §6/§12 ----
struct FollowerRec { uint8_t mac[6]; int64_t lastHelloUs; bool settled; };
static const int MAX_FOLLOWERS = 4;
static FollowerRec followers[MAX_FOLLOWERS];
static int followerN = 0;

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
  for (int i = 0; i < followerN; i++) {
    if (memcmp(followers[i].mac, mac, 6) == 0) {
      followers[i].lastHelloUs = esp_timer_get_time();
      followers[i].settled = settled;                     // §13-ask-5: relay the follower's clock-locked flag
      return;
    }
  }
  if (followerN < MAX_FOLLOWERS) {
    memcpy(followers[followerN].mac, mac, 6);
    followers[followerN].lastHelloUs = esp_timer_get_time();
    followers[followerN].settled = settled;
    followerN++;
    notifyFollowerStatus();                                // …0012 count rose → the app confirms the join (§12)
  }
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
  // Round-trip OWD pong: refine the clock offset with the NTP four-timestamp formula (§4.3).
  if (syncRole == ROLE_FOLLOWER && len >= 26 && data[0] == 'P' && data[1] == 'O') {
    int64_t followerTx, leaderRx, leaderTx, followerRx = esp_timer_get_time();
    memcpy(&followerTx, data + 2, 8);
    memcpy(&leaderRx, data + 10, 8);
    memcpy(&leaderTx, data + 18, 8);
    if (followerTx == lastPingTxUs) {
      clockOffsetUs = ((leaderRx - followerTx) + (leaderTx - followerRx)) / 2;   // removes the one-way bias
    }
    return;
  }
  // Addressed identify from the leader: blink only if the target MAC is ours (§13-ask-2).
  if (syncRole == ROLE_FOLLOWER && len >= 8 && data[0] == 'I' && data[1] == 'D') {
    if (memcmp(data + 2, myMac, 6) == 0) identifyBlink();
    return;
  }
  if (syncRole != ROLE_FOLLOWER || len < (int)sizeof(SyncBeacon)) return;
  SyncBeacon b; memcpy(&b, data, sizeof(b));
  if (b.magic[0] != 'O' || b.magic[1] != 'T' || b.sessionId != sessionId) return;
  uint8_t tag[8]; computeAuthTag(b, tag);                 // spoof/replay: drop any beacon whose MAC fails (§4.2.1)
  if (memcmp(tag, b.authTag, 8) != 0) return;
  if (b.seq && b.seq < followerLastSeq && (followerLastSeq - b.seq) < 0x40000000UL) return;  // replay (stale seq)
  followerLastSeq = b.seq;
  if (b.flags & BEACON_FLAG_DISBAND) { syncRevertToStandalone(); return; }

  int64_t rx = esp_timer_get_time();
  offsetSamples[offsetIdx] = (b.txEspTimer) - rx;         // OWD calibration refines this (§4.3)
  offsetIdx = (offsetIdx + 1) % OFFSET_WINDOW;
  if (offsetCount < OFFSET_WINDOW) offsetCount++;
  clockOffsetUs   = minFilteredOffset();
  followerEpochUs = b.epochEspTimer;
  followerControl = b.control;
  followerParams  = b.active;                            // kinematic staged; merged at boundary (§7)
  beaconBrightness = b.brightness;                       // cosmetic — applied immediately in the render loop
  beaconRails      = b.railsLit;
  lastBeaconRxUs  = rx;
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

// §7 follower ingestion: cosmetic fields apply IMMEDIATELY; kinematic fields merge only at a cycle
// boundary (u wrap) so a mid-cycle kinematic change can't jump the comet's phase. Runs on core 1.
void applyFollowerParamsAtRender(float u) {
  if (brightness != beaconBrightness) { brightness = beaconBrightness; FastLED.setBrightness(brightness); }
  railsLit = beaconRails;
  profiles[mode].aim2Enabled = followerParams.aim2Enabled;     // aim2 position/enable are cosmetic
  profiles[mode].aim2Led     = followerParams.aim2Led;
  static float lastU = 1e9f;
  if (!followerKinInit || u < lastU) {                         // first frame after join, or a cycle wrap
    profiles[mode].backswingMs = followerParams.backswingMs;
    profiles[mode].downswingMs = followerParams.downswingMs;
    profiles[mode].reachLeds   = followerParams.reachLeds;
    profiles[mode].topHoldMs   = followerParams.topHoldMs;
    profiles[mode].release     = followerParams.release;
    profiles[mode].followMs    = followerParams.followMs;
    profiles[mode].aim2When    = followerParams.aim2When;      // aim2When is kinematic (timeline)
    followerKinInit = true;
  }
  lastU = u;
}

// Shared cycle-time u (ms) for a follower: (localNow + offset) − sharedEpoch. Stateless (§3.4).
float syncFollowerU() {
  int64_t sharedNow = esp_timer_get_time() + clockOffsetUs;
  int64_t du = sharedNow - followerEpochUs;
  return (du < 0 ? 0.0f : du / 1000.0f);
}

// =====================================================================================
//  Leader: broadcast the beacon on the shared grid.
// =====================================================================================
static void fillBeacon(SyncBeacon &b, uint8_t flags) {
  b.magic[0] = 'O'; b.magic[1] = 'T'; b.version = 1;
  b.sessionId = sessionId; b.flags = flags;
  b.seq = ++beaconSeq;
  b.txEspTimer = esp_timer_get_time();
  b.control = control;
  b.epochEspTimer = leaderEpochUs;
  b.active = profiles[mode];
  b.brightness = brightness; b.railsLit = railsLit;      // cosmetic globals (§7)
  memset(b.authTag, 0, sizeof(b.authTag));
  computeAuthTag(b, b.authTag);                          // keyed MAC over all preceding bytes (§4.2.1)
}

void leaderBeaconTick() {
  if (syncRole != ROLE_LEADER || !wifiUp) return;
  uint32_t nowMs = millis();
  if (nowMs - lastBeaconMs < BEACON_KEEPALIVE_MS) return;
  lastBeaconMs = nowMs;
  SyncBeacon b; fillBeacon(b, 0);
  esp_now_send(BROADCAST_ADDR, (const uint8_t *)&b, sizeof(b));
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
  offsetCount = 0; offsetIdx = 0; clockOffsetUs = 0;
  followerKinInit = false;                              // re-merge kinematic params on the first synced frame
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
  followerN = 0;
}

// Two-threshold never-stuck revert (§6/§8): a follower that loses beacons past T_ORPHAN
// reverts to standalone (re-enables BLE). Called from loop().
static const int64_t T_ORPHAN_US = 20 * 1000000LL;       // a few s beyond DEGRADE_S (bench-tuned)
static const int64_t T_HELLO_US   = 20 * 1000000LL;      // ≥ T_ORPHAN so leader teardown ≥ follower revert (§6)
static const int64_t T_JOIN_BACKSTOP_US = 8 * 1000000LL; // leader-with-zero-followers self-revert (§13-ask-6)
void syncServiceTimeouts() {
  int64_t now = esp_timer_get_time();
  if (syncRole == ROLE_FOLLOWER && now - lastBeaconRxUs > T_ORPHAN_US) {
    syncRevertToStandalone();
    return;
  }
  if (syncRole == ROLE_LEADER) {
    // Prune followers whose hello timed out (they orphaned) and re-report the count (§6/§12).
    int before = followerN;
    for (int i = 0; i < followerN; ) {
      if (now - followers[i].lastHelloUs > T_HELLO_US) {
        for (int j = i; j < followerN - 1; j++) followers[j] = followers[j + 1];
        followerN--;
      } else i++;
    }
    if (followerN != before) notifyFollowerStatus();
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
int syncFollowerStatus(uint8_t *out, int cap) {
  if (cap < 1) return 0;
  int n = 0; out[n++] = (uint8_t)followerN;
  for (int i = 0; i < followerN && n + 7 <= cap; i++) {
    memcpy(out + n, followers[i].mac, 6); n += 6;
    int64_t age = esp_timer_get_time() - followers[i].lastHelloUs;
    uint8_t flags = 0;
    if (age > 15 * 1000000LL) flags |= 0x01;             // re-linking (stale age, §13-ask-1)
    if (followers[i].settled)  flags |= 0x02;            // clock locked (§13-ask-5)
    out[n++] = flags;
  }
  return n;
}
