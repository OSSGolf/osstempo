#!/bin/sh
# check_show_chokepoint.sh — structural guard for the 180° flip.
# =============================================================================
# The flip is implemented as a single output transform: the renderer composes every frame in
# LOGICAL index space into leds[], and showFrame() is the ONLY thing that copies that frame to
# the physical strip, mirroring it end-for-end when `flipped` is set. That design is total —
# no LED element can escape the mirror — but ONLY for as long as every render path actually
# goes through showFrame().
#
# So this asserts the invariant mechanically, over tempo_trainer/ (the built sketch; the
# docs/ snapshots and spikes/ are separate sketches and out of scope):
#
#   1. FastLED.show()     appears exactly once, and inside showFrame().
#   2. FastLED.clear(true) appears nowhere — it calls showColor(), a SECOND output path that
#      bypasses leds[] entirely and would paint unmirrored.
#   3. FastLED.clear()    appears nowhere — it clears the array bound to the controller, which
#      is physLeds[], so the renderer would compose onto a stale logical frame.
#   4. physLeds[]         is touched only by showFrame() and the FastLED binding.
#   5. addLeds             binds physLeds[], not leds[]. This is the single most load-bearing line
#                          in the design — bind the logical buffer instead and the flip silently
#                          stops working entirely, with everything else still compiling and passing.
#   6. showFrame()         copies the WHOLE strip, both ways (a short loop bound would drop LEDs).
#   7. `flipped`           is read only where it may be: the output copy, persistence, the GATT
#                          handler, the confirm tag, and the sync revert. Never by the renderer —
#                          that is what makes the flip a pure output transform, and no host test
#                          can check it, because a host test does not read the .ino.
#
# A new FastLED.show() added anywhere else is the realistic future regression — an element that
# renders unmirrored on a flipped stick — and a compile cannot catch it. This can.
#
# Comments are stripped before matching, so prose that mentions these names does not trip it.
#
# Run:  sh firmware_tests/check_show_chokepoint.sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
files="$root/tempo_trainer/tempo_trainer.ino $root/tempo_trainer/sync_firmware.ino"
fail=0

