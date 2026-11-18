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
static float dropEase(float ph, float release01) {
    float pendular = 1.0f - cosf(ph * (float)M_PI / 2.0f);
    float lag      = ph * ph * ph * ph;
    return pendular + (lag - pendular) * release01;
}

/* ---- Putt: Grober's resonant pendulum (ported from the .ino; arXiv:0903.1762) ---- */
#define PI_F ((float)M_PI)
static const float GROBER_TOP = 2.0f * PI_F / 3.0f;
static const float GROBER_AMP = 1.2990381f;                          /* X(GROBER_TOP) = 3*sqrt(3)/4 */
#define GROBER_IMPACT_SLOPE ((2.0f * PI_F / 3.0f) / GROBER_AMP)
static float groberX(float th) { return sinf(th) - 0.5f * sinf(2.0f * th); }
static float puttBackEase(float ph) { return groberX(ph * GROBER_TOP) / GROBER_AMP; }
static float puttDownEase(float ph, float release01) {
    float pendular = 1.0f - groberX(GROBER_TOP + ph * (PI_F - GROBER_TOP)) / GROBER_AMP;
    float lag      = ph * ph * ph * ph;
    return pendular + (lag - pendular) * release01;
}

/* ---- Through-swing style (…0016): identical up to impact, differs only after it ---- */
enum { THROUGH_PENDULUM = 0, THROUGH_CONSTANT = 1 };
static int throughMode = THROUGH_CONSTANT;
static float throughEase(float ph) { return throughMode == THROUGH_PENDULUM ? sinf(ph * PI_F / 2.0f) : ph; }
static float throughStretch(void)  { return throughMode == THROUGH_PENDULUM ? PI_F / 2.0f : 1.0f; }

/* Follow-through duration (see followDurMs in the .ino). `downSeg` is the segment that arrives at
 * impact: the whole downswing for a putt, the last leg (far end -> ball) for a full swing; `base`
 * is that segment's progress slope at impact. followMs is only the on/off. */
static unsigned followDur(Profile p, unsigned downSeg, float base) {
    if (p.followMs == 0) return 0;
    float r = p.release / 100.0f;
    unsigned d = (unsigned)(downSeg / (base + (4.0f - base) * r) * throughStretch());
    return d ? d : 1;
}
static unsigned fullDownLeg(Profile p) {
    float dOut = (NUM_LEDS - 1) - CENTER, dIn = (NUM_LEDS - 1) - (CENTER + (float)p.reachLeds);
    return p.downswingMs - (unsigned)(p.downswingMs * (dIn / (dIn + dOut)));
}
static unsigned followDur_putt(Profile p) { return followDur(p, p.downswingMs, GROBER_IMPACT_SLOPE); }
static unsigned followDur_full(Profile p) { return followDur(p, fullDownLeg(p), PI_F / 2.0f); }

/* ---- The pure, stateless renderer kinematics (the Phase-1 core) ---- */
/* PUTT: center -> +amp (backswing) -> center (downswing, through impact) -> -amp (follow) -> hold. */
static float positionAt_putt(float u, Profile p) {
    float amp = (float)p.reachLeds;
    if (u < getReadyMs) return CENTER;                                  u -= getReadyMs;
    if (u < p.backswingMs) { float ph = u / p.backswingMs; return CENTER + amp * puttBackEase(ph); }
    u -= p.backswingMs;
    if (u < p.downswingMs) { float ph = u / p.downswingMs; return CENTER + amp * (1.0f - puttDownEase(ph, p.release / 100.0f)); }
    u -= p.downswingMs;
    unsigned fd = followDur_putt(p);
    if (u < fd)            { float ph = u / fd;            return CENTER - amp * throughEase(ph); }
    return CENTER + (fd > 0 ? -amp : 0.0f);                             /* hold */
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
    unsigned fd = followDur_full(p);
    if (u < fd) { float ph = u / (float)fd; return center + (folRed - center) * throughEase(ph); } u -= fd;
    return (fd > 0 ? folRed : center);                                  /* hold */
}

