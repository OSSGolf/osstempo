#!/bin/sh
# Compile the tempo_trainer firmware and flash it to an ESP32 over USB.
#
#   sh tools/flash.sh                    compile, then flash the one ESP32 that is plugged in
#   sh tools/flash.sh --loop             flash a batch: swap boards between prompts, compile once
#   sh tools/flash.sh --port /dev/cu.X   flash a specific port (needed when several are plugged in)
#   sh tools/flash.sh --dig2go           build for the QuinLED-Dig2go instead of the generic ESP32
#   sh tools/flash.sh --compile-only     compile gate only, no board needed
#
# Why this wraps arduino-cli instead of calling it bare: arduino-cli's bundled ctags
# (5.8-arduino11) ships Intel-only for macOS. On an Apple-silicon Mac without Rosetta every
# compile dies with "ctags: bad CPU type in executable". When that is the case this script builds
# Arduino's ctags natively, once, into a per-user cache and points the compile at it. The Arduino
# install itself is never modified.
set -eu

FQBN="esp32:esp32:esp32:PartitionScheme=huge_app"
SKETCH="tempo_trainer"
CTAGS_REPO="https://github.com/arduino/ctags.git"
CTAGS_TAG="5.8-arduino11"
CTAGS_COMMIT="abc8fca7499f44c725122881cd380a88c37abe0e"   # what the tag pointed at when pinned

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/osstempo"
CTAGS_DIR="$CACHE/ctags-$CTAGS_TAG"
CTAGS_BIN="$CTAGS_DIR/ctags"

board="diy"
port=""
compile_only=0
loop=0

die() { echo "flash.sh: $*" >&2; exit 1; }

usage() { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --dig2go)       board="dig2go" ;;
    --port)         [ $# -ge 2 ] || die "--port needs a value"; port="$2"; shift ;;
    --compile-only) compile_only=1 ;;
    --loop)         loop=1 ;;
    -h|--help)      usage; exit 0 ;;
    *)              die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

[ "$compile_only" -eq 1 ] && [ "$loop" -eq 1 ] && die "--compile-only and --loop do not combine"
[ -n "$port" ] && [ "$loop" -eq 1 ] && die "--port and --loop do not combine (--loop finds each board itself)"
command -v arduino-cli >/dev/null 2>&1 || die "arduino-cli is not installed (see docs/full-diy/README.md)"

# The bundled ctags is unusable exactly when this is an arm64 Mac that cannot run x86_64 code.
needs_native_ctags() {
  [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ] && ! arch -x86_64 /usr/bin/true 2>/dev/null
}

build_native_ctags() {
  echo "==> Building a native ctags (one-time; the bundled one is Intel-only and Rosetta is absent)"
  command -v git  >/dev/null 2>&1 || die "git is needed to fetch the ctags source"
  command -v make >/dev/null 2>&1 || die "make is needed to build ctags (xcode-select --install)"
  src="$CTAGS_DIR.src.$$"
  rm -rf "$src"
  mkdir -p "$CACHE"
  git -c advice.detachedHead=false clone -q --depth 1 --branch "$CTAGS_TAG" "$CTAGS_REPO" "$src"
  got=$(git -C "$src" rev-parse HEAD)
  if [ "$got" != "$CTAGS_COMMIT" ]; then
    rm -rf "$src"
    die "ctags tag $CTAGS_TAG is at $got, expected $CTAGS_COMMIT; refusing to build an unpinned source"
  fi
  # The subshell is the left side of ||, where `set -e` is off, so every step is chained by hand.
  (
    cd "$src" &&
    # general.h defines __unused__ as a macro, which collides with the macOS SDK's own use of it.
    # LC_ALL=C: some sources are Latin-1, which a UTF-8 locale's sed refuses to read.
    LC_ALL=C sed -i '' 's/__unused__/__ctags_unused__/g' ./*.c ./*.h &&
    # 2009-era K&R C: modern clang rejects it without these.
    { ./configure CFLAGS="-O2 -std=gnu89 -Wno-implicit-function-declaration -Wno-implicit-int -Wno-deprecated-non-prototype" >configure.log 2>&1 \
        || { tail -20 configure.log >&2; exit 1; }; } &&
    { make >make.log 2>&1 || { tail -30 make.log >&2; exit 1; }; } &&
    ./ctags --version >/dev/null
  ) || { echo "flash.sh: ctags build failed; source left in $src" >&2; exit 1; }
  mkdir -p "$CTAGS_DIR"
  cp "$src/ctags" "$CTAGS_BIN"
  rm -rf "$src"
  echo "==> Native ctags installed at $CTAGS_BIN"
}

# Candidate USB-serial ports, one per line: CP2102, CH340 and FTDI bridges on macOS and Linux.
list_ports() {
  for p in /dev/cu.usbserial-* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial* /dev/ttyUSB* /dev/ttyACM*; do
    [ -e "$p" ] && echo "$p"
  done
  return 0
}

# Print the single plugged-in port, or explain why there is not exactly one.
pick_port() {
  ports=$(list_ports)
  count=$(printf '%s\n' "$ports" | grep -c . || true)
  if [ "$count" -eq 0 ]; then
    echo "flash.sh: no ESP32 serial port found. Check that the USB cable carries data (not" >&2
    echo "          charge-only) and that the CP2102/CH340 driver is installed." >&2
    return 1
  fi
  if [ "$count" -gt 1 ]; then
    echo "flash.sh: several serial ports are plugged in; pick one with --port:" >&2
    printf '%s\n' "$ports" | sed 's/^/          /' >&2
    return 1
  fi
  printf '%s\n' "$ports"
}

upload() {
  echo "==> Flashing $1"
  if ! arduino-cli upload --fqbn "$FQBN" -p "$1" --input-dir "$BUILD_DIR" "$REPO_ROOT/$SKETCH"; then
    echo "flash.sh: upload to $1 failed. If it stalled at 'Connecting....', hold the BOOT button" >&2
    echo "          while it connects and try again." >&2
    return 1
  fi
  echo "==> Flashed $1 ($board build)"
}

# ---- compile ----
BUILD_DIR="$CACHE/build-$board"

set -- compile --fqbn "$FQBN" --warnings all --build-path "$BUILD_DIR"
if needs_native_ctags; then
  [ -x "$CTAGS_BIN" ] && "$CTAGS_BIN" --version >/dev/null 2>&1 || build_native_ctags
  set -- "$@" --build-property "tools.ctags.cmd.path=$CTAGS_BIN"
fi
# The sketch defaults to the generic ESP32 (BOARD_DIY_ESP32); only the Dig2go needs a flag.
if [ "$board" = "dig2go" ]; then
  set -- "$@" --build-property "compiler.cpp.extra_flags=-DBOARD_DIG2GO"
fi

echo "==> Compiling $SKETCH ($board build)"
arduino-cli "$@" "$REPO_ROOT/$SKETCH"

[ "$compile_only" -eq 1 ] && exit 0

# ---- flash ----
if [ "$loop" -eq 1 ]; then
  n=0
  while :; do
    printf '\nPlug in the next ESP32 (only one at a time), then press Enter. Type q to finish: '
    read -r answer || break
    [ "$answer" = "q" ] && break
    p=$(pick_port) || continue
    if upload "$p"; then n=$((n + 1)); fi
  done
  echo "==> Done: $n board(s) flashed"
  exit 0
fi

[ -n "$port" ] || port=$(pick_port)
[ -e "$port" ] || die "port $port does not exist"
upload "$port"
