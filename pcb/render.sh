#!/bin/sh
# Render a board to the PNGs the READMEs embed:
#
#   sh pcb/render.sh                             # routed/<board>/ -> renders/v<rev>-{top,bottom}.png
#   sh pcb/render.sh <file.kicad_pcb> <label>    # any board, e.g. an archived revision
#
# The label defaults to the manifest's `revision:`. Output is 1000 x 1400 px, which is
# the 50 x 70 mm board's own aspect ratio.
#
# This is the 3D renderer, good for a photo-like overview and nothing more. It is NOT
# the check for silkscreen correctness: a large silk polygon with many holes can draw
# wrong here while the plotted gerber is fine (see CLAUDE.md). Check silkscreen with
# `kicad-cli pcb export svg --layers B.Silkscreen,Edge.Cuts --mirror` instead.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BOARD=osstempo_led_carrier
KICAD_CLI="${KICAD_CLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}"
PCB="${1:-$HERE/routed/$BOARD/$BOARD.kicad_pcb}"
LABEL="${2:-$(sed -n 's/^ *revision: *//p' "$HERE/examples/$BOARD/${BOARD}_agent_manifest.yaml" | head -1)}"
OUT="$HERE/renders"

[ -f "$PCB" ] || { echo "no such board: $PCB" >&2; exit 1; }
[ -n "$LABEL" ] || { echo "no revision label (pass one as the 2nd argument)" >&2; exit 1; }
mkdir -p "$OUT"

for side in top bottom; do
  png="$OUT/$LABEL-$side.png"
  # kicad-cli chatters on stdout and prints a harmless wxApp assertion on stderr;
  # show all of it only when the render actually fails.
  if ! log=$("$KICAD_CLI" pcb render --side "$side" --width 1000 --height 1400 \
               --quality high --background opaque -o "$png" "$PCB" 2>&1); then
    echo "$log" >&2
    echo "render failed: $png" >&2
    exit 1
  fi
  echo "$png"
done
