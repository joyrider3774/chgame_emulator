#!/bin/sh
# Builds the web version into build_web/ with Emscripten, puts roms/ beside it
# with a games.json listing them for the page's Games menu, and optionally
# serves it:
#
#   tools/make_web.sh            build only
#   tools/make_web.sh serve      build, then serve on http://127.0.0.1:8000
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
"$PY" "$EM/emcmake.py" cmake -S . -B build_web -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null || exit 1
# the shell page is a link option, which cmake does not track: relink when it changes
[ web/shell.html -nt build_web/CHGame_Emulator.html ] && rm -f build_web/CHGame_Emulator.html
cmake --build build_web || exit 1

# every build gets its own .js/.wasm URLs (?v= the .wasm's hash), so neither a
# browser nor a CDN can pair an old .js with a new .wasm
# The card manager page and the card helpers it shares with the emulator page
# (web/sdcard.html, web/sdtools.js: the same files as in aka_emulator) go
# beside it, stamped the same way.
cp web/sdcard.html web/sdtools.js build_web/
python - "build_web/CHGame_Emulator.html" "build_web/CHGame_Emulator.wasm" "build_web/sdtools.js" <<'PY'
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
    open(page, "w", encoding="utf-8").write(t)
print("build", build)
PY

rm -rf build_web/roms
cp -r roms build_web/roms
python - <<'PY'
import json, os
os.chdir("build_web")
games = []
for group in sorted(g for g in os.listdir("roms") if os.path.isdir(os.path.join("roms", g))):
    for f in sorted(os.listdir(os.path.join("roms", group))):
        if f.endswith(".bin"):
            games.append({"file": "roms/%s/%s" % (group, f), "name": "%s / %s" % (group, f[:-4])})
json.dump(games, open("games.json", "w"), indent=1)
print("%d games in build_web/games.json" % len(games))
PY

if [ "$1" = "serve" ]; then
    echo "http://127.0.0.1:8000/CHGame_Emulator.html"
    cd build_web && python -m http.server 8000 --bind 127.0.0.1
fi