static unsigned cycleDuration_putt(Profile p) { return getReadyMs + p.backswingMs + p.downswingMs + followDur_putt(p) + holdMs; }
static unsigned cycleDuration_full(Profile p) { return getReadyMs + p.backswingMs + p.topHoldMs + p.downswingMs + followDur_full(p) + holdMs; }

/* The follow-through must leave impact at the speed the downswing arrived with, and hold it:
 * compare LEDs/ms just before impact, just after, and just before the finish. */
static void checkFollowSpeed(const char* mode, float (*f)(float, Profile), Profile p, float tImpact, unsigned fd);

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

static void checkFollowSpeed(const char* mode, float (*f)(float, Profile), Profile p, float tImpact, unsigned fd) {
    const float h = 0.5f;
    float vIn   = (f(tImpact - 0.01f, p) - f(tImpact - 0.01f - h, p)) / h;
    float vOut  = (f(tImpact + h, p) - f(tImpact, p)) / h;
    float vLate = (f(tImpact + fd - 0.01f, p) - f(tImpact + fd - 0.01f - h, p)) / h;
    char name[64];
    /* 3%: the duration is truncated to whole ms, and vIn is a finite difference on a curve. */
    snprintf(name, sizeof name, "%s speed kept through impact", mode); check(name, vOut,  vIn,  fabsf(vIn) * 0.03f);
    if (throughMode == THROUGH_CONSTANT) {
        snprintf(name, sizeof name, "%s no slowing to the finish", mode);  check(name, vLate, vOut, fabsf(vOut) * 0.001f);
    } else {
        snprintf(name, sizeof name, "%s comes to rest at the finish", mode); check(name, vLate, 0.0f, fabsf(vOut) * 0.02f);
    }
}

/* Grober's three signatures, checked on the default putt. */
static void checkGrober(Profile p) {
    const float t0 = getReadyMs, top = t0 + p.backswingMs, impact = top + p.downswingMs, h = 0.5f;
    check("takeaway starts from rest",   (positionAt_putt(t0 + h, p) - positionAt_putt(t0, p)) / h, 0.0f, 0.001f);
    check("rest at the top",             (positionAt_putt(top + h, p) - positionAt_putt(top - h, p)) / (2 * h), 0.0f, 0.001f);
    /* halfway through the backswing TIME the putter is only a third of the way back */
    check("half-time backswing = 1/3",   (positionAt_putt(t0 + p.backswingMs / 2.0f, p) - CENTER) / p.reachLeds, 0.3333f, 0.002f);
    /* impact speed is 1.61x the downswing's average speed */
    float vImpact = (positionAt_putt(impact - 0.01f, p) - positionAt_putt(impact - 0.01f - h, p)) / h;
    float vAvg    = -(float)p.reachLeds / p.downswingMs;
    check("impact / average speed 1.61", vImpact / vAvg, 1.6123f, 0.01f);
    /* zero acceleration at impact: speed is flat over the last few ms */
    float vJustBefore = (positionAt_putt(impact - 5.0f, p) - positionAt_putt(impact - 5.0f - h, p)) / h;
    check("constant speed into impact",  vJustBefore / vImpact, 1.0f, 0.002f);
}

