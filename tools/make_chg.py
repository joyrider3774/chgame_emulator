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
"""
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROMS = os.path.join(HERE, "roms")
OUT = os.path.join(HERE, "chg")

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


def main():
    os.makedirs(OUT, exist_ok=True)
    taken, index, failed, written = set(), [], 0, []
    for group in sorted(d for d in os.listdir(ROMS) if os.path.isdir(os.path.join(ROMS, d))):
        for f in sorted(os.listdir(os.path.join(ROMS, group))):
            if not f.lower().endswith(".bin"):
                continue
            name = f[:-4]
            try:
                data = pack(open(os.path.join(ROMS, group, f), "rb").read(), title_of(name), group)
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
