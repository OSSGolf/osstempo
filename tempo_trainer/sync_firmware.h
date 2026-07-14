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
  uint8_t  brightness;      // cosmetic global — applied immediately by a follower (§7)
  uint8_t  railsLit;        // cosmetic global — applied immediately
  uint8_t  authTag[8];      // keyed MAC over preceding bytes (§4.2.1)
};
static const uint8_t BEACON_FLAG_DISBAND = 0x01;

extern volatile SyncRole syncRole;
extern Profile           followerParams;    // leader's authoritative params, adopted by a follower (§7)
extern uint8_t           followerControl;   // leader's control state, mirrored by a follower
extern int64_t           leaderEpochUs;     // shared cycle epoch (leader clock) carried in the beacon

// Called from the main tab (loop / BLE session-command handler / setup).
void  syncBecomeLeader(uint16_t sid, const uint8_t key[16], uint8_t channel);
void  syncJoinSession(uint16_t sid, const uint8_t key[16], uint8_t channel);
void  syncDisband();
void  syncRevertToStandalone();
void  syncServiceTimeouts();
void  syncIdentifyFollower(const uint8_t mac[6]);
void  applyFollowerParamsAtRender(float u);   // §7: cosmetic immediate, kinematic at cycle boundary
void  leaderBeaconTick();
void  followerHelloTick();
float syncFollowerU();
int   syncFollowerStatus(uint8_t *out, int cap);
void  notifyFollowerStatus();