int main(void) {
    Profile putt = { 650, 325, 23, 0,   0,  350 };   /* the .ino PUTT default (a 5 ft putt) */
    Profile full = { 900, 300, 60, 150, 55, 300 };   /* the .ino FULL default */

    printf("PUTT waypoints (amp=%d):\n", putt.reachLeds);
    check("start = center",        positionAt_putt(0, putt),                                        CENTER, 0.01f);
    check("top of backswing",      positionAt_putt(getReadyMs + putt.backswingMs - 0.001f, putt),   CENTER + putt.reachLeds, 0.05f);
    check("impact (end downswing)",positionAt_putt(getReadyMs + putt.backswingMs + putt.downswingMs - 0.001f, putt), CENTER, 0.05f);
    check("follow end = -amp",     positionAt_putt(getReadyMs + putt.backswingMs + putt.downswingMs + followDur_putt(putt) - 0.001f, putt), CENTER - putt.reachLeds, 0.05f);
    checkFollowSpeed("PUTT", positionAt_putt, putt, getReadyMs + putt.backswingMs + putt.downswingMs, followDur_putt(putt));
    checkGrober(putt);
    check("constant through = 201 ms", (float)followDur_putt(putt), 201.0f, 0.5f);
    { Profile lag = putt; lag.release = 60;
      checkFollowSpeed("PUTT lag60", positionAt_putt, lag, getReadyMs + lag.backswingMs + lag.downswingMs, followDur_putt(lag)); }
    { Profile off = putt; off.followMs = 0;
      check("follow off = stays at ball", positionAt_putt(getReadyMs + off.backswingMs + off.downswingMs + 50, off), CENTER, 0.01f); }
    checkContinuity("PUTT", positionAt_putt, putt, cycleDuration_putt(putt));

    printf("FULL waypoints (amp=%d):\n", full.reachLeds);
    float amp = full.reachLeds;
    unsigned bOut = (unsigned)(full.backswingMs * ((NUM_LEDS-1-CENTER) / ((NUM_LEDS-1-CENTER) + (NUM_LEDS-1-(CENTER+amp)))));
    check("start = center",        positionAt_full(0, full),                                        CENTER, 0.01f);
    check("end bOut = red end",    positionAt_full(getReadyMs + bOut - 0.001f, full),               NUM_LEDS - 1, 0.5f);
    check("top of backswing=amber",positionAt_full(getReadyMs + full.backswingMs - 0.001f, full),   CENTER + amp, 0.5f);
    check("impact = center",       positionAt_full(getReadyMs + full.backswingMs + full.topHoldMs + full.downswingMs - 0.001f, full), CENTER, 0.5f);
    check("follow end = far(0)",   positionAt_full(getReadyMs + full.backswingMs + full.topHoldMs + full.downswingMs + followDur_full(full) - 0.001f, full), 0.0f, 0.5f);
    checkFollowSpeed("FULL", positionAt_full, full, getReadyMs + full.backswingMs + full.topHoldMs + full.downswingMs, followDur_full(full));
    checkContinuity("FULL", positionAt_full, full, cycleDuration_full(full));

    /* Pendulum through-swing: same stroke up to impact, same finish, decelerating to rest. */
    printf("PENDULUM through-swing:\n");
    { float before[4]; const float ts[4] = { 1100, 1400, 1700, 1974 };
      for (int i = 0; i < 4; i++) before[i] = positionAt_putt(ts[i], putt);
      throughMode = THROUGH_PENDULUM;
      for (int i = 0; i < 4; i++) check("identical before impact", positionAt_putt(ts[i], putt), before[i], 0.0f);
      check("pendulum through = 316 ms", (float)followDur_putt(putt), 316.0f, 0.5f);
      check("follow end = -amp", positionAt_putt(getReadyMs + putt.backswingMs + putt.downswingMs + followDur_putt(putt) - 0.001f, putt), CENTER - putt.reachLeds, 0.05f);
      checkFollowSpeed("PUTT", positionAt_putt, putt, getReadyMs + putt.backswingMs + putt.downswingMs, followDur_putt(putt));
      checkFollowSpeed("FULL", positionAt_full, full, getReadyMs + full.backswingMs + full.topHoldMs + full.downswingMs, followDur_full(full));
      checkContinuity("PUTT", positionAt_putt, putt, cycleDuration_putt(putt));
      checkContinuity("FULL", positionAt_full, full, cycleDuration_full(full));
      throughMode = THROUGH_CONSTANT; }

    /* Statelessness: same u twice ⇒ identical (no history). */
    check("stateless putt repeat", positionAt_putt(1234, putt), positionAt_putt(1234, putt), 0.0f);
    check("stateless full repeat", positionAt_full(1234, full), positionAt_full(1234, full), 0.0f);

    printf("\n%s (%d failure%s)\n", failures ? "GOLDEN PARITY FAILED" : "GOLDEN PARITY PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
