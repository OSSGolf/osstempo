// sync_firmware.h — shared interface between the main tab and sync_firmware.ino.
// Included from tempo_trainer.ino AFTER `Profile` is defined (a follower holds live Profile params).
// This exists because Arduino concatenates the main .ino first, so the main tab cannot otherwise
// see the sync module's types/state.
#pragma once

// Ephemeral role, RAM only, default standalone at boot (§2.1).
enum SyncRole : uint8_t { ROLE_STANDALONE = 0, ROLE_LEADER = 1, ROLE_FOLLOWER = 2 };

// SyncBeacon wire format (§4.2). Contains Profile, so this header is included after Profile.
struct __attribute__((packed)) SyncBeacon {
  uint8_t  magic[2];        // 'O','T'
  uint8_t  version;
  uint16_t sessionId;
  uint8_t  flags;           // bit0 DISBAND
  uint32_t seq;             // monotonic; resets on leader reboot
  int64_t  txEspTimer;      // leader µs captured just before send (§4.3)
  uint8_t  control;         // 0 idle / 1 loop / 2 single / 3 preview
  int64_t  epochEspTimer;   // leader-clock start of the CURRENT cycle
  Profile  active;          // authoritative params in effect this cycle (kinematic + aim2)
  uint8_t  mode;            // KINEMATIC globals the follower must adopt or it renders the wrong stroke:
  uint16_t getReadyMs;      //   mode / get-ready / hold / rest all feed positionAt/cycleDuration (§7)
  uint16_t holdMs;
  uint16_t restMs;
  uint8_t  brightness;      // cosmetic global — applied immediately by a follower (§7)
  uint8_t  railsLit;        // cosmetic global — applied immediately
  uint8_t  authTag[8];      // keyed MAC over preceding bytes (§4.2.1)
};
static const uint8_t BEACON_FLAG_DISBAND = 0x01;
// The wire format must be byte-identical across BOTH builds (dig2go ↔ generic ESP32) — the struct has
// no board-conditional fields, so it is; this guards the ESP-NOW payload limit and future edits.
static_assert(sizeof(SyncBeacon) <= 250, "SyncBeacon exceeds the ESP-NOW 250-byte payload limit");

// Follower render state published across cores via a seqlock (declared here so the auto-generated
// prototypes in the main tab can see the type). Contains Profile, so this header follows Profile.
struct FollowerPub {
  int64_t epochUs, offsetUs;
  Profile params;
  uint8_t control, mode, brightness, railsLit;
  uint16_t getReadyMs, holdMs, restMs;
};

extern volatile SyncRole syncRole;
extern int64_t           leaderEpochUs;     // shared cycle epoch (leader clock) carried in the beacon

// Called from the main tab (loop / BLE session-command handler / setup). The follower render state is
// now private to sync_firmware.ino behind a seqlock — the main loop just calls followerRenderStep().
void  syncBecomeLeader(uint16_t sid, const uint8_t key[16], uint8_t channel);
void  syncJoinSession(uint16_t sid, const uint8_t key[16], uint8_t channel);
void  syncDisband();
void  syncRevertToStandalone();
void  syncServiceTimeouts();
void  syncDrainDeferred();                    // perform RX-callback-deferred actions (revert/identify) on core 1
void  syncIdentifyFollower(const uint8_t mac[6]);
void  syncDeferIdentifySelf();                // queue THIS stick's blink for core 1 (never blink on the BLE task)
void  syncInvalidateFollowerFrameSkip();      // force the follower render path to repaint next tick
void  leaderBeaconTick();
void  leaderMarkEvent();                      // #9: leader sends an immediate repeated event beacon
void  followerRenderStep();                   // one follower render tick off the shared clock
int   syncFollowerStatus(uint8_t *out, int cap);
void  notifyFollowerStatus();
