#!/bin/sh
# Builds the CHGame bootloader with the SD game menu from bateske/CHGame
# (https://github.com/bateske/CHGame/tree/main/platform/bootloader, release
# mode) and puts only the binary in bootloaders/ (gitignored), for the
# emulator's --bootloader:
#
#   bootloaders/chgame_sdboot.bin
#
# Built with the toolchain of the CHGame board package (its build.sh finds it)
# in a temporary copy of the source, deleted afterwards, so neither the
# CHGame clone (c:/github/CHGame, pulled first) nor this repository keeps
# any of it.
#
# Usage: tools/build_bootloaders.sh

GITHUB=${GITHUB:-/c/github}
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC="$GITHUB/CHGame/platform/bootloader"
OUT="$HERE/bootloaders"

[ -d "$SRC" ] || { echo "no $SRC: clone https://github.com/bateske/CHGame into $GITHUB"; exit 1; }
git -C "$GITHUB/CHGame" pull -q 2>/dev/null

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
