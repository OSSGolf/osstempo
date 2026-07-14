/*
 * golden_kinematics.c — host-compilable golden test for the analytic stroke kinematics.
 * =====================================================================================
 * This is TEST infrastructure, not product firmware (it lives outside tempo_trainer/ and
 * is not part of the gated `arduino-cli` build). It ports the current per-segment position
 * math of `runStroke` / `runStrokeFull` (tempo_trainer.ino) into a PURE `positionAt(u, p)`
 * and verifies, by RUNNING on the dev machine, that:
 *   (1) the analytic head position hits the intended waypoints at each segment boundary
 *       (center, +reach, strip ends, amber) to float precision,
 *   (2) it is continuous across every boundary (no phase jump — the plan's §4.5 requirement),
 *   (3) it is the stateless function of (u, params) the sync design (TEMPO_SYNC_PLAN.md §3.2)
 *       depends on — same u ⇒ same position, no history.
 * This is the golden reference the Phase-1 renderer refactor must keep matching (plan §11).
 *
 * Build & run:  cc -O2 -o /tmp/golden firmware_tests/golden_kinematics.c -lm && /tmp/golden
 */

#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NUM_LEDS 144
static const float CENTER = (NUM_LEDS - 1) / 2.0f;   // 71.5
static const int   HALF   = (NUM_LEDS - 1) / 2;      // 71

/* Mirror of tempo_trainer.ino Profile (only the fields positionAt needs). */
typedef struct {
    unsigned backswingMs, downswingMs;
    int      reachLeds;      /* amp */
    unsigned topHoldMs;
    int      release;        /* 0..100 */
    unsigned followMs;
} Profile;

/* Global timing (mirrors the .ino globals used by the stroke). */
static unsigned getReadyMs = 1000, holdMs = 600;

/* ---- Easing (ported verbatim from tempo_trainer.ino:300-310) ---- */
static float easeInOut(float ph) { return 0.5f - 0.5f * cosf(ph * (float)M_PI); }
static float easeOut  (float ph) { return sinf(ph * (float)M_PI / 2.0f); }
static float dropEase(float ph, float release01) {
    float pendular = 1.0f - cosf(ph * (float)M_PI / 2.0f);
    float lag      = ph * ph * ph * ph;
    return pendular + (lag - pendular) * release01;
}

/* ---- The pure, stateless renderer kinematics (the Phase-1 core) ---- */
/* PUTT: center -> +amp (backswing) -> center (downswing, through impact) -> -amp (follow) -> hold. */
static float positionAt_putt(float u, Profile p) {
    float amp = (float)p.reachLeds;
    if (u < getReadyMs) return CENTER;                                  u -= getReadyMs;
    if (u < p.backswingMs) { float ph = u / p.backswingMs; return CENTER + amp * sinf(ph * (float)M_PI / 2.0f); }
    u -= p.backswingMs;
    if (u < p.downswingMs) { float ph = u / p.downswingMs; return CENTER + amp * (1.0f - dropEase(ph, p.release / 100.0f)); }
    u -= p.downswingMs;
    if (u < p.followMs)    { float ph = u / p.followMs;    return CENTER - amp * easeOut(ph); }
    return CENTER + (p.followMs > 0 ? -amp : 0.0f);                     /* hold */
}

/* FULL: center -> red(end) -> amber(top) -> [pause] -> red -> center(impact) -> far end(0) -> hold.
 * Backswing/downswing time is split by distance exactly as runStrokeFull (tempo_trainer.ino:506-528). */
static float positionAt_full(float u, Profile p) {
    float amp       = (float)p.reachLeds;
    float center    = CENTER;
    float backRed   = NUM_LEDS - 1;
    float backAmber = CENTER + amp;
    float folRed    = 0.0f;
    float dOut = backRed - center;
    float dIn  = backRed - backAmber;
    unsigned bOut  = (unsigned)(p.backswingMs * (dOut / (dOut + dIn)));
    unsigned bIn   = p.backswingMs - bOut;
    unsigned dBack = (unsigned)(p.downswingMs * (dIn / (dIn + dOut)));
    unsigned dDown = p.downswingMs - dBack;

    if (u < getReadyMs) return center;                                  u -= getReadyMs;
    if (u < bOut)  { float ph = u / (float)bOut;  return center    + (backRed - center)    * easeInOut(ph); } u -= bOut;
    if (u < bIn)   { float ph = u / (float)bIn;   return backRed   + (backAmber - backRed) * easeInOut(ph); } u -= bIn;
    if (u < p.topHoldMs) return backAmber;                              u -= p.topHoldMs;
    if (u < dBack) { float ph = u / (float)dBack; return backAmber + (backRed - backAmber) * easeInOut(ph); } u -= dBack;
    if (u < dDown) { float ph = u / (float)dDown; return backRed   + (center - backRed)    * dropEase(ph, p.release / 100.0f); } u -= dDown;
    if (u < p.followMs) { float ph = u / (float)p.followMs; return center + (folRed - center) * easeOut(ph); } u -= p.followMs;
    return (p.followMs > 0 ? folRed : center);                          /* hold */
}

