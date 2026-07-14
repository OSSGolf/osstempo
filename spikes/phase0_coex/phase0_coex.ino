/*
 * OSSTempo — Phase 0 coexistence / render-timing spike  (THROWAWAY, not product code)
 * ================================================================================
 * Gating go/no-go for the whole multi-stick sync feature (TEMPO_SYNC_PLAN.md §5.3, §10, §11).
 * This sketch exercises BLE (Bluedroid) + WiFi/ESP-NOW + a FastLED render task under load and
 * MEASURES the exit criteria. It contains NO product logic — the real renderer/protocol only
 * begin (Phase 1) if this passes on BOTH real boards.
 *
 * WHAT IT MEASURES (map to plan §5.3):
 *   1. BLE stays up + >=99% beacon delivery over 10 min   (leader TX count, follower RX seq-gap %)
 *   2. Free heap >= baseline - 40 KB, stable              (leader heap log: baseline, current, min-ever)
 *   3. Render-tick jitter < 1 ms under coex               (per-tick esp_timer delta, serial + scope GPIO)
 *      + no flash write lands during a visible stroke      (EVENT_PIN pulses on BLE CCCD/subscribe)
 *   4. No corrupted/dropped frame (RMT-vs-I2S bake-off)    (visual + jitter under each driver)
 *   5. Leader power stability                              (esp_reset_reason() at boot catches brownout/WDT)
 *
 * HOW TO USE: see spikes/phase0_coex/README.md for the full bench procedure. In brief:
 *   - Flash board A as ROLE_LEADER=1, board B as ROLE_LEADER=0 (follower).
 *   - Build once with USE_I2S_DRIVER=0 (RMT, default) and once with =1 (I2S/DMA) — the bake-off.
 *   - Watch each board's serial @115200; scope RENDER_TICK_PIN (+ EVENT_PIN) if your board exposes them.
 *
 * Build (both boards, both drivers — all four must compile --warnings all):
 *   arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=huge_app" --warnings all \
 *     --build-property "compiler.cpp.extra_flags=-DBOARD_DIG2GO -DROLE_LEADER=1 -DUSE_I2S_DRIVER=0" \
 *     spikes/phase0_coex
 */

// ---- Compile-time selection (override with -D… on the arduino-cli command line) ------------------
#ifndef ROLE_LEADER
  #define ROLE_LEADER      1        // 1 = leader (BLE+WiFi+ESP-NOW TX+render), 0 = follower (WiFi+RX+render)
#endif
#ifndef USE_I2S_DRIVER
  #define USE_I2S_DRIVER   0        // 0 = RMT (default), 1 = I2S/DMA (the bake-off alternative)
#endif
#if !defined(BOARD_DIG2GO) && !defined(BOARD_DIY_ESP32)
  #define BOARD_DIG2GO              // default to the board this repo runs on now
#endif

// The I2S driver must be requested BEFORE FastLED.h is included.
#if USE_I2S_DRIVER
  #define FASTLED_ESP32_I2S
#endif

#include <FastLED.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_system.h>

#if ROLE_LEADER
  #include <BLEDevice.h>
  #include <BLEServer.h>
  #include <BLEUtils.h>
  #include <BLE2902.h>
#endif

// ---- Hardware config (mirrors tempo_trainer.ino) -------------------------------------------------
#if defined(BOARD_DIG2GO)
  #define LED_PIN        16
  #define LED_POWER_PIN  12         // Dig2go relay: assert to feed 5 V to the strip
  #define LED_POWER_ON   HIGH
#elif defined(BOARD_DIY_ESP32)
  #define LED_PIN         5
#endif
#define NUM_LEDS        144
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB

// Scope pins — route to a 2-channel scope. CHANGE these to pins your board actually breaks out.
// (Jitter is ALSO reported on serial, so the scope is authoritative but not strictly required.)
#define RENDER_TICK_PIN 17          // toggles every render tick — measure interval jitter here
#define EVENT_PIN       18          // pulses on a BLE CCCD/subscribe write (flash-vs-render correlation)

// ---- Grid / cadence ------------------------------------------------------------------------------
static const int64_t FRAME_US = 8000;   // 125 Hz render grid (plan §3.4)
static const uint32_t BEACON_MS = 250;  // 4 Hz keepalive (plan §4.4)

CRGB leds[NUM_LEDS];

// ---- ESP-NOW beacon (minimal — the full SyncBeacon is Phase 2) -----------------------------------
static const uint16_t MAGIC = 0x544F;   // 'O','T' little-endian
struct __attribute__((packed)) SpikeBeacon {
  uint16_t magic;
  uint32_t seq;
  int64_t  txMicros;
};
static uint8_t BROADCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t CH_OP = 6;         // fixed operating channel for the spike

