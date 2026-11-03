#!/bin/sh
# One-time setup of the sibling kicad-agent-toolkit checkout for this board on macOS.
# Idempotent - safe to re-run.
#
#   sh pcb/setup-toolkit.sh
#
# Assumes: KiCad 10 in /Applications, Homebrew, and ../kicad-agent-toolkit cloned
# (it is a private repo - ask its owner for access; nothing from it is vendored here).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TK="$HERE/../../kicad-agent-toolkit"
BRANCH=osstempo-patches-v2
[ -d "$TK" ] || { echo "clone georgemclaughlin/kicad-agent-toolkit next to this repo first"; exit 1; }

echo "== toolkit branch ($BRANCH - upstream main plus the patches this board needs; see README.md)"
if (cd "$TK" && git rev-parse --verify -q "$BRANCH" >/dev/null); then
  (cd "$TK" && [ "$(git branch --show-current)" = "$BRANCH" ] || git checkout -q "$BRANCH") && echo "   on $BRANCH"
else
  echo "   !! branch $BRANCH not found in $TK - it lives only in the local clone that built this board"; exit 1
fi

echo "== toolkit venv"
[ -x "$TK/.venv/bin/kicad-agent" ] || (cd "$TK" && python3 -m venv .venv && .venv/bin/python -m pip install -q -e '.[dev]')

echo "== toolkit version pins (.ci/toolchain.json is the toolkit's file; gitignored here)"
mkdir -p "$HERE/.ci" && cp "$TK/.ci/toolchain.json" "$HERE/.ci/toolchain.json"

echo "== optional simulators / container runtime"
command -v ngspice >/dev/null || brew install ngspice
command -v /opt/homebrew/bin/podman >/dev/null || brew install podman
podman machine inspect >/dev/null 2>&1 || podman machine init --cpus 2 --memory 4096 --disk-size 20
podman machine list --format '{{.Running}}' 2>/dev/null | grep -q true || podman machine start

echo "== pinned KiBot container"
. "$HERE/env.sh"
"$KICAD_AGENT" toolchain setup-kibot

echo "== smoke"
"$KICAD_AGENT" board list
echo "ok - now: source pcb/env.sh && \$KICAD_AGENT board validate osstempo_led_carrier"
