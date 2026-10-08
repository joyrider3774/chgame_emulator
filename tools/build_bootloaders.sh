#!/bin/sh
# Builds the CHGame bootloaders from bateske/CHGame
# (https://github.com/bateske/CHGame/tree/main/platform/bootloader) and puts
# only the binaries in bootloaders/ (gitignored), for the emulator's
# --bootloader. Every variant its tools/dist.sh makes that is a bootloader,
# under the names the board package gives them (Tools > Bootloader):
#
#   chgame_sdboot.bin            SD Text Menu (Rainbow)    build.sh release
#   chgame_sdboot_static.bin     SD Text Menu (Static)     build.sh release --style=static
#   chgame_sdvisual.bin          SD Graphic Menu (Rainbow) build.sh release --ui=visual
#   chgame_sdvisual_static.bin   SD Graphic Menu (Static)  build.sh release --ui=visual --style=static
#   chgame_boot_nomenu.bin       USB Only                  build.sh nomenu
#   chgame_sdboot_locked.bin     the text menu without the bootloader update
#                                over USB (a production option, not in the
#                                board package)            build.sh locked
#
# The menus look as the card says: copy chg/ (tools/make_chg.py: the
# packages and the default MENU.BG, COVER.PIC, SYSTEM.PIC) into a card's
# GAMES folder.
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
git -C "$GITHUB/CHGame" pull -q --ff-only 2>/dev/null

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp -r "$SRC/." "$WORK/"
rm -rf "$WORK/build"
mkdir -p "$OUT"

# name|build.sh arguments|its output folder under build/
failed=0
while IFS='|' read -r name args dir; do
    # shellcheck disable=SC2086
    if bash "$WORK/build.sh" $args > "$WORK/$name.log" 2>&1 && [ -f "$WORK/build/$dir/chgame_boot.bin" ]; then
        cp "$WORK/build/$dir/chgame_boot.bin" "$OUT/$name.bin"
        echo "ok      bootloaders/$name.bin  $(wc -c < "$OUT/$name.bin") bytes"
    else
        tail -20 "$WORK/$name.log"
        echo "FAILED  $name.bin"
        failed=1
    fi
done <<EOF
chgame_sdboot|release|release
chgame_sdboot_static|release --style=static|release-static
chgame_sdvisual|release --ui=visual|release-visual
chgame_sdvisual_static|release --ui=visual --style=static|release-visual-static
chgame_boot_nomenu|nomenu|nomenu
chgame_sdboot_locked|locked|locked
EOF
exit $failed
