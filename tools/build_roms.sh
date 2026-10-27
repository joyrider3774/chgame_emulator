#!/usr/bin/env bash
# Builds the test ROMs in roms/ from their sources in c:/github with the
# arduino-cli that ships with Arduino IDE 2, using the IDE's own settings
# (~/.arduinoIDE/arduino-cli.yaml): the board package and the sketchbook's
# libraries are the IDE's.
#
# The board package is bateske/CHGame's released one (0.3.0 on, board rev0,
# https://github.com/bateske/CHGame/releases/latest/download/package_chgame_index.json),
# which carries the libraries CHGfx (with its examples), CHSd and CHGame,
# whose examples are the twenty casino games (examples/Games/<Name>) and the
# apps CHSDtoUSB and CHStlView (examples/Apps). Everything is built against
# those, and the casino games and apps are the release's copies: what players
# have, not the newest commit of github.com/bateske/CHGame.
#
#   roms/bateske/        Kevin Bates' CHGame games and tools
#   roms/chgfx/          the CHGfx library's examples
#   roms/poevoid/        poevoid's CHGame-Ponglike
#   roms/filmote/        filmote's CHSpriteView
#   roms/joyrider3774/   the *_embedded games, every CHGame target their
#                        tools/build_releases.py lists, with its defines
#   roms/sdcard/         the card files those games read (<NAME>.DAT, for
#                        the root of the microSD card)
#
# It builds only the board package's examples (the casino games, the apps and
# the CHGfx examples) and the games of the web emulator (web/games.json).
# Other games are built once by hand into roms/<author>/ and stay there: this
# script never deletes ROMs outside joyrider3774/ and chgfx/, and make_chg.py
# and the games site take everything in roms/.
#
# Each project is built with the board settings its README asks for.
# Usage: tools/build_roms.sh [jobs]      (default 4 builds at once)

HERE=$(cd "$(dirname "$0")/.." && pwd)
# Git Bash on Windows, or Linux. WSL counts as Linux: a Linux arduino-cli and
# the package's Linux toolchain, not the Windows ones through interop (a
# Windows arduino-cli cannot read the Linux paths it is given)
if [ "$(uname -s)" = Linux ]; then
    GITHUB=${GITHUB:-$HOME/github}
    CLI=${ARDUINO_CLI:-$(command -v arduino-cli)}
    ARDUINO_DATA="$HOME/.arduino15"
    CLIDIR="$HERE/build/arduino-cli-linux"
    CLIURL=https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Linux_64bit.tar.gz
else
    GITHUB=${GITHUB:-/c/github}
    CLI=${ARDUINO_CLI:-/c/arduino2/resources/app/lib/backend/resources/arduino-cli.exe}
    ARDUINO_DATA="$(cygpath -u "$LOCALAPPDATA")/Arduino15"
    CLIDIR="$HERE/build/arduino-cli"
    CLIURL=https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip
fi
CONFIG=${ARDUINO_CONFIG:-$HOME/.arduinoIDE/arduino-cli.yaml}
# python on Windows (python3 there can be the Microsoft Store's stub), often
# only python3 on Linux
PYTHON=$(command -v python || command -v python3)
OUT="$HERE/roms"
WORK="$HERE/build/roms"
JOBS=${1:-4}
BOARD=CHGame:ch32v:rev0

winpath() { cygpath -w "$1" 2>/dev/null || echo "$1"; }

mkdir -p "$OUT" "$WORK"

# a machine without Arduino IDE 2: arduino-cli is downloaded into
# build/arduino-cli (build/arduino-cli-linux on Linux: the same checkout can
# be built from Windows and WSL), and without the IDE's settings it gets a
# settings file of its own there: the usual data folder (board packages are
# shared with a later IDE install) and a sketchbook of its own, so no
# sketchbook library can take the place of the board package's
if [ ! -x "$CLI" ]; then
    CLI="$CLIDIR/arduino-cli"
    [ "$(uname -s)" = Linux ] || CLI="$CLI.exe"
    if [ ! -x "$CLI" ]; then
        echo "arduino-cli not found: downloading it into $CLIDIR"
        mkdir -p "$CLIDIR"
        case $CLIURL in
            *.zip) curl -fsSL -o "$CLIDIR/cli.zip" "$CLIURL" &&
                       unzip -oq "$CLIDIR/cli.zip" -d "$CLIDIR" && rm -f "$CLIDIR/cli.zip" ;;
            *)     curl -fsSL "$CLIURL" | tar -xz -C "$CLIDIR" ;;
        esac || { echo "arduino-cli download failed"; exit 1; }
    fi
