"""Packs every built ROM in roms/ as a CHG game package into chg/.

    python tools/make_chg.py

A CHG package (CHGame's docs/chg-format.md) is what CHGame's SD menu
bootloader installs from a microSD card: a 512-byte header, then the program
image padded with 0xFF to whole words. Copy chg/*.CHG into a card's GAMES
folder and run the emulator with --bootloader chgame_sdboot.bin; or open a
.CHG in the emulator directly, like a .bin.

The bootloader reads 8.3 names, so the files get those: the ROM's name in
capitals, cut to eight characters, keeping a level pack's number
(sokoban_3 -> SOKOBA_3.CHG). chg/INDEX.TXT says which is which. The title
the menu shows (19 characters, capitals) and the author come from the ROM's
name and its folder in roms/.

Since board package 0.3.0 a package can carry a picture after the program,
which the visual SD menu (Tools > Bootloader > SD Graphic Menu) shows while
the game is selected (CHGame's spec/chg.md and spec/card.md). Each package
gets one: for bateske's games and apps their own cart picture (the
cartImage of the chgame.json beside the sketch in the board package, or for
a game taken from a release cart its box art, roms/<group>/<name>.png), for
the rest the games site's screenshot (c:/github/chgames/screenshots), cut to
the 11 colours the picture rule allows. The picture is encoded by CHGame's
own tools/chgpack.py (the c:/github/CHGame clone); without that clone the
packages are made here, with no picture.

Beside the packages go the menu's own files, CHGame's defaults made by its
card tool (tools/chcart): MENU.BG (the text menu's CHGAME logo and button
bar), COVER.PIC and SYSTEM.PIC (the graphic menu's cover and screens). So
copying everything in chg/ into a card's GAMES folder gives the menus as a
card prepared by CHGame's tools shows them.
"""
import glob
import json
import os
import re
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROMS = os.path.join(HERE, "roms")
OUT = os.path.join(HERE, "chg")
GITHUB = os.environ.get("GITHUB_DIR", "c:/github")
SHOTS = os.path.join(GITHUB, "chgames", "screenshots")

# CHGame's packer, for the picture: c:/github/CHGame/tools/chgpack.py
sys.path.insert(0, os.path.join(GITHUB, "CHGame", "tools"))
try:
    import chgpack
except ImportError:
    chgpack = None
# and its card tool, for the menu's own files
try:
    from chcart import runtime as chcart_runtime
except ImportError:
    chcart_runtime = None


def menu_files():
    """The files a card's GAMES folder holds besides the games, as CHGame's
    card tool makes them for a card that names none of its own (spec/card.md):
    MENU.BG, the text menu's background (the CHGAME logo, the A PLAY / B BACK
    bar; without it the list is on plain black), COVER.PIC, the graphic
    menu's cover, and SYSTEM.PIC, its about page and other screens. {} without
    the clone"""
    if chcart_runtime is None:
        return {}
    r = chcart_runtime
    return {
        "MENU.BG": r.menu_background(r.DEFAULT_BACKGROUND.read_bytes(), r.model.UI_COLORS),
        "COVER.PIC": r.menu_picture(r.DEFAULT_COVER.read_bytes()),
        "SYSTEM.PIC": r.system_pic(),
    }

MAGIC = 0x31474843          # "CHG1"
FORMAT_VERSION = 1
HEADER_BYTES = 512
TARGET_ID = 0x35335843      # "CX35": the CHGame, CH32X035G8U6
LAYOUT_ID = 0x003000F7      # program at 0x3000, metadata page at 0xF700
APP_MAX = 0xF700 - 0x3000   # 50,944 bytes
BOOT_SIG = 0x4C424843       # "CHBL" at offset 8 marks a bootloader image


def text(s, n):
    """printable ASCII, NUL padded to n bytes (at most n - 1 characters)"""
    b = "".join(c if 32 <= ord(c) <= 126 else "?" for c in s)[:n - 1].encode("ascii")
    return b.ljust(n, b"\0")


def pack(image, title, author):
    payload = image + b"\xff" * (-len(image) % 4)
    if not payload or len(payload) > APP_MAX:
        raise ValueError("%d bytes; a program is at most %d" % (len(payload), APP_MAX))
    if len(payload) >= 12 and struct.unpack_from("<I", payload, 8)[0] == BOOT_SIG:
        raise ValueError("a bootloader image, not a program")
    h = bytearray(HEADER_BYTES)
    struct.pack_into("<IHHIIIIII", h, 0, MAGIC, FORMAT_VERSION, HEADER_BYTES, TARGET_ID, LAYOUT_ID,
                     len(payload), zlib.crc32(payload) & 0xFFFFFFFF, 0, 0)
    h[0x20:0x40] = text(title, 32)
    h[0x40:0x50] = text(author, 16)
    struct.pack_into("<I", h, 0x1FC, zlib.crc32(bytes(h[:0x1FC])) & 0xFFFFFFFF)
    return bytes(h) + payload


def short_name(name, taken):
    """an 8.3 base name, unique among those already taken"""
    m = re.match(r"(.*?)[_-](\d+)$", name)      # a level pack: blips_1 (not formula1)
    stem, num = (m.group(1), m.group(2)) if m and m.group(1) else (name, "")
    stem = re.sub(r"[^A-Z0-9]", "", stem.upper()) or "GAME"
    tail = "_" + num if num else ""
    base = stem[:8 - len(tail)] + tail
    n = 1
    while base in taken:
        tail2 = "~%d" % n
        base = stem[:8 - len(tail2)] + tail2
        n += 1
    taken.add(base)
    return base


