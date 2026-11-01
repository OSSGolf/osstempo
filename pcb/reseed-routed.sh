#!/bin/sh
# Refresh pcb/routed/<board>/ from the toolkit's current accepted release.
#
#   sh pcb/reseed-routed.sh <board>
#
# pcb/routed/ is a DERIVED artifact: a byte copy of the promoted release project, so it
# opens in KiCad without the toolkit installed and can be diffed in git. Everything in
# it - placement, netlist, copper, silkscreen, artwork - comes from
# examples/<board>/*.yaml and regenerates through `board validate`. Do not edit it in
# the GUI; change the YAML and re-run.
set -e
BOARD="${1:?usage: reseed-routed.sh <board>}"
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/env.sh"

CUR="$HERE/build/kicad-agent/$BOARD/current/project"
[ -d "$CUR" ] || { echo "no accepted release for $BOARD - run: \$KICAD_AGENT board validate $BOARD (twice)"; exit 1; }

DEST="$HERE/routed/$BOARD"
if [ -d "$DEST" ] && (cd "$HERE/.." && git status --porcelain "pcb/routed/$BOARD" 2>/dev/null | grep -qv '^??'); then
  echo "REFUSING: pcb/routed/$BOARD has uncommitted modifications (GUI edits?). Commit or discard them first."
  exit 1
fi
# Releases are published read-only and cp -R preserves that; the routed copy is meant
# to be opened and diffed, so make it (and any stale previous copy) writable.
[ -d "$DEST" ] && chmod -R u+w "$DEST"
rm -rf "$DEST"; mkdir -p "$HERE/routed"; cp -R "$CUR" "$DEST"; chmod -R u+w "$DEST"
echo "pcb/routed/$BOARD <- $(readlink "$HERE/build/kicad-agent/$BOARD/current")"
kicad-cli pcb drc --severity-all --exit-code-violations -o "$TMPDIR/reseed-drc.json" --format json "$DEST/$BOARD.kicad_pcb" >/dev/null 2>&1 \
  && echo "DRC: clean" || { echo "DRC: VIOLATIONS - see $TMPDIR/reseed-drc.json"; exit 1; }
