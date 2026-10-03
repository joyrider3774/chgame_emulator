#!/bin/sh
# Builds the web version into build_web/ with Emscripten, with the games in
# web/games.json beside it (newer ones are on the games site only), and
# optionally serves it:
#
#   tools/make_web.sh            build only
#   tools/make_web.sh serve      build, then serve the repository on http://127.0.0.1:8000
#
# EMSDK defaults to c:/github/emsdk.

EMSDK=${EMSDK:-/c/github/emsdk}
HERE=$(cd "$(dirname "$0")/.." && pwd)
EM="$EMSDK/upstream/emscripten"
PY=$(ls "$EMSDK"/python/*/python.exe 2>/dev/null | head -1)
[ -n "$PY" ] || PY=python3
NODE_BIN=$(dirname "$(ls "$EMSDK"/node/*/bin/node* 2>/dev/null | head -1)")
export EMSDK PATH="$EM:$NODE_BIN:$PATH"

cd "$HERE" || exit 1
# emcmake from the emsdk folder, or the one on the PATH (the GitHub Action's
# setup-emsdk puts it there)
if [ -f "$EM/emcmake.py" ]; then
    EMCMAKE="$PY $EM/emcmake.py"
else
    EMCMAKE=emcmake
fi
GEN=
command -v ninja > /dev/null && GEN="-G Ninja"
$EMCMAKE cmake -S . -B build_web $GEN -DCMAKE_BUILD_TYPE=Release > /dev/null || exit 1
# the shell page is a link option, which cmake does not track: relink when it changes
[ web/shell.html -nt build_web/CHGame_Emulator.html ] && rm -f build_web/CHGame_Emulator.html
cmake --build build_web || exit 1

# every build gets its own .js/.wasm URLs (?v= the .wasm's hash), so neither a
# browser nor a CDN can pair an old .js with a new .wasm
# The card manager page and the card helpers it shares with the emulator page
# (web/sdcard.html, web/sdtools.js: the same files as in aka_emulator) go
# beside it, stamped the same way.
cp web/sdcard.html web/sdtools.js build_web/
"$PY" - "build_web/CHGame_Emulator.html" "build_web/CHGame_Emulator.wasm" "build_web/sdtools.js" <<'PY'
import hashlib, re, sys
html, wasm, tools = sys.argv[1], sys.argv[2], sys.argv[3]
build = hashlib.sha1(open(wasm, "rb").read() + open(tools, "rb").read() +
                     open("web/sdcard.html", "rb").read()).hexdigest()[:12]
for page in (html, "build_web/sdcard.html"):
    t = open(page, encoding="utf-8").read()
    # the page's script comes out minified: BUILD_ID="dev"
    t = re.sub(r"""BUILD_ID\s*=\s*["'][^"']*["']""", 'BUILD_ID="%s"' % build, t)
    t = re.sub(r'src="?CHGame_Emulator\.js(\?v=\w+)?"?>', 'src="CHGame_Emulator.js?v=%s">' % build, t)
    t = re.sub(r'src="?sdtools\.js(\?v=\w+)?"?>', 'src="sdtools.js?v=%s">' % build, t)
    # newline="\n": Python on Windows would otherwise write CRLF line ends
    open(page, "w", encoding="utf-8", newline="\n").write(t)
print("build", build)
PY

# The emulator carries only the games listed in web/games.json (its Games
# menu): new games in roms/ are published on the games site (c:/github/chgames,
# tools/build_site.py) and come here only when added to that list by hand (the
# twelve CHGame casino games not yet in it were added on 2026-10-03).
# roms/ is not in git, so a build from a fresh checkout (the GitHub Action)
# has none of them: the menu then lists only the games that are there.
rm -rf build_web/roms build_web/games.json
"$PY" - <<'PY'
import json, os, shutil
games = json.load(open("web/games.json"))
missing = []
for g in games:
    src = g["file"]
    if not os.path.isfile(src):
        missing.append(src)
        continue
    dst = os.path.join("build_web", src)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
if not missing:
    shutil.copy2("web/games.json", "build_web/games.json")
elif len(missing) < len(games):
    with open("build_web/games.json", "w", newline="\n") as f:
        json.dump([g for g in games if g["file"] not in missing], f, indent=1)
print("%d games in build_web/games.json%s" % (len(games) - len(missing),
      " (%d not in roms/: run tools/build_roms.sh)" % len(missing) if missing else ""))
PY

if [ "$1" = "serve" ]; then
    # serving the repository root makes roms/ reachable as ../roms/... for testing:
    # http://127.0.0.1:8000/build_web/CHGame_Emulator.html?rom=../roms/bateske/CHChess.bin
    echo "http://127.0.0.1:8000/build_web/CHGame_Emulator.html"
    python -m http.server 8000 --bind 127.0.0.1
fi