say_fail() { printf '  FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_ok()   { printf '  ok   %s\n' "$1"; }

# Code-only view of a file: blank out // and /* */ comments, keep line numbering intact.
# (Block comments are handled line-wise, which is enough for this codebase's style and avoids a
#  spurious CI failure when prose mentions one of the banned names.)
code() {
  sed -e 's|//.*||' -e 's|/\*[^*]*\*/||g' -e '\|/\*|,\|\*/|s|.*||' "$1" \
    | grep -n "$2" | sed "s|^|$1:|"
}

matches() {
  pattern=$1
  for f in $files; do code "$f" "$pattern" || true; done
}

# --- 1. FastLED.show() exactly once, inside showFrame() --------------------------------
shows=$(matches 'FastLED\.show()')
count=$(printf '%s' "$shows" | grep -c . || true)

if [ "$count" -eq 0 ]; then
  say_fail "FastLED.show() not found at all — did the renderer move?"
elif [ "$count" -ne 1 ]; then
  say_fail "FastLED.show() appears $count times; it must appear once, inside showFrame()."
  printf '%s\n' "$shows" | sed 's/^/         /'
  printf '       Every render path must latch through showFrame(), or it paints unmirrored\n'
  printf '       on a flipped stick. Replace the extra call(s) with showFrame().\n'
else
  file=${shows%%:*}
  line=$(printf '%s' "$shows" | cut -d: -f2)
  def=$(grep -n 'void showFrame()' "$file" | head -1 | cut -d: -f1 || true)
  if [ -z "$def" ]; then
    say_fail "showFrame() is not defined in $(basename "$file") — the choke point is gone."
  else
    end=$(awk -v s="$def" 'NR > s && /^}/ { print NR; exit }' "$file")
    if [ -n "$end" ] && [ "$line" -gt "$def" ] && [ "$line" -lt "$end" ]; then
      say_ok "FastLED.show() appears exactly once, inside showFrame() ($(basename "$file"):$line)"
    else
      say_fail "the single FastLED.show() at $(basename "$file"):$line is OUTSIDE showFrame() (lines $def-${end:-?})"
    fi
  fi
fi

# --- 2/3. No other output or clear path ------------------------------------------------
for pat_desc in 'FastLED\.clear(true)|calls showColor(), a second output path that bypasses leds[]' \
                'FastLED\.clear()|clears physLeds[] (the bound array), not the logical frame — use clearLeds()' \
                'FastLED\.showColor|is a second output path that never reads the frame buffer at all' \
                'FastLED\.clearData|writes the bound array behind the renderer' \
                'FastLED\.delay|latches repeatedly outside showFrame()'; do
  pat=${pat_desc%%|*}; why=${pat_desc#*|}
  hits=$(matches "$pat")
  if [ -n "$hits" ]; then
    say_fail "$(printf '%s' "$pat" | tr -d '\\\\') found — it $why:"
    printf '%s\n' "$hits" | sed 's/^/         /'
  else
    say_ok "no $(printf '%s' "$pat" | tr -d '\\\\')"
  fi
done

# --- 4. physLeds[] is private to showFrame() (+ the addLeds binding) --------------------
stray=''
for f in $files; do
  def=$(grep -n 'void showFrame()' "$f" | head -1 | cut -d: -f1 || true)
  end=''
  [ -n "$def" ] && end=$(awk -v s="$def" 'NR > s && /^}/ { print NR; exit }' "$f")
  hits=$(code "$f" 'physLeds' || true)
  printf '%s\n' "$hits" | while IFS= read -r l; do
    [ -z "$l" ] && continue
    n=$(printf '%s' "$l" | cut -d: -f2)
    case $l in *addLeds*|*"CRGB physLeds["*) continue ;; esac
    if [ -z "$def" ] || [ "$n" -le "$def" ] || [ "$n" -ge "${end:-0}" ]; then
      printf '%s\n' "$l" >> "${TMPDIR:-/tmp}/chokepoint_stray.$$"
    fi
  done
done
if [ -f "${TMPDIR:-/tmp}/chokepoint_stray.$$" ]; then
  say_fail "physLeds[] touched outside showFrame() — that composes in PHYSICAL space, unmirrored:"
  sed 's/^/         /' "${TMPDIR:-/tmp}/chokepoint_stray.$$"
  rm -f "${TMPDIR:-/tmp}/chokepoint_stray.$$"
else
  say_ok "physLeds[] is private to showFrame() (+ the FastLED binding)"
fi

# --- 5. addLeds must bind the PHYSICAL buffer -------------------------------------------
bind=$(matches 'addLeds')
if [ -z "$bind" ]; then
  say_fail "no addLeds call found — where is the strip bound?"
elif printf '%s' "$bind" | grep -q 'physLeds'; then
  say_ok "addLeds binds physLeds[] (the flip's output buffer)"
else
  say_fail "addLeds does NOT bind physLeds[] — the flip silently does nothing:"
  printf '%s\n' "$bind" | sed 's/^/         /'
fi

# --- 6. showFrame() must copy the whole strip, both ways ---------------------------------
sf=$(grep -n 'void showFrame()' "$root/tempo_trainer/tempo_trainer.ino" | head -1 | cut -d: -f1 || true)
if [ -n "$sf" ]; then
  sfend=$(awk -v s="$sf" 'NR > s && /^}/ { print NR; exit }' "$root/tempo_trainer/tempo_trainer.ino")
  bounds=$(sed -n "${sf},${sfend}p" "$root/tempo_trainer/tempo_trainer.ino" | grep -c 'i < NUM_LEDS;' || true)
  if [ "$bounds" -eq 2 ]; then
    say_ok "showFrame() copies the full strip in both orientations"
  else
    say_fail "showFrame() has $bounds full-length loop bounds, expected 2 (i < NUM_LEDS) — a short bound drops LEDs"
  fi
fi

# --- 7. the renderer must never read `flipped` ------------------------------------------
# This is the invariant that makes the flip a pure OUTPUT transform, and it is the one thing no
# host test can check — a host test re-implements the renderer, it does not read the .ino. Stated
# positively, per function, rather than as an allowlist of legitimate lines: the question is which
# function a read sits in, not what the line looks like.
# `loop` is in the list because it composes the idle frame itself (fadeToBlackBy + the centre dot),
# exactly as followerRenderStep does. Its one legitimate read of the flag lives behind
# flipConfirmTag() for precisely this reason.
renderers="loop renderFrameAt drawCometAnalytic renderDotMap showMarkers showRefs positionAt strokeDirAt aim2LitAt followerRenderStep"
flip_leak=''
for f in $files; do
  # Anchor on the comment-stripped view and require an actual definition, so prose mentioning a
  # function name (e.g. "debounced-saved in loop()") cannot anchor the range at the wrong line.
  stripped=$(mktemp)
  sed -e 's|//.*||' -e 's|/\*[^*]*\*/||g' -e '\|/\*|,\|\*/|s|.*||' "$f" > "$stripped"
  for fn in $renderers; do
    d=$(grep -n "^[A-Za-z].*[ *]$fn([^;]*) *{ *$" "$stripped" | head -1 | cut -d: -f1 || true)
    [ -z "$d" ] && continue
    e=$(awk -v s="$d" 'NR > s && /^}/ { print NR; exit }' "$stripped")
    [ -z "$e" ] && continue
    hit=$(sed -n "${d},${e}p" "$stripped" | grep -n 'flipped' || true)
    [ -z "$hit" ] && continue
    # Report the real file line, not the offset within the range.
    for h in $hit; do
      rel=${h%%:*}
      flip_leak="$flip_leak$(basename "$f"):$((d + rel - 1)) in $fn(): $(printf '%s' "$h" | cut -d: -f2-)
"
    done
  done
  rm -f "$stripped"
done
if [ -n "$flip_leak" ]; then
  say_fail "the renderer reads 'flipped' — the flip must be an OUTPUT transform, not baked into composition:"
  printf '%s' "$flip_leak" | sed 's/^/         /'
else
  say_ok "no renderer function reads 'flipped' (it is a pure output transform)"
fi

printf '\n'
if [ "$fail" -eq 0 ]; then
  printf 'SHOW CHOKEPOINT PASSED (0 failures)\n'
  exit 0
fi
printf 'SHOW CHOKEPOINT FAILED (%d failures)\n' "$fail"
exit 1
