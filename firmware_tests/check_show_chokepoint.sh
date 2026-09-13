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

# Code-only view of a file: blank out // comments, keep line numbering intact.
code() { sed 's|//.*||' "$1" | grep -n "$2" | sed "s|^|$1:|"; }

scan() {
  pattern=$1
  out=''
  for f in $files; do
    hits=$(code "$f" "$pattern" || true)
    [ -n "$hits" ] && out="$out$hits
"
  done
  printf '%s' "$out" | grep -c . 2>/dev/null || true
  :
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
                'FastLED\.clear()|clears physLeds[] (the bound array), not the logical frame — use clearLeds()'; do
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

printf '\n'
if [ "$fail" -eq 0 ]; then
  printf 'SHOW CHOKEPOINT PASSED (0 failures)\n'
  exit 0
fi
printf 'SHOW CHOKEPOINT FAILED (%d failures)\n' "$fail"
exit 1