fi
if [ ! -f "$CONFIG" ]; then
    CONFIG="$CLIDIR/arduino-cli.yaml"
    mkdir -p "$CLIDIR/sketchbook"
    [ -f "$CONFIG" ] || cat > "$CONFIG" <<EOF
board_manager:
    additional_urls:
        - https://github.com/bateske/CHGame/releases/latest/download/package_chgame_index.json
directories:
    data: $(winpath "$ARDUINO_DATA")
    user: $(winpath "$CLIDIR/sketchbook")
EOF
fi

# the installed board package: its newest version's libraries, which every
# build finds by itself (platform libraries), and the casino games and apps
# among their examples. Installed (the latest release) when it is missing.
DATA=$("$CLI" --config-file "$(winpath "$CONFIG")" config get directories.data 2>/dev/null | tr -d '
')
[ -n "$DATA" ] || DATA="$ARDUINO_DATA"
PKG=$(ls -d "$(cygpath -u "$DATA" 2>/dev/null || echo "$DATA")"/packages/CHGame/hardware/ch32v/*/ 2>/dev/null | sort -V | tail -1)
if [ -z "$PKG" ]; then
    echo "CHGame board package not installed: installing it"
    "$CLI" --config-file "$(winpath "$CONFIG")" core update-index &&
        "$CLI" --config-file "$(winpath "$CONFIG")" core install CHGame:ch32v ||
        { echo "CHGame board package install failed"; exit 1; }
    PKG=$(ls -d "$(cygpath -u "$DATA" 2>/dev/null || echo "$DATA")"/packages/CHGame/hardware/ch32v/*/ 2>/dev/null | sort -V | tail -1)
fi
# dot_a_linkage=true, as the games' workflows add it after installing the
# package (bateske/CHGame PR #20): without it CHGfx's strong
# DMA1_Channel3_IRQHandler keeps its 8 KB framebuffer in every sketch that
# includes CHGame.h
for props in "${PKG%/}/libraries/CHGfx/library.properties" "${PKG%/}/libraries/CHGame/library.properties" \
             "${PKG%/}/libraries/CHSd/library.properties"; do
    if [ -f "$props" ] && ! grep -q "^dot_a_linkage=true" "$props"; then
        printf '\ndot_a_linkage=true\n' >> "$props" && echo "dot_a_linkage=true added to $props"
    fi
done
CHGLIBS="${PKG%/}/libraries"
CHGFX="$CHGLIBS/CHGfx"
CHGAMELIB="$CHGLIBS/CHGame"
CHSDLIB="$CHGLIBS/CHSd"
CHGAMES="$CHGAMELIB/examples/Games"
CHAPPS="$CHGAMELIB/examples/Apps"
echo "board package: $(basename "${PKG%/}")"
# the c:/github/CHGame clone only for its tools: make_chg.py packs with its
# chgpack.py, which also puts the visual menu's picture in each package
CHGAME="$GITHUB/CHGame"
[ -d "$CHGAME" ] || git clone -q https://github.com/bateske/CHGame "$CHGAME"
git -C "$CHGAME" pull -q --ff-only 2>/dev/null || echo "not updated: $CHGAME"
# every repository a ROM is built from, so a build is always of what is on
# GitHub: cloned when it is missing, pulled when it is there. Fast-forward
# only: a clone with local commits or edits in the way is left as it is, with
# a note. "GitHub owner/repository:folder in c:/github" (filmote's
# CHSpriteView in filmote/: an Arduino sketch folder must keep the .ino's name,
# and bateske's modified copy has it too)
for r in bateske/NewBlocksColor:NewBlocksColor bateske/CHMultiSprite:CHMultiSprite \
         filmote/CHSpriteView:filmote/CHSpriteView bateske/CH32Doom:CH32Doom \
         bateske/FileBrowser:FileBrowser poevoid/CHGame-Ponglike:CHGame-Ponglike \
         joyrider3774/bunnymark_ports:bunnymark_ports \
         joyrider3774/blips_embedded:blips_embedded joyrider3774/blockdude_embedded:blockdude_embedded \
         joyrider3774/formula1_embedded:formula1_embedded joyrider3774/mazethingie_embedded:mazethingie_embedded \
         joyrider3774/puzzleland_embedded:puzzleland_embedded joyrider3774/rubido_embedded:rubido_embedded \
         joyrider3774/sokoban_embedded:sokoban_embedded joyrider3774/waternet_embedded:waternet_embedded \
         joyrider3774/znax_embedded:znax_embedded; do
    d="$GITHUB/${r#*:}"
    if [ ! -d "$d/.git" ]; then
        mkdir -p "$(dirname "$d")"
        git clone -q "https://github.com/${r%%:*}" "$d" && echo "cloned $d" || echo "not cloned: ${r%%:*}"
        continue
    fi
    git -C "$d" pull -q --ff-only 2>/dev/null || echo "not updated: $d (local changes or commits in the way)"
done
# a folder that moved must stop the build: without it the sketches would
# quietly build against whatever copy of CHGfx the sketchbook has
for d in "$CHGFX" "$CHGAMELIB" "$CHSDLIB" "$CHGAMES" "$CHAPPS"; do
    [ -d "$d" ] || { echo "not found: $d (CHGame board package 0.3.0 or later not installed, or reorganised?)"; exit 1; }
done

# FileBrowser uses Arduino's SD library, which stops at "#error Architecture
# or board not supported" on the CH32. A copy in the build folder gets the
# CH32 pin map (the fix CHSDtoUSB and CHStlView carry); the installed
# library is left alone (installed when it is missing).
sdlib() {
    "$CLI" --config-file "$(winpath "$CONFIG")" lib list SD --format json 2>/dev/null |
        "$PYTHON" -c "import json,sys; d=json.load(sys.stdin); l=d.get('installed_libraries',d) if isinstance(d,dict) else d; print(l[0]['library']['install_dir'] if l else '')" 2>/dev/null
}
SDLIB=$(sdlib)
if [ -z "$SDLIB" ]; then
    echo "Arduino SD library not installed: installing it"
    "$CLI" --config-file "$(winpath "$CONFIG")" lib update-index > /dev/null &&
        "$CLI" --config-file "$(winpath "$CONFIG")" lib install SD > /dev/null && SDLIB=$(sdlib)
fi
if [ -n "$SDLIB" ] && [ -d "$SDLIB" ]; then
    rm -rf "$WORK/libs/SD" && mkdir -p "$WORK/libs" && cp -r "$SDLIB" "$WORK/libs/SD"
    "$PYTHON" - "$WORK/libs/SD/src/utility/Sd2PinMap.h" <<'PY'
import sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
if "ARDUINO_ARCH_CH32" not in t:
    fix = ("#if defined(ARDUINO_ARCH_CH32) || defined(CH32X035)\n"
           "#ifndef Sd2PinMap_h\n#define Sd2PinMap_h\n#include <Arduino.h>\n"
           "uint8_t const SS_PIN = PIN_SPI_SS;\nuint8_t const MOSI_PIN = PIN_SPI_MOSI;\n"
           "uint8_t const MISO_PIN = PIN_SPI_MISO;\nuint8_t const SCK_PIN = PIN_SPI_SCK;\n"
           "#endif\n#elif defined(__arm__)")
    t = t.replace("#if defined(__arm__)", fix, 1)
    open(p, "w", encoding="utf-8").write(t)
PY
    export SDLIB_OPT="$WORK/libs/SD"
else
    echo "Arduino SD library not installed: FileBrowser will not build (arduino-cli lib install SD)"
fi
# CHGfx's AdafruitGFXCompat example draws through Adafruit's GFX library
# (on the owner's machine it is in the IDE's sketchbook): installed when it
# is missing, with Adafruit BusIO, which it depends on
if ! "$CLI" --config-file "$(winpath "$CONFIG")" lib list "Adafruit GFX Library" 2>/dev/null | grep -q "^Adafruit GFX Library"; then
    echo "Adafruit GFX library not installed: installing it"
    { "$CLI" --config-file "$(winpath "$CONFIG")" lib update-index > /dev/null &&
        "$CLI" --config-file "$(winpath "$CONFIG")" lib install "Adafruit GFX Library" > /dev/null; } ||
        echo "Adafruit GFX library not installed: AdafruitGFXCompat will not build"
fi

# The *_embedded games read their art (and the level packs) off the microSD
# card: one binary each, and a card file <NAME>.DAT that goes in the card's
# root. Their tools/mkcard.py writes it into the game's releases/ and the
# cardindex.h its build checks the card against (the same as committed when
# the art is), so it runs before the build. The card files are collected in
# roms/sdcard/, the root of a card for all of them (the web build's and the
# games site's sample card take them from there)
rm -rf "$OUT/sdcard"
mkdir -p "$OUT/sdcard"
for g in "$GITHUB"/*_embedded; do
    [ -f "$g/tools/mkcard.py" ] || continue
    n=$(basename "$g")
    if "$PYTHON" "$g/tools/mkcard.py" > "$WORK/$n-mkcard.log" 2>&1; then
        dat="$g/releases/$(sed -n 's/^#define CARD_FILE_NAME "\(.*\)"/\1/p' "$g/source/$n/cardindex.h")"
        cp "$dat" "$OUT/sdcard/" && echo "ok      sdcard/$(basename "$dat")  ($(wc -c < "$dat") bytes)"
        rm -f "$WORK/$n-mkcard.log"
    else
        echo "FAILED  $n card file - $(tail -1 "$WORK/$n-mkcard.log")"
    fi
done

# group|name|sketch folder|board options|defines
{
    # bateske/CHGame's games and apps (CHSDtoUSB, CHSDtoSerial, CHStlView),
    # with the release options its tools/device.py builds them with: LTO and
    # the C library's nano variant to fit, no USB serial; unless the sketch's
    # own tools/game.py names a board (FQBN, CHSDtoUSB and CHSDtoSerial need USB
    # serial) or defines (DEFINES, CHSDtoSerial's GFX_CHUNK_ROWS=1)
    for g in "$CHGAMES"/*/ "$CHAPPS"/*/; do
        "$PYTHON" - "$g" <<'PY'
import ast, os, re, sys
g, group = sys.argv[1], "bateske"
opts, defines = "opt=oslto,rtlib=nano,periph=game,usb=uploadonly", ""
cfg = os.path.join(g, "tools", "game.py")
if os.path.exists(cfg):
    text = open(cfg, encoding="utf-8").read()
    m = re.search(r'^FQBN\s*=\s*"CHGame:ch32v:rev0:([^"]*)"', text, re.M)
    if m:
        opts = m.group(1)
    m = re.search(r'^DEFINES\s*=\s*(\[.*?\])', text, re.M | re.S)
    if m:
        defines = " ".join("-D" + d for d in ast.literal_eval(m.group(1)))
print("%s|%s|%s|%s|%s" % (group, os.path.basename(os.path.normpath(g)), g, opts, defines))
PY
    done
    # not in CHGame: their own repositories
    echo "bateske|NewBlocksColor|$GITHUB/NewBlocksColor|opt=o2std|"
    echo "bateske|CHMultiSprite|$GITHUB/CHMultiSprite|opt=o2std|"
    # CHSpriteView is filmote's: bateske/CHSpriteView is a modified copy of it
    # ("CHSpriteViewMOD"). Cloned into filmote/: the folder must be named like the .ino
    echo "filmote|CHSpriteView|$GITHUB/filmote/CHSpriteView|opt=o2std|"
    echo "bateske|CH32Doom|$GITHUB/CH32Doom|opt=osstd|"
    echo "bateske|FileBrowser|$GITHUB/FileBrowser|opt=osstd|"
    echo "poevoid|CHGame-Ponglike|$GITHUB/CHGame-Ponglike|opt=osstd|"
    # the BunnyMark port (c:/github/bunnymark_ports, the AKA version is beside it)
    if [ -d "$GITHUB/bunnymark_ports/chgame/BunnyMark" ]; then
        echo "joyrider3774|BunnyMark|$GITHUB/bunnymark_ports/chgame/BunnyMark|opt=o2std,periph=game,usb=uploadonly|"
    fi
    # this repository's display check (16 steps on A), to compare the
    # emulator with a device
    echo "joyrider3774|chg_lcdtest|$HERE/tests/sketches/chg_lcdtest|opt=o2std,usb=uploadonly|"
    for ex in "$CHGFX"/examples/*/; do
        echo "chgfx|$(basename "$ex")|$ex|opt=o2std|"
    done
    # the CHGame targets of each game's own release script, with their defines
    for g in "$GITHUB"/*_embedded; do
        "$PYTHON" - "$g" <<'PY'
import os, re, sys
g = sys.argv[1]
name = os.path.basename(g)[:-len("_embedded")]
script = os.path.join(g, "tools", "build_releases.py")
targets = []
# the board options the release script builds the CHGame target with
opts = "periph=game"
if os.path.exists(script):
    for line in open(script, encoding="utf-8"):
        m = re.match(r'\s*\("CHGame",\s*"([^"]*)",\s*(\{.*?\})\s*\)', line)
        if m:
            targets.append((m.group(1), eval(m.group(2), {"__builtins__": {}})))
        m = re.search(r'"fqbn":\s*"CHGame:ch32v:(?:CHGame|rev0):([^"]*)"', line)
        if m:
            opts = m.group(1)
if not targets:
    targets = [("", {})]
for suffix, defines in targets:
    flags = " ".join("-D%s=%s" % (k, v) for k, v in defines.items())
    print("joyrider3774|%s%s|%s|%s|%s" % (name, suffix, os.path.join(g, "source", os.path.basename(g)), opts, flags))
PY
    done
} > "$WORK/list.txt"

build_one() {
    IFS='|' read -r group name sketch opts defines <<EOF
$1
EOF
    bp="$WORK/$group/$name"
    log="$bp.log"
    mkdir -p "$bp" "$OUT/$group"
    set -- --config-file "$(winpath "$CONFIG")" compile --fqbn "$BOARD:$opts" --build-path "$(winpath "$bp")"
    if [ -n "$SDLIB_OPT" ]; then
        set -- "$@" --library "$(winpath "$SDLIB_OPT")"
    fi
    # CHGfx, CHGame and CHSd as the board package has them, named outright: a
    # copy in the sketchbook (an old CHGfx is there) would otherwise be taken
    # ahead of the platform's own, and clash with the CHGame library
    for lib in "$CHGFX" "$CHGAMELIB" "$CHSDLIB"; do
        set -- "$@" --library "$(winpath "$lib")"
    done
    if [ -n "$defines" ]; then
        set -- "$@" --build-property "compiler.c.extra_flags=$defines" --build-property "compiler.cpp.extra_flags=$defines"
    fi
    # parallel builds reading the same library folder now and then fail
    # without an error message, so a failed build gets one more go. Without
    # the libraries' archives: a build that stopped halfway can leave one
    # with only some of the objects (CHCrossword's CHGame.a had 3 of 10 in
    # WSL), and arduino-cli takes it as up to date ever after, so every
    # build of that sketch fails to link
    if "$CLI" "$@" "$sketch" > "$log" 2>&1 ||
        { rm -f "$bp"/libraries/*/*.a; "$CLI" "$@" "$sketch" > "$log" 2>&1; }; then
        bin=$(ls "$bp"/*.ino.bin 2>/dev/null | head -1)
        cp "$bin" "$OUT/$group/$name.bin"
        size=$(grep -o "Sketch uses [0-9]* bytes" "$log" | grep -o "[0-9]*")
        echo "ok      $group/$name.bin  ($size bytes)"
        # a log is kept only for a build that failed
        rm -f "$log"
    else
        echo "FAILED  $group/$name  - $(grep -iE "error|overflow" "$log" | head -1 | cut -c1-150)"
    fi
}
export CLI CONFIG GITHUB WORK OUT BOARD CHGFX CHGAMELIB CHSDLIB

# arduino-cli does not like several instances filling its caches at once on
# a first run, so one build goes first and the rest follow in parallel
first=$(head -1 "$WORK/list.txt")
build_one "$first"
tail -n +2 "$WORK/list.txt" | tr '\n' '\0' |
    xargs -0 -P "$JOBS" -I{} bash -c "$(declare -f winpath build_one); build_one \"\$1\"" _ {}

# ROMs of targets that are gone (the *_embedded games had a binary per level
# pack before their levels moved to the card): only what the list builds stays
# in the groups it builds, and their build folders and logs in build/roms go
# with them
for gdir in "$OUT"/joyrider3774 "$OUT"/chgfx; do
    group=$(basename "$gdir")
    for f in "$gdir"/*.bin; do
        [ -f "$f" ] || continue
        grep -q "^$group|$(basename "$f" .bin)|" "$WORK/list.txt" ||
            { rm -f "$f"; echo "removed $group/$(basename "$f")"; }
    done
    for d in "$WORK/$group"/*/; do
        [ -d "$d" ] || continue
        name=$(basename "$d")
        grep -q "^$group|$name|" "$WORK/list.txt" || rm -rf "$d" "$WORK/$group/$name.log"
    done
done

# and every ROM as a CHG package for the SD menu bootloader, in chg/
GITHUB_DIR="$(winpath "$GITHUB")" "$PYTHON" "$HERE/tools/make_chg.py"
