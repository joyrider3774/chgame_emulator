#!/bin/sh
# Builds CHCasino's CHGame bootloader with the SD game menu
# (https://github.com/bateske/CHCasino/tree/main/platform/bootloader, release
# mode) and puts only the binary in bootloaders/ (gitignored), for the
# emulator's --bootloader:
#
#   bootloaders/chgame_sdboot.bin
#
# Built with the toolchain of the CHGame board package (its build.sh finds it)
# in a temporary copy of the source, deleted afterwards, so neither the
# CHCasino clone (c:/github/CHCasino, pulled first) nor this repository keeps
# any of it.
#
# Usage: tools/build_bootloaders.sh

GITHUB=${GITHUB:-/c/github}
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC="$GITHUB/CHCasino/platform/bootloader"
OUT="$HERE/bootloaders"

[ -d "$SRC" ] || { echo "no $SRC: clone https://github.com/bateske/CHCasino into $GITHUB"; exit 1; }
git -C "$GITHUB/CHCasino" pull -q 2>/dev/null

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -r "$SRC/." "$WORK/"
rm -rf "$WORK/build"
mkdir -p "$OUT"

if bash "$WORK/build.sh" release > "$WORK/build.log" 2>&1 && [ -f "$WORK/build/release/chgame_boot.bin" ]; then
    cp "$WORK/build/release/chgame_boot.bin" "$OUT/chgame_sdboot.bin"
    echo "ok      bootloaders/chgame_sdboot.bin  $(wc -c < "$OUT/chgame_sdboot.bin") bytes"
else
    tail -20 "$WORK/build.log"
    echo "FAILED  chgame_sdboot.bin"
    exit 1
fi