def taken_files(index):
    """the package file names an index lists"""
    return {line.split()[0] for line in index}


def title_of(name):
    """the menu's title: words in capitals (sokoban_3 -> SOKOBAN 3)"""
    return re.sub(r"[_-]+", " ", name).upper()[:19]


def cart_images():
    """sketch name -> its cart picture, for the games and apps in the
    installed CHGame board package (the newest version)"""
    root = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Arduino15", "packages", "CHGame", "hardware", "ch32v")
    versions = sorted(glob.glob(os.path.join(root, "*")),
                      key=lambda p: [int(x) if x.isdigit() else 0 for x in re.split(r"[.]", os.path.basename(p))])
    found = {}
    if versions:
        for cfg in glob.glob(os.path.join(versions[-1], "libraries", "CHGame", "examples", "*", "*", "chgame.json")):
            sketch = os.path.dirname(cfg)
            try:
                img = json.load(open(cfg, encoding="utf-8")).get("cartImage")
            except (OSError, ValueError):
                continue
            if img and os.path.isfile(os.path.join(sketch, img)):
                found[os.path.basename(sketch)] = os.path.join(sketch, img)
    return found


def site_shot(group, name, tmp):
    """the games site's screenshot of a ROM, in the colours the picture rule
    allows (at most 11), as a PNG in tmp; None when the site has none"""
    slug = re.sub(r"[^a-z0-9]+", "-", ("%s-%s" % (group, name)).lower()).strip("-")
    shot = os.path.join(SHOTS, slug + ".png")
    if not os.path.isfile(shot):
        return None
    from PIL import Image
    im = Image.open(shot).convert("RGB")
    if im.size != (128, 128):
        return None
    if len(im.getcolors(1 << 24) or []) > 11 or im.getcolors(12) is None:
        im = im.quantize(colors=11, dither=Image.Dither.NONE).convert("RGB")
    out = os.path.join(tmp, slug + ".png")
    im.save(out)
    return out


def package(image, title, author, picture_png):
    """the package, with CHGame's packer and the picture when it can be had"""
    if chgpack is None:
        return pack(image, title, author)
    picture = None
    if picture_png:
        try:
            picture = chgpack.picture_file(picture_png)
        except Exception as e:      # a PNG breaking the picture rule: no picture, as chcart does
            print("no picture for %s: %s" % (title, e))
    return chgpack.pack(chgpack.pad_image(image), title, author[:15], picture=picture)


def main():
    os.makedirs(OUT, exist_ok=True)
    taken, index, failed, written = set(), [], 0, []
    carts = cart_images()
    tmp = tempfile.mkdtemp(prefix="make_chg_")
    for group in sorted(d for d in os.listdir(ROMS) if os.path.isdir(os.path.join(ROMS, d))):
        for f in sorted(os.listdir(os.path.join(ROMS, group))):
            if not f.lower().endswith(".bin"):
                continue
            name = f[:-4]
            picture = carts.get(name) if group == "bateske" else None
            # a release cart's box art, which build_roms.sh puts beside its ROM
            beside = os.path.join(ROMS, group, name + ".png")
            picture = picture or (beside if os.path.isfile(beside) else None)
            picture = picture or site_shot(group, name, tmp)
            try:
                data = package(open(os.path.join(ROMS, group, f), "rb").read(), title_of(name), group, picture)
            except ValueError as e:
                print("skipped %s/%s: %s" % (group, f, e))
                failed += 1
                continue
            short = short_name(name, taken) + ".CHG"
            path = os.path.join(OUT, short)
            # written only when it changes, and dated like its ROM, so the
            # folder shows which games were rebuilt and when
            if not os.path.exists(path) or open(path, "rb").read() != data:
                with open(path, "wb") as out:
                    out.write(data)
                written.append(short)
            rom_time = os.path.getmtime(os.path.join(ROMS, group, f))
            os.utime(path, (rom_time, rom_time))
            index.append("%-12s %-20s roms/%s/%s" % (short, title_of(name), group, f))
    # the menu's own files, beside the packages
    for fname, data in menu_files().items():
        path = os.path.join(OUT, fname)
        if not os.path.exists(path) or open(path, "rb").read() != data:
            with open(path, "wb") as out:
                out.write(data)
            written.append(fname)
    # packages of ROMs that are gone
    for f in os.listdir(OUT):
        if f.upper().endswith(".CHG") and f not in taken_files(index):
            os.remove(os.path.join(OUT, f))
            print("removed %s" % f)
    text_index = ("CHG packages made by tools/make_chg.py from roms/: file, menu title, ROM\n\n"
                  + "\n".join(index) + "\n")
    index_path = os.path.join(OUT, "INDEX.TXT")
    if not os.path.exists(index_path) or open(index_path, encoding="utf-8").read() != text_index:
        with open(index_path, "w", newline="\n") as out:
            out.write(text_index)
    print("%d packages in %s, %d new or changed%s%s" % (
        len(index), OUT, len(written), ": " + " ".join(written) if written else "",
        ", %d skipped" % failed if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
