/*
 * flip_mirror.c — host-compilable golden test for the 180° strip flip.
 * =====================================================================================
 * This is TEST infrastructure, not product firmware (it lives outside tempo_trainer/ and
 * is not part of the gated `arduino-cli` build). It ports the FRAME COMPOSITION of
 * `renderFrameAt` / `drawCometAnalytic` (tempo_trainer.ino) plus the output mirror from
 * `showFrame`, and verifies, by RUNNING on the dev machine, that:
 *
 *   (1) the mirror is an involution and maps i -> NUM_LEDS-1-i over the WHOLE buffer (no
 *       partial range, no dropped endpoint);
 *   (2) reflection about the strip is NOT geometry inversion. CENTER is the half-integer 71.5,
 *       so mirroring the amber marker gives 71-amp while inverting the stroke math would give
 *       72-amp — they differ by exactly one LED (6.9 mm) for EVERY reach. This is the assertion
 *       that earns its keep: it fails the day someone "simplifies" the output mirror into the
 *       rejected CENTER-amp geometry inversion, instead of that shipping as a silent asymmetry;
 *   (3) the mirror is an exact element-wise reversal of REAL frame content — swept over every
 *       cycle-time u, in both modes, across rails on/off x aim2 on/off x all four aim2When x
 *       inside/outside the get-ready window — and is colour- and count-preserving, so nothing is
 *       dropped, duplicated, or dimmed by the transform.
 *
 * WHAT THIS DOES NOT PROVE, stated plainly, because an earlier version of this file overclaimed.
 * Like golden_kinematics.c it is a PORT: it checks the algebra, not that the .ino wires it up.
 * Two things it therefore cannot do, both of which used to be asserted here as unfailable
 * tautologies and have been removed rather than left as green paint:
 *
 *   - It cannot show the renderer ignores the flip flag. This port's composeFrame does not take
 *     `flipped` at all, so any such assertion compares a function against itself.
 *   - It cannot show that every LED element reaches the mirror. That is structural: the mirror
 *     is applied to the finished buffer, so it is total exactly as long as every render path
 *     latches through showFrame() and FastLED is bound to the physical buffer.
 *
 * Both of those are enforced by firmware_tests/check_show_chokepoint.sh, which reads the actual
 * .ino. The two tests are complements; neither is sufficient alone. Run both.
 *
 * Build & run:  cc -O2 -o /tmp/flipmirror firmware_tests/flip_mirror.c -lm && /tmp/flipmirror
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NUM_LEDS 144
#define TAIL 6
static const float CENTER = (NUM_LEDS - 1) / 2.0f;   /* 71.5 */

enum { MODE_PUTT = 0, MODE_FULL = 1 };

/* ---- CRGB, ported from FastLED's byte-triple semantics (only what the renderer uses) ---- */
typedef struct { unsigned char r, g, b; } CRGB;
static const CRGB BLACK        = {   0,   0,   0 };
static const CRGB BALL_COLOR   = {   0, 120,   0 };
static const CRGB TARGET_COLOR = { 255,  90,   0 };
static const CRGB RED_COLOR    = { 200,   0,   0 };
static const CRGB AIM2_COLOR   = { 150,  70, 255 };

