#!/bin/sh
# The one way to build a board. Runs the full pipeline and ALWAYS shuts the Podman VM
# down afterwards, even on failure or Ctrl-C:
#
#   sh pcb/build.sh [board]            # default board: osstempo_led_carrier
#
# 1. `board validate` twice - the second identical run is what promotes an immutable
#    fabrication release (ERC/DRC/evidence gates, KiBot fab export in the container).
# 2. reseed-routed.sh - copy the promoted release into pcb/routed/<board>/ and re-run DRC.
# 3. tools/make_jlc_order.py - the gerber zip + BOM + CPL for jlcpcb.com.
#
# Why the shutdown: the KiBot export needs the Podman machine (a Linux VM), and the VM
# keeps 2 CPU cores and 4 GB of RAM reserved for as long as it runs. Nothing else here
# uses it, so it should only be alive while a build is in progress. The VM is started
# here if it is not already running; the EXIT trap stops it regardless of who started it.
#
# KICAD_AGENT may be set beforehand to build with a different toolkit checkout (env.sh
# otherwise points at ../kicad-agent-toolkit); it is kept across the env.sh source.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BOARD="${1:-osstempo_led_carrier}"
PRESET_AGENT="${KICAD_AGENT:-}"
. "$HERE/env.sh"
[ -n "$PRESET_AGENT" ] && KICAD_AGENT="$PRESET_AGENT"
PODMAN=/opt/homebrew/bin/podman

vm_running() { "$PODMAN" machine list --format '{{.Running}}' 2>/dev/null | grep -q true; }

stop_vm() {
  status=$?
  echo "== stopping the podman VM"
  "$PODMAN" machine stop >/dev/null 2>&1 || true
  if vm_running; then
    echo "   !! podman VM still running - stop it by hand: podman machine stop" >&2
  else
    echo "   stopped"
  fi
  exit $status
}
trap stop_vm EXIT INT TERM

if ! vm_running; then
  echo "== starting the podman VM"
  "$PODMAN" machine start
fi

echo "== validate (1/2)"
"$KICAD_AGENT" board validate "$BOARD" || true     # first run: BLOCKED is expected (needs a 2nd match)
echo "== validate (2/2)"
"$KICAD_AGENT" board validate "$BOARD"
echo "== reseed routed/"
sh "$HERE/reseed-routed.sh" "$BOARD"
echo "== JLC order files"
python3 "$HERE/tools/make_jlc_order.py"
echo "== done: $HERE/build/jlcpcb_order/"