// ---- Shared render-jitter accounting -------------------------------------------------------------
static TaskHandle_t   renderTaskHandle = nullptr;
static esp_timer_handle_t gridTimer = nullptr;
static volatile uint32_t g_tickCount = 0;

// ---- Follower RX accounting ----------------------------------------------------------------------
#if !ROLE_LEADER
static volatile uint32_t rxCount = 0, firstSeq = 0, lastSeq = 0;
#endif

// ---- Leader TX + BLE accounting ------------------------------------------------------------------
#if ROLE_LEADER
static volatile uint32_t txSeq = 0;
static volatile uint32_t g_cccdWrites = 0;
static volatile bool     g_bleConnected = false;
#endif

// =================================================================================================
//  Render task — pinned to APP_CPU (core 1). Radios live on PRO_CPU (core 0). (plan §3.5)
// =================================================================================================
static void gridTimerCb(void*) {                    // esp_timer TASK context (not ISR) — only wakes render
  if (renderTaskHandle) xTaskNotifyGive(renderTaskHandle);
}

static void renderTask(void*) {
  int64_t prev = 0, jMin = INT64_MAX, jMax = 0; double jSum = 0; uint32_t jN = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);        // block until the grid tick
    int64_t now = esp_timer_get_time();
    if (prev) { int64_t j = llabs((now - prev) - FRAME_US); if (j < jMin) jMin = j; if (j > jMax) jMax = j; jSum += j; jN++; }
    prev = now;

    digitalWrite(RENDER_TICK_PIN, (g_tickCount & 1) ? HIGH : LOW);   // scope edge per tick

    // Dummy "comet" at FULL brightness (worst case for the brownout soak).
    fadeToBlackBy(leds, NUM_LEDS, 40);
    leds[g_tickCount % NUM_LEDS] = CRGB::White;
    FastLED.show();

    g_tickCount++;
    if (jN >= 625) {                                 // ~5 s @125 Hz
      Serial.printf("[render] tick=%lu  jitter us  min/avg/max = %lld / %.1f / %lld  %s\n",
                    (unsigned long)g_tickCount, (long long)jMin, jSum / jN, (long long)jMax,
                    (jMax < 1000) ? "OK(<1ms)" : "**OVER 1ms**");
      jMin = INT64_MAX; jMax = 0; jSum = 0; jN = 0;
    }
  }
}

// =================================================================================================
//  ESP-NOW
// =================================================================================================
#if !ROLE_LEADER
static void onEspNowRecv(const esp_now_recv_info_t* /*info*/, const uint8_t* data, int len) {
  if (len < (int)sizeof(SpikeBeacon)) return;
  SpikeBeacon b; memcpy(&b, data, sizeof(b));
  if (b.magic != MAGIC) return;
  if (rxCount == 0) firstSeq = b.seq;
  lastSeq = b.seq;
  rxCount++;
}
#endif

#if ROLE_LEADER
static void beaconTask(void*) {                      // pinned to core 0 (radios)
  for (;;) {
    SpikeBeacon b { MAGIC, ++txSeq, esp_timer_get_time() };
    esp_now_send(BROADCAST, (const uint8_t*)&b, sizeof(b));
    vTaskDelay(pdMS_TO_TICKS(BEACON_MS));
  }
}
#endif

static void radioInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);                      // no AP association; just the STA radio for ESP-NOW
  esp_wifi_set_ps(WIFI_PS_NONE);                     // no power save (plan §5.1)
  esp_wifi_set_channel(CH_OP, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) { Serial.println("!! esp_now_init failed"); return; }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = CH_OP;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

#if !ROLE_LEADER
  esp_now_register_recv_cb(onEspNowRecv);
#endif
}

// =================================================================================================
//  BLE (leader only) — a live connection + a NOTIFY char whose CCCD write is the "involuntary flash"
//  event we correlate against the render tick (plan §5.3-3, §11).
// =================================================================================================
#if ROLE_LEADER
static const char* SVC_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";  // product service (spike reuse)
static const char* CHR_UUID = "6e400006-b5a3-f393-e0a9-e50e24dcca9e";  // NOTIFY (control-tail 0006)
static BLECharacteristic* g_notifyChr = nullptr;