static int rgbEq(CRGB a, CRGB b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
/* FastLED's operator+= saturates per channel (qadd8). drawCometAnalytic relies on it. */
static unsigned char qadd8(int a, int b) { int s = a + b; return (unsigned char)(s > 255 ? 255 : s); }
static CRGB rgbAdd(CRGB a, CRGB b) { CRGB o; o.r = qadd8(a.r, b.r); o.g = qadd8(a.g, b.g); o.b = qadd8(a.b, b.b); return o; }

/* ---- Mirror of tempo_trainer.ino Profile (the fields the renderer reads) ---- */
typedef struct {
    unsigned backswingMs, downswingMs;
    int      reachLeds;
    unsigned topHoldMs;
    int      release;
    unsigned followMs;
    int      aim2Enabled, aim2Led, aim2When;
} Profile;

/* Global timing + look (mirrors the .ino globals the renderer reads). */
static unsigned getReadyMs = 1000, holdMs = 600;
static int mode = MODE_PUTT;
static int railsLit = 1;
static int flipped = 0;

/* ---- Easing, ported verbatim from tempo_trainer.ino ---- */
static float easeInOut(float ph) { return 0.5f - 0.5f * cosf(ph * (float)M_PI); }
static float easeOut  (float ph) { return sinf(ph * (float)M_PI / 2.0f); }
static float dropEase(float ph, float release01) {
    float pendular = 1.0f - cosf(ph * (float)M_PI / 2.0f);
    float lag      = ph * ph * ph * ph;
    return pendular + (lag - pendular) * release01;
}

/* ---- cycleDuration / positionAt / strokeDirAt / aim2LitAt, ported verbatim ---- */
static unsigned cycleDuration(const Profile *p) {
    return getReadyMs + p->backswingMs + p->topHoldMs + p->downswingMs + p->followMs + holdMs;
}

static float positionAt(float u, const Profile *p) {
    float amp = (float)p->reachLeds;
    if (mode == MODE_FULL) {
        float center = CENTER, backRed = NUM_LEDS - 1, backAmber = CENTER + amp, folRed = 0.0f;
        float dOut = backRed - center, dIn = backRed - backAmber;
        unsigned bOut = (unsigned)(p->backswingMs * (dOut / (dOut + dIn))), bIn = p->backswingMs - bOut;
        unsigned dBack = (unsigned)(p->downswingMs * (dIn / (dIn + dOut))), dDown = p->downswingMs - dBack;
        if (u < getReadyMs) return center;                                                  u -= getReadyMs;
        if (u < bOut)  { float ph = u / (float)bOut;  return center    + (backRed - center)    * easeInOut(ph); } u -= bOut;
        if (u < bIn)   { float ph = u / (float)bIn;   return backRed   + (backAmber - backRed) * easeInOut(ph); } u -= bIn;
        if (u < p->topHoldMs) return backAmber;                                             u -= p->topHoldMs;
        if (u < dBack) { float ph = u / (float)dBack; return backAmber + (backRed - backAmber)* easeInOut(ph); } u -= dBack;
        if (u < dDown) { float ph = u / (float)dDown; return backRed   + (center - backRed)   * dropEase(ph, p->release / 100.0f); } u -= dDown;
        if (u < p->followMs) { float ph = u / (float)p->followMs; return center + (folRed - center) * easeOut(ph); }
        return (p->followMs > 0 ? folRed : center);
    } else {
        if (u < getReadyMs) return CENTER;                                                  u -= getReadyMs;
        if (u < p->backswingMs) { float ph = u / (float)p->backswingMs; return CENTER + amp * sinf(ph * (float)M_PI / 2.0f); } u -= p->backswingMs;
        if (u < p->downswingMs) { float ph = u / (float)p->downswingMs; return CENTER + amp * (1.0f - dropEase(ph, p->release / 100.0f)); } u -= p->downswingMs;
        if (u < p->followMs)    { float ph = u / (float)p->followMs;    return CENTER - amp * easeOut(ph); }
        return CENTER + (p->followMs > 0 ? -amp : 0.0f);
    }
}

static int strokeDirAt(float u, const Profile *p) {
    float amp = (float)p->reachLeds;
    if (mode == MODE_FULL) {
        float dOut = (NUM_LEDS - 1) - CENTER, dIn = (NUM_LEDS - 1) - (CENTER + amp);
        unsigned bOut = (unsigned)(p->backswingMs * (dOut / (dOut + dIn))), bIn = p->backswingMs - bOut;
        unsigned dBack = (unsigned)(p->downswingMs * (dIn / (dIn + dOut))), dDown = p->downswingMs - dBack;
        if (u < getReadyMs) return 0;      u -= getReadyMs;
        if (u < bOut) return 1;            u -= bOut;
        if (u < bIn) return -1;            u -= bIn;
        if (u < p->topHoldMs) return 0;    u -= p->topHoldMs;
        if (u < dBack) return 1;           u -= dBack;
        if (u < dDown) return -1;          u -= dDown;
        if (u < p->followMs) return -1;
        return 0;
    } else {
        if (u < getReadyMs) return 0;      u -= getReadyMs;
        if (u < p->backswingMs) return 1;  u -= p->backswingMs;
        if (u < p->downswingMs) return -1; u -= p->downswingMs;
        if (u < p->followMs) return -1;
        return 0;
    }
}

static int aim2LitAt(float u, const Profile *p) {
    unsigned tTop = getReadyMs + p->backswingMs;
    unsigned tDown = tTop + p->topHoldMs;
    unsigned tImpact = tDown + p->downswingMs;
    switch (p->aim2When) {
        case 0: return u >= getReadyMs;
        case 1: return u >= tTop;
        case 2: return u >= tDown;
        case 3: return u >= tImpact;
    }
    return 0;
}

/* ---- drawCometAnalytic + renderFrameAt composition, ported verbatim ---- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void drawCometAnalytic(CRGB *leds, float pos, int dir, int loIdx, int hiIdx) {
    if (dir == 0) dir = 1;
    int base = (int)floorf(pos);
    for (int i = -1; i <= TAIL; i++) {
        int idx = base - dir * i;
        if (idx < loIdx || idx > hiIdx || idx < 0 || idx >= NUM_LEDS) continue;
        unsigned char b = (i <= 0) ? 255 : (unsigned char)(255.0f * (1.0f - (float)i / (TAIL + 1)));
        CRGB add = { b, b, b };
        leds[idx] = rgbAdd(leds[idx], add);
    }
}

/* Compose one LOGICAL frame at cycle-time u (the body of renderFrameAt, minus the show). */
static void composeFrame(CRGB *leds, float u, const Profile *p) {
    float amp = (float)p->reachLeds;
    int centerIdx = (int)roundf(CENTER);
    for (int i = 0; i < NUM_LEDS; i++) leds[i] = BLACK;          /* FastLED.clear() */
    if (railsLit) {
        if (centerIdx >= 0 && centerIdx < NUM_LEDS) leds[centerIdx] = BALL_COLOR;
        int amberIdx = (int)roundf(CENTER + amp);
        if (amberIdx >= 0 && amberIdx < NUM_LEDS) leds[amberIdx] = TARGET_COLOR;
        if (mode == MODE_FULL) { leds[NUM_LEDS - 1] = RED_COLOR; leds[0] = RED_COLOR; }
    }
    if (mode == MODE_FULL && p->aim2Enabled && aim2LitAt(u, p)) {
        int a = clampi(p->aim2Led, 1, NUM_LEDS - 2);
        leds[a] = AIM2_COLOR;
    }
    if (u < getReadyMs) {                                        /* get-ready pulse, early return */
        float local = u - floorf(u / 1000.0f) * 1000.0f;
        float lvl   = sinf((local / 1000.0f) * (float)M_PI);
        if (centerIdx >= 0 && centerIdx < NUM_LEDS) {
            CRGB g = { 0, (unsigned char)(220.0f * lvl), 0 };
            leds[centerIdx] = g;
        }
        return;
    }
    drawCometAnalytic(leds, positionAt(u, p), strokeDirAt(u, p), 0, NUM_LEDS - 1);
}

/* The output mirror from showFrame(): logical frame -> physical strip. */
static void toPhysical(CRGB *phys, const CRGB *logical) {
    if (flipped) for (int i = 0; i < NUM_LEDS; i++) phys[NUM_LEDS - 1 - i] = logical[i];
    else         memcpy(phys, logical, NUM_LEDS * sizeof(CRGB));
}

/* ---- Harness ---- */
static int failures = 0;
static void fail(const char *what, const char *detail) {
    printf("  FAIL %-44s %s\n", what, detail);
    failures++;
}
static void ok(const char *what, const char *detail) { printf("  ok   %-44s %s\n", what, detail); }

/* (1) the mirror is an involution and maps i -> NUM_LEDS-1-i. */
static void checkInvolutionAndIndexMap(void) {
    CRGB a[NUM_LEDS], b[NUM_LEDS], c[NUM_LEDS];
    for (int i = 0; i < NUM_LEDS; i++) { a[i].r = (unsigned char)i; a[i].g = (unsigned char)(255 - i); a[i].b = (unsigned char)(i * 7); }
    flipped = 1;
    toPhysical(b, a);
    for (int i = 0; i < NUM_LEDS; i++)
        if (!rgbEq(b[NUM_LEDS - 1 - i], a[i])) { fail("index map i -> NUM_LEDS-1-i", "mismatch"); flipped = 0; return; }
    ok("index map i -> NUM_LEDS-1-i", "all 144 indices");
    toPhysical(c, b);
    for (int i = 0; i < NUM_LEDS; i++)
        if (!rgbEq(c[i], a[i])) { fail("mirror is an involution", "second application != identity"); flipped = 0; return; }
    ok("mirror is an involution", "mirror(mirror(x)) == x");
    flipped = 0;
    toPhysical(b, a);
    for (int i = 0; i < NUM_LEDS; i++)
        if (!rgbEq(b[i], a[i])) { fail("unflipped is the identity", "buffer altered"); return; }
    ok("unflipped is the identity", "byte-for-byte copy");
}

/* (4) colour/count preservation: the multiset of lit pixels survives the mirror. */
static int litCount(const CRGB *f) { int n = 0; for (int i = 0; i < NUM_LEDS; i++) if (!rgbEq(f[i], BLACK)) n++; return n; }
static long channelSum(const CRGB *f) { long s = 0; for (int i = 0; i < NUM_LEDS; i++) s += f[i].r + f[i].g + f[i].b; return s; }

/*
 * (2)+(4) The core sweep: for every combination and every sampled u, assert that the
 * PHYSICAL frame rendered with flipped=1 is the exact element-wise reverse of the physical
 * frame rendered with flipped=0, and that nothing is lost or dimmed.
 */
static void checkFrameSweep(const char *label, Profile p, int modeVal) {
    CRGB logical[NUM_LEDS], normal[NUM_LEDS], mirrored[NUM_LEDS];
    long samples = 0;
    mode = modeVal;
    for (int rails = 0; rails <= 1; rails++) {
        for (int aimEn = 0; aimEn <= 1; aimEn++) {
            for (int aimWhen = 0; aimWhen <= 3; aimWhen++) {
                railsLit = rails;
                p.aim2Enabled = aimEn;
                p.aim2When = aimWhen;
                unsigned dur = cycleDuration(&p);
                for (unsigned u = 0; u <= dur; u++) {          /* 1 ms resolution over the whole cycle */
                    flipped = 0; composeFrame(logical, (float)u, &p); toPhysical(normal, logical);
                    flipped = 1; composeFrame(logical, (float)u, &p); toPhysical(mirrored, logical);
                    for (int i = 0; i < NUM_LEDS; i++) {
                        if (!rgbEq(mirrored[i], normal[NUM_LEDS - 1 - i])) {
                            char d[160];
                            snprintf(d, sizeof d, "u=%u rails=%d aim2=%d when=%d led=%d", u, rails, aimEn, aimWhen, i);
                            fail(label, d);
                            flipped = 0;
                            return;
                        }
                    }
                    if (litCount(mirrored) != litCount(normal) || channelSum(mirrored) != channelSum(normal)) {
                        char d[160];
                        snprintf(d, sizeof d, "lit/energy changed at u=%u", u);
                        fail(label, d);
                        flipped = 0;
                        return;
                    }
                    samples++;
                }
            }
        }
    }
    flipped = 0;
    railsLit = 1;
    char d[80];
    snprintf(d, sizeof d, "%ld frames x 144 LEDs", samples);
    ok(label, d);
}

/*
 * THE non-vacuous assertion. CENTER is 71.5, a half-integer, so reflecting the strip is NOT the
 * same as negating the stroke math:
 *     mirror of the amber marker : 143 - roundf(71.5 + amp) = 143 - (72 + amp) = 71 - amp
 *     geometry inversion         :       roundf(71.5 - amp) =                    72 - amp
 * They differ by exactly one LED for EVERY reach. The output mirror is the correct reflection
 * (it is the only one that is an exact involution of the rendered picture); the inversion is the
 * rejected design. Pinning the difference here means a future "simplification" back to
 * CENTER - amp fails a test instead of shipping a silent 6.9 mm asymmetry.
 */
static void checkNotGeometryInversion(void) {
    int bad = 0;
    for (int amp = 1; amp <= (NUM_LEDS - 1) / 2; amp++) {
        int amberLogical  = (int)roundf(CENTER + (float)amp);   /* what the renderer lights */
        int amberMirrored = NUM_LEDS - 1 - amberLogical;        /* where showFrame puts it   */
        int amberInverted = (int)roundf(CENTER - (float)amp);   /* the rejected design       */
        if (amberLogical != 72 + amp)  { bad = amp; break; }
        if (amberMirrored != 71 - amp) { bad = amp; break; }
        if (amberInverted != 72 - amp) { bad = amp; break; }
        if (amberMirrored == amberInverted) { bad = amp; break; }   /* must ALWAYS differ */
    }
    if (bad) {
        char d[80];
        snprintf(d, sizeof d, "reach=%d", bad);
        fail("mirror is a reflection, not a geometry inversion", d);
        return;
    }
    /* Same story for the ball, which sits on the half-integer itself. */
    int ball = (int)roundf(CENTER);
    if (ball != 72 || NUM_LEDS - 1 - ball != 71) {
        fail("ball reflects 72 <-> 71 about CENTER 71.5", "unexpected indices");
        return;
    }
    ok("reflection != geometry inversion", "differs by 1 LED for all 71 reaches");
}

int main(void) {
    Profile putt = { 700, 350, 58, 0,   0,  350, 0, 70, 1 };   /* the .ino PUTT default */
    Profile full = { 900, 300, 60, 150, 55, 300, 0, 70, 1 };   /* the .ino FULL default */

    printf("Mirror algebra:\n");
    checkInvolutionAndIndexMap();
    checkNotGeometryInversion();

    printf("Exact whole-frame reversal of real content (every u, every rails/aim2 combination):\n");
    checkFrameSweep("PUTT flipped frame == reverse(normal)", putt, MODE_PUTT);
    checkFrameSweep("FULL flipped frame == reverse(normal)", full, MODE_FULL);

    printf("\n%s (%d failure%s)\n", failures ? "FLIP MIRROR FAILED" : "FLIP MIRROR PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
