#!/bin/sh
# Builds the test ROMs in roms/ from their sources in c:/github with the
# arduino-cli that ships with Arduino IDE 2, using the IDE's own settings
# (~/.arduinoIDE/arduino-cli.yaml): the board package (0.2.4 or later) and
# the sketchbook's libraries are the IDE's, except CHGfx, which is always the
# one in bateske/CHGame (platform/libraries/CHGfx).
#
# bateske/CHGame (the c:/github/CHGame clone, pulled first) is the source of
# truth for CHGame development: its board package's libraries/ hold CHGfx
# (with its examples), CHSd and the CHGame library, whose examples are the
# twenty casino games (examples/games/<Name>) and the apps CHSDtoUSB and
# CHStlView (examples/apps).
# The separate repositories those came from are frozen. The board package
# installed is still the released 0.2.4 (CHGame has made no release yet).
#
#   roms/bateske/        Kevin Bates' CHGame games and tools
#   roms/chgfx/          the CHGfx library's examples
#   roms/poevoid/        poevoid's CHGame-Ponglike
#   roms/filmote/        filmote's CHSpriteView
#   roms/joyrider3774/   the *_embedded games, every CHGame target their
#                        tools/build_releases.py lists, with its defines
#
# Each project is built with the board settings its README asks for.
# Usage: tools/build_roms.sh [jobs]      (default 4 builds at once)

GITHUB=${GITHUB:-/c/github}
CLI=${ARDUINO_CLI:-/c/arduino2/resources/app/lib/backend/resources/arduino-cli.exe}
CONFIG=${ARDUINO_CONFIG:-$HOME/.arduinoIDE/arduino-cli.yaml}
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/roms"
WORK="$HERE/build/roms"
JOBS=${1:-4}
BOARD=CHGame:ch32v:CHGame

winpath() { cygpath -w "$1" 2>/dev/null || echo "$1"; }

mkdir -p "$OUT" "$WORK"

CHGAME="$GITHUB/CHGame"
# since 2026-10-03 the libraries, and the games and apps as the CHGame
# library's examples, live inside the board package in CHGame
CHGLIBS="$CHGAME/platform/board/arduino/CHGame/libraries"
CHGFX="$CHGLIBS/CHGfx"
CHGAMELIB="$CHGLIBS/CHGame"
CHSDLIB="$CHGLIBS/CHSd"
CHGAMES="$CHGAMELIB/examples/Games"
CHAPPS="$CHGAMELIB/examples/Apps"
[ -d "$CHGAME" ] || git clone -q https://github.com/bateske/CHGame "$CHGAME"
git -C "$CHGAME" pull -q 2>/dev/null
# a folder that moved must stop the build: without it the sketches would
# quietly build against whatever copy of CHGfx the sketchbook has
for d in "$CHGFX" "$CHGAMELIB" "$CHSDLIB" "$CHGAMES" "$CHAPPS"; do
    [ -d "$d" ] || { echo "not found: $d (has bateske/CHGame been reorganised?)"; exit 1; }
done

# FileBrowser uses Arduino's SD library, which stops at "#error Architecture
# or board not supported" on the CH32. A copy in the build folder gets the
# CH32 pin map (the fix CHSDtoUSB and CHStlView carry); the installed
# library is left alone. Install it once with: arduino-cli lib install SD
SDLIB=$("$CLI" --config-file "$(winpath "$CONFIG")" lib list SD --format json 2>/dev/null |
        python -c "import json,sys; d=json.load(sys.stdin); l=d.get('installed_libraries',d) if isinstance(d,dict) else d; print(l[0]['library']['install_dir'] if l else '')" 2>/dev/null)
if [ -n "$SDLIB" ] && [ -d "$SDLIB" ]; then
    rm -rf "$WORK/libs/SD" && mkdir -p "$WORK/libs" && cp -r "$SDLIB" "$WORK/libs/SD"
    python - "$WORK/libs/SD/src/utility/Sd2PinMap.h" <<'PY'
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

# group|name|sketch folder|board options|defines
{
    # bateske/CHGame's games and apps (CHSDtoUSB, CHStlView), with the release
    # options its tools/device.py builds all of them with: LTO and the C
    # library's nano variant to fit, no USB serial. Its FQBN names the board
    # rev0, which is the 0.3.0 board package's name for it; the installed
    # 0.2.4 still calls it CHGame (BOARD above)
    for g in "$CHGAMES"/*/ "$CHAPPS"/*/; do
        echo "bateske|$(basename "$g")|$g|opt=oslto,rtlib=nano,periph=game,usb=uploadonly|"
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
    for ex in "$CHGFX"/examples/*/; do
        echo "chgfx|$(basename "$ex")|$ex|opt=o2std|"
    done
    # the CHGame targets of each game's own release script, with their defines
    for g in "$GITHUB"/*_embedded; do
        python - "$g" <<'PY'
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
        m = re.search(r'"fqbn":\s*"CHGame:ch32v:CHGame:([^"]*)"', line)
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
    # everything builds against CHGame's CHGfx (pulled above), not whichever
    # copy the sketchbook has, and its CHGame library (CHGame.h: buttons,
    # frame pacing, START held 3 s back to the SD game menu), which its games
    # include since 2026-10-02 (its tools/device.py passes both)
    if [ -d "$CHGFX" ]; then
        set -- "$@" --library "$(winpath "$CHGFX")"
    fi
    if [ -d "$CHGAMELIB" ]; then
        set -- "$@" --library "$(winpath "$CHGAMELIB")"
    fi
    # and CHSd, the third library its tools/device.py passes
    if [ -d "$CHSDLIB" ]; then
        set -- "$@" --library "$(winpath "$CHSDLIB")"
    fi
    if [ -n "$defines" ]; then
        set -- "$@" --build-property "compiler.c.extra_flags=$defines" --build-property "compiler.cpp.extra_flags=$defines"
    fi
    # parallel builds reading the same library folder now and then fail
    # without an error message, so a failed build gets one more go
    if "$CLI" "$@" "$sketch" > "$log" 2>&1 || "$CLI" "$@" "$sketch" > "$log" 2>&1; then
        bin=$(ls "$bp"/*.ino.bin 2>/dev/null | head -1)
        cp "$bin" "$OUT/$group/$name.bin"
        size=$(grep -o "Sketch uses [0-9]* bytes" "$log" | grep -o "[0-9]*")
        echo "ok      $group/$name.bin  ($size bytes)"
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
    xargs -0 -P "$JOBS" -I{} sh -c "$(declare -f winpath build_one); build_one \"\$1\"" _ {}


# and every ROM as a CHG package for the SD menu bootloader, in chg/
python "$HERE/tools/make_chg.py"