class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override    { g_bleConnected = true;  Serial.println("[ble] connected"); }
  void onDisconnect(BLEServer* s) override { g_bleConnected = false; Serial.println("[ble] disconnected — re-advertising"); s->getAdvertising()->start(); }
};
class CccdCb : public BLEDescriptorCallbacks {       // fires when the central subscribes → CCCD persist
  void onWrite(BLEDescriptor*) override {
    g_cccdWrites++;
    digitalWrite(EVENT_PIN, HIGH); delayMicroseconds(50); digitalWrite(EVENT_PIN, LOW);  // scope marker
    Serial.printf("[ble] CCCD write #%lu (subscribe) — watch the render tick for a stall NOW\n",
                  (unsigned long)g_cccdWrites);
  }
};

static void bleInit() {
  BLEDevice::init("Phase0Spike");
  BLEServer* srv = BLEDevice::createServer();
  srv->setCallbacks(new SrvCb());
  BLEService* svc = srv->createService(SVC_UUID);
  g_notifyChr = svc->createCharacteristic(CHR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  BLE2902* cccd = new BLE2902();
  cccd->setCallbacks(new CccdCb());
  g_notifyChr->addDescriptor(cccd);
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[ble] advertising as Phase0Spike");
}
#endif

// =================================================================================================
static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "sw";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "**INT_WDT**";
    case ESP_RST_TASK_WDT: return "**TASK_WDT**";
    case ESP_RST_WDT:      return "**WDT**";
    case ESP_RST_BROWNOUT: return "**BROWNOUT**";
    default:               return "other";
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("=== Phase0 spike | role=%s | driver=%s | board=%s ===\n",
                ROLE_LEADER ? "LEADER" : "follower",
                USE_I2S_DRIVER ? "I2S/DMA" : "RMT",
#if defined(BOARD_DIG2GO)
                "DIG2GO");
#else
                "DIY_ESP32");
#endif
  Serial.printf("[boot] last reset reason: %s   (watch for BROWNOUT/WDT after a soak)\n",
                resetReasonStr(esp_reset_reason()));

  pinMode(RENDER_TICK_PIN, OUTPUT); digitalWrite(RENDER_TICK_PIN, LOW);
  pinMode(EVENT_PIN, OUTPUT);       digitalWrite(EVENT_PIN, LOW);
#ifdef LED_POWER_PIN
  pinMode(LED_POWER_PIN, OUTPUT);   digitalWrite(LED_POWER_PIN, LED_POWER_ON);   // Dig2go: feed the strip
#endif

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(255);        // full brightness — worst case for the power soak

  radioInit();
#if ROLE_LEADER
  bleInit();
  xTaskCreatePinnedToCore(beaconTask, "beacon", 4096, nullptr, 4, nullptr, 0);   // radios → core 0
#endif

  // Render task + grid pacer LAST, pinned to APP_CPU (core 1).
  xTaskCreatePinnedToCore(renderTask, "render", 4096, nullptr, 5, &renderTaskHandle, 1);
  const esp_timer_create_args_t ta = { &gridTimerCb, nullptr, ESP_TIMER_TASK, "grid", true };
  esp_timer_create(&ta, &gridTimer);
  esp_timer_start_periodic(gridTimer, FRAME_US);

  Serial.printf("[heap] baseline free = %u bytes  (criterion #2: stay >= baseline-40KB)\n",
                (unsigned)esp_get_free_heap_size());
}

void loop() {
  static uint32_t lastLog = 0;
  if (millis() - lastLog < 10000) { delay(50); return; }
  lastLog = millis();

#if ROLE_LEADER
  if (g_bleConnected && g_notifyChr) {               // keep BLE traffic flowing during the soak
    uint8_t v = (uint8_t)(g_tickCount & 0xFF);
    g_notifyChr->setValue(&v, 1);
    g_notifyChr->notify();
  }
  Serial.printf("[heap] free=%u  min-ever=%u  | [ble] %s  cccd=%lu | [tx] beacons=%lu\n",
                (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
                g_bleConnected ? "up" : "down", (unsigned long)g_cccdWrites, (unsigned long)txSeq);
#else
  uint32_t rc = rxCount, fs = firstSeq, ls = lastSeq;
  uint32_t expected = (rc ? (ls - fs + 1) : 0);
  double pct = expected ? (100.0 * rc / expected) : 0.0;
  Serial.printf("[rx] beacons=%lu  expected(seq-span)=%lu  delivery=%.1f%%  %s | [heap] free=%u\n",
                (unsigned long)rc, (unsigned long)expected, pct,
                (pct >= 99.0 || rc < 40) ? "" : "**UNDER 99%**", (unsigned)esp_get_free_heap_size());
#endif
}
