#!/bin/sh
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
# Each project is built with the board settings its README asks for.
# Usage: tools/build_roms.sh [jobs]      (default 4 builds at once)

GITHUB=${GITHUB:-/c/github}
CLI=${ARDUINO_CLI:-/c/arduino2/resources/app/lib/backend/resources/arduino-cli.exe}
CONFIG=${ARDUINO_CONFIG:-$HOME/.arduinoIDE/arduino-cli.yaml}
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT="$HERE/roms"
WORK="$HERE/build/roms"
JOBS=${1:-4}
BOARD=CHGame:ch32v:rev0

winpath() { cygpath -w "$1" 2>/dev/null || echo "$1"; }

mkdir -p "$OUT" "$WORK"

# the installed board package: its newest version's libraries, which every
# build finds by itself (platform libraries), and the casino games and apps
# among their examples
DATA=$("$CLI" --config-file "$(winpath "$CONFIG")" config get directories.data 2>/dev/null | tr -d '')
[ -n "$DATA" ] || DATA="$LOCALAPPDATA/Arduino15"
PKG=$(ls -d "$(cygpath -u "$DATA" 2>/dev/null || echo "$DATA")"/packages/CHGame/hardware/ch32v/*/ 2>/dev/null | sort -V | tail -1)
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
# GitHub. Fast-forward only: a clone with local commits or edits in the way
# is left as it is, with a note
for r in NewBlocksColor CHMultiSprite filmote/CHSpriteView CH32Doom FileBrowser CHGame-Ponglike \
         bunnymark_ports "$GITHUB"/*_embedded; do
    case "$r" in /*|?:*) d="$r" ;; *) d="$GITHUB/$r" ;; esac
    [ -d "$d/.git" ] || continue
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
    if python "$g/tools/mkcard.py" > "$WORK/$n-mkcard.log" 2>&1; then
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
        python - "$g" <<'PY'
import ast, os, re, sys
g = sys.argv[1]
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
print("bateske|%s|%s|%s|%s" % (os.path.basename(os.path.normpath(g)), g, opts, defines))
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
    # without an error message, so a failed build gets one more go
    if "$CLI" "$@" "$sketch" > "$log" 2>&1 || "$CLI" "$@" "$sketch" > "$log" 2>&1; then
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
    xargs -0 -P "$JOBS" -I{} sh -c "$(declare -f winpath build_one); build_one \"\$1\"" _ {}

# ROMs of targets that are gone (the *_embedded games had a binary per level
# pack before their levels moved to the card): only what the list builds stays
# in the groups it builds, and their build folders and logs in build/roms go
# with them; the carts below add theirs afterwards
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

# Games published as CHGame carts (.chgame, spec/chgame.md in CHGame: a ZIP
# with the release image rev0.bin, the box art and the game's SD files),
# taken from their newest GitHub release rather than built: Ethan's Critters
# compiles the hash of its 44 MB card file in, and that file is only
# published with the release (its art is sold, not in the repository), so a
# build of main says WRONG CARD DATA with it. Each gives
# roms/<group>/<name>.bin, its box art roms/<group>/<name>.png (make_chg.py's
# picture) and its SD files in build/carts/<name>/sdcard/ (the newest
# release, prereleases included; downloaded again only when it changes)
CARTS="bateske|EthansCritters
bateske|OtherRealm"
echo "$CARTS" | while IFS='|' read -r group repo; do
    python - "$group" "$repo" "$OUT" "$HERE/build/carts" <<'PY'
import io, json, os, shutil, sys, urllib.request, zipfile
group, repo, out, cache = sys.argv[1:5]
api = "https://api.github.com/repos/%s/%s/releases" % (group, repo)
try:
    releases = json.load(urllib.request.urlopen(api, timeout=60))
    rel = next(r for r in releases if any(a["name"].endswith(".chgame") for a in r["assets"]))
    asset = next(a for a in rel["assets"] if a["name"].endswith(".chgame"))
except Exception as e:
    print("FAILED  %s/%s  - no release cart (%s)" % (group, repo, e))
    sys.exit(0)
d = os.path.join(cache, repo)
os.makedirs(d, exist_ok=True)
cart = os.path.join(d, asset["name"])
if not os.path.exists(cart) or os.path.getsize(cart) != asset["size"]:
    for f in os.listdir(d):
        if f.endswith(".chgame"):
            os.remove(os.path.join(d, f))
    with urllib.request.urlopen(asset["browser_download_url"], timeout=600) as r, open(cart, "wb") as f:
        shutil.copyfileobj(r, f)
z = zipfile.ZipFile(cart)
names = z.namelist()
binary = next(n for n in names if n.endswith("/rev0.bin"))
game = binary.rsplit("/", 1)[0]
os.makedirs(os.path.join(out, group), exist_ok=True)
with open(os.path.join(out, group, repo + ".bin"), "wb") as f:
    f.write(z.read(binary))
if game + "/cart.png" in names:
    with open(os.path.join(out, group, repo + ".png"), "wb") as f:
        f.write(z.read(game + "/cart.png"))
sd = os.path.join(d, "sdcard")
shutil.rmtree(sd, ignore_errors=True)
for n in names:
    if n.startswith(game + "/sdcard/") and not n.endswith("/"):
        p = os.path.join(sd, n[len(game) + len("/sdcard/"):])
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(z.read(n))
with open(os.path.join(d, "release.txt"), "w") as f:
    f.write(rel["tag_name"])
print("ok      %s/%s.bin  (%d bytes, release %s)" % (group, repo, z.getinfo(binary).file_size, rel["tag_name"]))
PY
done

# Ethan's Critters carries its own copy of CHSd, which sends each card
# command straight after the previous response, without the 8 idle clocks
# (N_RC) the SD spec asks for; a strict card (the owner's SanDisk 32 GB) then
# never initialises, and the game says INSERT CARD. So its release tag is
# built here from the c:/github clone with that one line added to its CHSd
# (the fix in the owner's CHGame branch chsd-idle-clocks-before-command), as
# its own tools build it, and replaces the release image. The rest of the
# source is the release's, so the card hash compiled in still matches the
# release's CRITTERS.DAT. If that build fails, the release image stays
SDFIX="bateske|EthansCritters|EthansCritters"
echo "$SDFIX" | while IFS='|' read -r group repo sketch; do
    tag=$(cat "$HERE/build/carts/$repo/release.txt" 2>/dev/null)
    src="$GITHUB/$repo"
    [ -n "$tag" ] && [ -d "$src/.git" ] || { echo "FAILED  $group/$repo  - no release tag or no clone for the CHSd fix"; continue; }
    w="$WORK/sdfix/$repo"
    rm -rf "$w"
    mkdir -p "$w"
    git -C "$src" fetch -q --tags 2>/dev/null
    git -C "$src" archive "$tag" | tar -x -C "$w" || { echo "FAILED  $group/$repo  - no tag $tag in $src"; continue; }
    spi=$(find "$w" -path "*libraries/CHSd/src/SdSpi.cpp" | head -1)
    libs=${spi%/CHSd/src/SdSpi.cpp}
    python - "$spi" <<'PY' || { echo "FAILED  $group/$repo  - its CHSd changed, the fix does not apply"; continue; }
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8", newline="").read()
old = "static uint8_t cmd(uint8_t c, uint32_t arg) {\n    xfer((uint8_t)(0x40 | c));\n"
if old not in s:
    sys.exit(1)
s = s.replace(old, "static uint8_t cmd(uint8_t c, uint32_t arg) {\n    xfer(0xFF);\n    xfer((uint8_t)(0x40 | c));\n")
open(p, "w", encoding="utf-8", newline="").write(s)
PY
    bp="$WORK/sdfix/$repo-build"
    if "$CLI" --config-file "$(winpath "$CONFIG")" compile --fqbn "$BOARD:opt=oslto,rtlib=nano,periph=game,usb=uploadonly" \
            --build-path "$(winpath "$bp")" --library "$(winpath "$libs/CHGfx")" --library "$(winpath "$libs/CHGame")" \
            --library "$(winpath "$libs/CHSd")" "$(winpath "$w/$sketch")" > "$bp.log" 2>&1; then
        cp "$bp/$sketch.ino.bin" "$OUT/$group/$repo.bin"
        echo "ok      $group/$repo.bin  ($(wc -c < "$OUT/$group/$repo.bin") bytes, release $tag with the CHSd fix)"
        rm -f "$bp.log"
    else
        echo "FAILED  $group/$repo  (CHSd fix build; the release image stays) - $(grep -iE "error" "$bp.log" | head -1 | cut -c1-150)"
    fi
done

# and every ROM as a CHG package for the SD menu bootloader, in chg/
python "$HERE/tools/make_chg.py"
