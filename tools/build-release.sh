#!/bin/bash
# build-release.sh: build the R7_HermesTalk release firmware WITHOUT secrets.h and package the
# separate flash parts for esptool (see RELEASE.md).
#
#   tools/build-release.sh [--libraries DIR]
#
# Output: dist/stackchan-r7-v<version>/ and dist/stackchan-r7-v<version>.zip (dist/ is git-ignored)
#
# Never ships R7_HermesTalk.ino.merged.bin: that is a full 16 MB image, and writing it would
# erase the settings area at 0x9000 (Wi-Fi, secrets AND the servo calibration).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBS="$HOME/Documents/Arduino/libraries"
FQBN="esp32:esp32:m5stack_cores3"   # board defaults: USB CDC on boot, 16 MB, app3M_fat9M_16MB
while [ $# -gt 0 ]; do
  case "$1" in
    --libraries) LIBS="$2"; shift 2 ;;
    *) echo "usage: $0 [--libraries DIR]" >&2; exit 2 ;;
  esac
done
[ -d "$LIBS" ] || { echo "library folder not found: $LIBS (use --libraries)" >&2; exit 1; }

SRC="$REPO/R7_HermesTalk"
VERSION="$(sed -n 's/^static const char\* SKETCH_VERSION *= *"\([0-9.]*\)";.*/\1/p' "$SRC/R7_HermesTalk.ino")"
[ -n "$VERSION" ] || { echo "cannot read SKETCH_VERSION" >&2; exit 1; }
NAME="stackchan-r7-v$VERSION"
OUT="$REPO/dist/$NAME"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/R7_HermesTalk"
# Only the sketch and its headers: no secrets.h, ever
cp "$SRC/R7_HermesTalk.ino" "$SRC/move_rules.h" "$SRC/move_outcome.h" "$SRC/robot_config.h" "$WORK/R7_HermesTalk/"
[ ! -e "$WORK/R7_HermesTalk/secrets.h" ] || { echo "secrets.h in the build folder" >&2; exit 1; }

echo "Building R7_HermesTalk v$VERSION ($FQBN, libraries: $LIBS)"
arduino-cli compile --fqbn "$FQBN" --warnings all --libraries "$LIBS" \
  --build-path "$WORK/build" "$WORK/R7_HermesTalk" > "$WORK/compile.log" 2>&1 || {
  cat "$WORK/compile.log" >&2; exit 1; }
grep -E "^Sketch uses" "$WORK/compile.log"
# Library names and versions for BUILD-INFO.txt, from each used library's library.properties
LIBLIST="$(grep -oE "$LIBS/[^/]+|/Arduino15/packages/esp32/hardware/esp32/[^/]+/libraries/[^/]+" \
  "$WORK/build/includes.cache" 2>/dev/null | sort -u | while read -r d; do
    [ -f "$d/library.properties" ] || d="$HOME/Library$d"
    [ -f "$d/library.properties" ] && sed -n 's/^name=//p;s/^version=//p' "$d/library.properties" | paste -sd' ' -
  done | sort -u)"

# The parts and offsets must be exactly these four (no write at 0x9000-0xdfff)
EXPECTED=$'0x0 R7_HermesTalk.ino.bootloader.bin\n0x8000 R7_HermesTalk.ino.partitions.bin\n0xe000 boot_app0.bin\n0x10000 R7_HermesTalk.ino.bin'
[ "$(sed -n '2,$p' "$WORK/build/flash_args")" = "$EXPECTED" ] || {
  echo "unexpected flash layout:" >&2; cat "$WORK/build/flash_args" >&2; exit 1; }
grep -q '^nvs, *data, *nvs, *0x9000, *0x5000' "$WORK/build/partitions.csv" || {
  echo "settings area is not at 0x9000 (size 0x5000)" >&2; exit 1; }

rm -rf "$OUT" "$OUT.zip"
mkdir -p "$OUT"
cp "$WORK/build/R7_HermesTalk.ino.bootloader.bin" "$OUT/bootloader.bin"
cp "$WORK/build/R7_HermesTalk.ino.partitions.bin" "$OUT/partitions.bin"
cp "$WORK/build/boot_app0.bin" "$OUT/boot_app0.bin"
cp "$WORK/build/R7_HermesTalk.ino.bin" "$OUT/R7_HermesTalk-v$VERSION.bin"

# No secrets in the binaries: none of this Mac's Keychain values, no Wi-Fi password placeholder
for item in stackchan-r1-token stackchan-webhook-secret; do
  value="$(security find-generic-password -s "$item" -w 2>/dev/null || true)"
  if [ -n "$value" ] && grep -rqF -- "$value" "$OUT"; then
    echo "STOP: a Keychain value ($item) is inside the build" >&2; rm -rf "$OUT"; exit 1
  fi
done
unset value
if grep -rqF "your-home-wifi-password" "$OUT"; then
  echo "STOP: secrets.h placeholders are inside the build" >&2; rm -rf "$OUT"; exit 1
fi

CORE="$(arduino-cli core list 2>/dev/null | awk '$1=="esp32:esp32"{print $2}')"
GIT="$(git -C "$REPO" rev-parse --short HEAD)$(git -C "$REPO" diff --quiet -- R7_HermesTalk || echo '+changes')"
{
  echo "R7_HermesTalk v$VERSION for M5Stack StackChan (CoreS3)"
  echo "Built $(date -u +%Y-%m-%dT%H:%MZ) from commit $GIT"
  echo "Board $FQBN, esp32 core $CORE, flash 16MB dio 80m, partitions app3M_fat9M_16MB"
  echo "Libraries:"
  echo "${LIBLIST:-  (not found)}" | sed 's/^/  /'
  echo
  echo "Flash (never write anything to 0x9000-0xdfff, never erase the whole flash):"
  echo "  esptool --chip esp32s3 --port PORT write-flash \\"
  echo "    0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 R7_HermesTalk-v$VERSION.bin"
} > "$OUT/BUILD-INFO.txt"
(cd "$OUT" && shasum -a 256 bootloader.bin partitions.bin boot_app0.bin "R7_HermesTalk-v$VERSION.bin" > SHA256SUMS)
(cd "$REPO/dist" && zip -qr "$NAME.zip" "$NAME")

echo "Release files: dist/$NAME/ and dist/$NAME.zip"
cat "$OUT/SHA256SUMS"
