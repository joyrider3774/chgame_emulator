#!/bin/sh
# Builds the CHGame bootloaders from bateske/CHGame
# (https://github.com/bateske/CHGame/tree/main/platform/bootloader) and puts
# only the binaries in bootloaders/ (gitignored), for the emulator's
# --bootloader: Rev0's in bootloaders/, Rev1's (Rev1 and Rev 1.5) in
# bootloaders/rev1/, as the board package keeps them. Every variant its
# tools/dist.sh makes that is a bootloader, under the names the board package
# gives them (Tools > Bootloader):
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
# from bateske's main branch, fetched into the CHGame clone (c:/github/CHGame)
# and taken out with git archive into a temporary folder, deleted afterwards:
# the clone may be a fork behind it (the owner's is), and neither its branches
# and files nor this repository keep any of it.
#
# Usage: tools/build_bootloaders.sh

# c:/github on Windows, ~/github on Linux and WSL (as build_roms.sh); the
# CHGame clone is made there when it is missing. The toolchain is the board
# package's, which tools/build_roms.sh installs when it is missing.
if [ "$(uname -s)" = Linux ]; then
    GITHUB=${GITHUB:-$HOME/github}
else
    GITHUB=${GITHUB:-/c/github}
fi
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/bootloaders"

if [ ! -d "$GITHUB/CHGame/.git" ]; then
    mkdir -p "$GITHUB"
    git clone -q https://github.com/bateske/CHGame "$GITHUB/CHGame" && echo "cloned $GITHUB/CHGame" ||
        { echo "not cloned: bateske/CHGame"; exit 1; }
fi
# bateske's main into FETCH_HEAD: no branch, tag or file of the clone changes
git -C "$GITHUB/CHGame" fetch -q https://github.com/bateske/CHGame main ||
    { echo "not fetched: bateske/CHGame"; exit 1; }
echo "bateske/CHGame $(git -C "$GITHUB/CHGame" describe --tags FETCH_HEAD 2>/dev/null || git -C "$GITHUB/CHGame" rev-parse --short FETCH_HEAD)"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
git -C "$GITHUB/CHGame" archive FETCH_HEAD platform/bootloader | tar -x -C "$TMP" ||
    { echo "no platform/bootloader in bateske/CHGame (reorganised?)"; exit 1; }
WORK="$TMP/platform/bootloader"

# name|build.sh arguments|its output folder under build/ (rev1's get -rev1)
failed=0
for board in rev0 rev1; do
    if [ $board = rev0 ]; then dst="$OUT"; sfx=; rel=bootloaders
    else dst="$OUT/$board"; sfx="-$board"; rel="bootloaders/$board"; fi
    mkdir -p "$dst"
    while IFS='|' read -r name args dir; do
        # shellcheck disable=SC2086
        if bash "$WORK/build.sh" --board=$board $args > "$WORK/$name$sfx.log" 2>&1 &&
           [ -f "$WORK/build/$dir$sfx/chgame_boot.bin" ]; then
            cp "$WORK/build/$dir$sfx/chgame_boot.bin" "$dst/$name.bin"
            echo "ok      $rel/$name.bin  $(wc -c < "$dst/$name.bin") bytes"
        else
            tail -20 "$WORK/$name$sfx.log"
            echo "FAILED  $rel/$name.bin"
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
done
exit $failed