static unsigned cycleDuration_putt(Profile p) { return getReadyMs + p.backswingMs + p.downswingMs + p.followMs + holdMs; }
static unsigned cycleDuration_full(Profile p) { return getReadyMs + p.backswingMs + p.topHoldMs + p.downswingMs + p.followMs + holdMs; }

/* ---- Test harness ---- */
static int failures = 0;
static void check(const char* name, float got, float want, float eps) {
    if (fabsf(got - want) > eps) { printf("  FAIL %-34s got %.4f want %.4f\n", name, got, want); failures++; }
    else                         { printf("  ok   %-34s %.4f\n", name, got); }
}

/* Continuity: sample densely and assert no step larger than the fastest legit per-ms move. */
static void checkContinuity(const char* mode, float (*f)(float, Profile), Profile p, unsigned dur) {
    float prev = f(0, p), maxStep = 0;
    for (unsigned u = 1; u < dur; u++) { float cur = f((float)u, p); float d = fabsf(cur - prev); if (d > maxStep) maxStep = d; prev = cur; }
    /* Peak speed ~ amp over the shortest active segment; 3 LEDs/ms is a generous continuity ceiling. */
    if (maxStep > 3.0f) { printf("  FAIL %-8s continuity: max per-ms step %.3f LEDs\n", mode, maxStep); failures++; }
    else                 printf("  ok   %-8s continuity: max per-ms step %.3f LEDs\n", mode, maxStep);
}

int main(void) {
    Profile putt = { 700, 350, 58, 0,   0,  350 };   /* the .ino PUTT default */
    Profile full = { 900, 300, 60, 150, 55, 300 };   /* the .ino FULL default */

    printf("PUTT waypoints (amp=%d):\n", putt.reachLeds);
    check("start = center",        positionAt_putt(0, putt),                                        CENTER, 0.01f);
    check("top of backswing",      positionAt_putt(getReadyMs + putt.backswingMs - 0.001f, putt),   CENTER + putt.reachLeds, 0.05f);
    check("impact (end downswing)",positionAt_putt(getReadyMs + putt.backswingMs + putt.downswingMs - 0.001f, putt), CENTER, 0.05f);
    check("follow end = -amp",     positionAt_putt(getReadyMs + putt.backswingMs + putt.downswingMs + putt.followMs - 0.001f, putt), CENTER - putt.reachLeds, 0.05f);
    checkContinuity("PUTT", positionAt_putt, putt, cycleDuration_putt(putt));

    printf("FULL waypoints (amp=%d):\n", full.reachLeds);
    float amp = full.reachLeds;
    unsigned bOut = (unsigned)(full.backswingMs * ((NUM_LEDS-1-CENTER) / ((NUM_LEDS-1-CENTER) + (NUM_LEDS-1-(CENTER+amp)))));
    check("start = center",        positionAt_full(0, full),                                        CENTER, 0.01f);
    check("end bOut = red end",    positionAt_full(getReadyMs + bOut - 0.001f, full),               NUM_LEDS - 1, 0.5f);
    check("top of backswing=amber",positionAt_full(getReadyMs + full.backswingMs - 0.001f, full),   CENTER + amp, 0.5f);
    check("impact = center",       positionAt_full(getReadyMs + full.backswingMs + full.topHoldMs + full.downswingMs - 0.001f, full), CENTER, 0.5f);
    check("follow end = far(0)",   positionAt_full(getReadyMs + full.backswingMs + full.topHoldMs + full.downswingMs + full.followMs - 0.001f, full), 0.0f, 0.5f);
    checkContinuity("FULL", positionAt_full, full, cycleDuration_full(full));

    /* Statelessness: same u twice ⇒ identical (no history). */
    check("stateless putt repeat", positionAt_putt(1234, putt), positionAt_putt(1234, putt), 0.0f);
    check("stateless full repeat", positionAt_full(1234, full), positionAt_full(1234, full), 0.0f);

    printf("\n%s (%d failure%s)\n", failures ? "GOLDEN PARITY FAILED" : "GOLDEN PARITY PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
