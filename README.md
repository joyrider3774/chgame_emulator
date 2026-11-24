# CHGame Emulator

An emulator for the [CHGame](https://github.com/bateske/CH32SerialBoot) handheld —
a WCH CH32X035G8U6 (QingKe V4C RISC-V, 48 MHz) driving a 128x128 ST7735S — written
in C with SDL3. It runs natively and in the browser (Emscripten).

It is an emulator, not a simulator: it executes the real RISC-V machine code of a
compiled game (`.bin`, `.hex` or `.elf`, as the Arduino IDE builds them for the
CHGame board) and models the chip's peripherals and the hardware wired to them.
Nothing in it knows anything about any particular game.

Made with the help of Claude (Anthropic).

## Not an official CHGame project

This is an independent, unofficial piece of software, not affiliated with or
endorsed by the makers of the CHGame. Where the emulator and the hardware
disagree, the hardware is right and the emulator has a bug.

## Boards

The CHGame has had more than one board (hardware revision), with the same
chip and memory map but the screen, card, buttons, buzzer and LED on other
pins. A program built for one board does not run on another: on the wrong
board it shows nothing, as on the real thing. The emulator does them all and
emulates **Rev1** (also Rev 1.5, the same board for a program) unless told
otherwise:

| Board | `--board` | Target id | Display D/C | Card CS | Buttons U/D/L/R, A/B/SEL/START | Buzzer | LED | Backlight |
|---|---|---|---|---|---|---|---|---|
| CHGame Rev1, Rev 1.5 (default) | `rev1` | `CGR1` | PB4 | PB3 | PB0/PB1/PC3/PC0, PB11/PB6/PB7/PB8 | PA1, TIM2 CH2 | PB10 | switched, PB9 |
| CHGame Rev0 | `rev0` | `CX35` | PB0 | PB11 | PB4/PC14/PB3/PC15, PB1/PB6/PB7/PB8 | PB10, TIM1 CH2 | PB9 | always on |

Which board runs a program:

1. `--board` when given. A `.chg` for another board is then refused, as the
   bootloader refuses it (the SD menu's ERROR 5).
2. Else the board a `.chg` package is for (its target id), the newest board a
   `.chgame` cart has a build for, or the board a `--bootloader` image is for
   (its board word at offset 0x14; 0 is Rev0).
3. Else Rev1. A `.bin`, `.hex` or `.elf` does not say which board it was built
   for: a Rev0 build needs `--board rev0`.

Each board boots with its own bootloader, the board package's "USB Only"
one (0.4.0), and the CHGame SD menu bootloaders are built per board too
(`--bootloader` takes either board's). The web page has a Board menu and
`?board=rev0`. A board is one entry in `src/board.c` (its pins, timer,
target id and bootloader), so a later revision is added there, with its
bootloader in `tools/make_bootloader_image.py`.

## What is emulated

* **CPU** — RV32IMAC plus WCH's **XW** extension (the extra 16-bit `c.lbu`,
  `c.lhu`, `c.sb`, `c.sh` and their sp-relative forms that `-march=rv32imacxw`
  emits). The decoding is checked against WCH's own assembler, see
  `tests/xw_golden.txt`. Also the QingKe specifics the board package relies on:
  vectored absolute-address `mtvec`, the **PFIC** interrupt controller with
  priorities, nesting and `NVIC_SystemReset`, the **HPE** hardware prologue/
  epilogue that `WCH-Interrupt-fast` handlers need, and CSRs `0x800`/`0x804`.
* **Timing** — a pipeline model measured on a real CHGame: a prefetcher that
  reads flash a 32-bit word every 4 cycles (so flash code is fetch bound, as on
  the chip) and SRAM a word a cycle, with data accesses taking the single SRAM
  port from it; 2-cycle loads and stores, paired stores, a background divider,
  and branch refetch. It matches the hardware on 51 test loops to within 0.5%
  (see *Calibration* below and the top of `src/rv32.c`).
* **SysTick**, **GPIO A/B/C**, **AFIO**, **RCC**, **SPI1** with its real clock
  (a byte takes as long as it takes on the wire, TXE/BSY/RXNE behave), **DMA1**
  (SPI transmit/receive and memory-to-memory), **TIM1/TIM2/TIM3** (counting,
  update interrupts, PWM), the **flash controller** (fast page erase/program,
  half-word programming) and the **ADC** (an unconnected pin reads noise, which
  is what the games seed their random numbers from).
* **ST7735S** — 132x162 frame memory, CASET/RASET/RAMWR, MADCTL (MX/MY/MV/BGR),
  COLMOD 12/16/18 bit, inversion, sleep, display on/off, hardware reset on PB12.
  The glass shows the 128x128 block the board's own library addresses; a program
  with the wrong offsets or rotation looks wrong here the way it would on the panel.
  On Rev1 the glass stays dark until the program lights the backlight (PB9).
* **Buzzer** — driven by GPIO or by its timer's PWM: TIM1 CH2 on Rev0's PB10
  (partial remap), TIM2 CH2 on Rev1's PA1. The pin is integrated exactly over
  every audio sample, so tones come out clean at any frequency.
* **LED** (PB9 on Rev0, PB10 on Rev1), shown in the status bar.
* **Rev1's board parts** — the 4.7k pull-ups on CN2's I2C lines, and VBUS sense
  on PA0 reading 0 V (no USB cable: nothing enumerates here).
* **Saves** — flash pages a game writes are kept in `<game>.sav` beside it.

* **The bootloader** — programs start the way a CHGame starts them: the real
  CHGame bootloader (MIT, from the board package, embedded in
  `src/bootloader_image.c`) sits in the first 12 KB, the loader writes the
  metadata page at 0xF700 the way the uploader does, and the bootloader checks
  the image's CRC and jumps to it. `--no-bootloader` starts at 0x3000 instead.

* **The microSD card** — an SDHC card in SPI mode on SPI1 with its chip select
  on PB11 (Rev0) or PB3 (Rev1), sharing the bus with the display as on the
  board (`src/sdspi.c`).
  Reads, writes, multi-block streaming over DMA. CHStlView, CHSpriteView and
  FileBrowser run with it.
  
USB is present as registers only (the core's CDC code runs, no host ever
enumerates it, so `Serial` output is dropped exactly as on a board with no PC
attached).

## Building

Needs CMake and SDL3 (on Windows, MSYS2's `mingw-w64-x86_64-SDL3` works; the exe
is then linked statically).

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Web

With the Emscripten SDK:

```sh
emcmake cmake -S . -B build_web -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_web
```

Serve `build_web/` over http (browsers do not allow `fetch` from `file://`).
`tools/make_web.sh serve` does all of it: builds, then serves the repository
on http://127.0.0.1:8000/build_web/CHGame_Emulator.html with Python (so
`?rom=../roms/bateske/CHChess.bin` works). The web build carries the games
listed in `web/games.json` (its Games menu); other games are
published on the games site only, which has its own copy
of the emulator.
Programs load from:

* **Open file…** — a file on this computer
* the **URL box**, or `CHGame_Emulator.html?rom=https://…/game.bin`
* `CHGame_Emulator.html?rom=game.bin` — a file in the same folder as the page
* a `games.json` beside the page (`["blips.bin", {"file": "znax.bin", "name": "Znax", "board": "rev0"}]`)
  fills a Games menu; an entry's `board` picks the board it runs on

The Board menu (or `?board=rev0`) sets the board for a `.bin`, `.hex` or
`.elf`; a `.chg` brings its own.

Saves go to the browser's IndexedDB.

## Running

```sh
./build/CHGame_Emulator path/to/game.bin
```

or drop a file on the window, or press F3.

### Command line

```
CHGame_Emulator [program] [options]
```

The program is a `.bin`, `.hex`, `.elf`, `.chg` (a CHG package for the SD menu)
or `.chgame` (a cart, CHGame's sharing format: a ZIP holding a game's build
for each board, its card files and pictures). A cart's game runs with its
build for the board, and its card files are copied onto the card folder
(not onto a card image), replacing older copies, as a card prepared from the
cart would have them. `--game id` picks one game of a cart with several.

| Option | What it does |
|---|---|
| `--board rev1\|rev0` | the board to emulate (see *Boards*): `rev1` (also `rev1.5`) or `rev0`. Default: the board a `.chg`, a `.chgame` or a `--bootloader` image is for, else `rev1` |
| `--game id` | which game of a `.chgame` cart to run (default: its first) |
| `--bootloader file.bin` | another bootloader instead of the built-in one, e.g. one of CHGame's SD menus (`tools/build_bootloaders.sh` builds them into `bootloaders/`); without a program the flash starts erased |
| `--no-bootloader` | start the program at 0x3000, no bootloader |
| `--sd folder\|card.img` | the microSD card: a folder (made into a FAT32 card) or a card image (default: the `sdcard` folder next to the emulator) |
| `--sd-size MB` | the size of a card made from a folder (default: room for its files) |
| `--no-sd` | an empty card slot |
| `--piezo` | the piezo's sound (F7 switches) |
| `--no-piezo` | the bare pin signal (the default) |
| `--integer-scale` | whole multiples of the screen only, for this run |
| `--no-integer-scale` | fill the window, for this run (F8 switches between the two and remembers the choice) |
| `--scale N` | the window's first size, N times the 128x128 screen (1-10, default 4) |
| `--help`, `-h` | list the options and exit (in a message box on Windows) |

| | |
|---|---|
| D-pad | Arrow keys or WASD, gamepad d-pad or left stick |
| A / B | X or Space / Z — gamepad east / south button |
| START / SELECT | Enter / Right Shift or Backspace — gamepad Start / Back |
| Help | F1 |
| Reset | F2 |
| Open | F3 |
| Back to the bootloader (game menu) | F4 |
| Turn the screen 90 degrees clockwise; the d-pad turns with it (kept per game) | R — gamepad north button |
| Pause / fast forward | P / hold Tab |
| Stats overlay (game fps, speed, MIPS, host load) | F9 |
| Scaling: fill the window / whole multiples only; remembered (`--integer-scale`, `--no-integer-scale` for one run) | F8 |
| Screenshot | F10 |
| Record a GIF; press again to stop and choose where to save it | F6 |
| Fullscreen | F11 or Alt+Enter |
| Volume | + / - |

The game fps in the overlay is counted at the display: a new frame is a write
window that starts higher up the screen than the one before it.

### Another bootloader: the SD game menu

```sh
./build/CHGame_Emulator --bootloader bootloaders/chgame_sdboot.bin            # the bootloader alone
./build/CHGame_Emulator --bootloader bootloaders/chgame_sdboot.bin game.bin   # with a program installed
./build/CHGame_Emulator chg/CHCHESS.CHG                                       # a package, directly
```

`tools/build_bootloaders.sh` builds that bootloader from CHGame's source
(the `c:/github/CHGame` clone, with the board package's toolchain) and
puts the binary in `bootloaders/chgame_sdboot.bin`. `tools/make_chg.py`
packs every ROM in `roms/` as a `.CHG` package into `chg/` (8.3 names,
`chg/INDEX.TXT` lists them; `build_roms.sh` runs it at the end): copy them
into a card's `GAMES` folder. The emulator also opens a `.CHG` directly,
like a `.bin`, after the same checks the bootloader makes.

`--bootloader file.bin` puts another bootloader (at most 12 KB) at 0x0000
instead of the built-in one, which stays the default. It was made for
[CHGame's SD menu bootloader](https://github.com/bateske/CHGame/tree/main/platform/bootloader)
(`release/chgame_sdboot.bin`): at power-on it lists the `/GAMES/*.CHG`
packages on the microSD card (`tools/chgpack.py` in CHGame makes them from
a game's `.bin`), and A installs the chosen one into the program flash, the
same way a USB upload writes it, then starts it.

Without a program the emulator starts like a board with only that bootloader
on it: the program flash is erased. Everything the bootloader flashes, and
whatever the games save afterwards, is kept in `<bootloader>.sav` beside the
bootloader file, so the installed game is still there next time.

**F4** goes back to the bootloader the way a game returns to the menu (a
casino game in CHGame when START is held for 3 s): a software reset with no boot
request pending. With the built-in bootloader it simply restarts the program.
`chg_headless` takes the same option, `-` for no program, and
`--back sec` for F4.

### The microSD card

```sh
./build/CHGame_Emulator game.bin --sd folder|card.img [--sd-size MB]   # --no-sd: an empty slot
```

By default the card is the folder `sdcard` next to the emulator (on macOS
next to the `.app` bundle, not inside it). A folder's
files go on a FAT32 card built in memory; what the program writes, creates or
deletes goes back into the folder every few seconds and when the emulator
closes. An image file is a whole card (as a card reader dumps it), read and
written in place, and is created empty (256 MB, or `--sd-size`) when it does
not exist. `chg_headless --sd-make card.img [folder] [MB]` writes a card image,
empty or holding a folder's files. In the browser the card is kept in the
browser, with buttons to add files or a folder, empty it, and download it as
an image.

**Manage…** opens the card manager (`sdcard.html?card=chgame`, also usable on
its own). The card comes out of the emulator, and the game pauses, while it is
open. It lists the card's folders and files. Files download one at a time, a
folder or the whole card as a `.zip`. Files, folders, a `.zip` or a FAT card
image can be uploaded (or dropped on the list) into the folder shown. Folders
can be made, and files and folders deleted. **Done** puts the card back in.

`?sd=card.zip` or `?sd=card.img` (a file beside the page or a URL) fills the
card from a zip file or a FAT12/16/32 card image before the first program
starts. It does so once: as long as that file stays the same, later visits
keep what the programs wrote to the card. Emptying the card makes the next
visit fill it again. Files put on a card that was never filled (with the
card manager on its own, before any program ran) stay: the image's files are
added beside them. Without `?sd=`, a `sdcard.zip` (or else a `sdcard.img`)
beside the page is used the same way. The page only asks the server for the
file's size and date on each visit, and downloads it only when they changed.

FileBrowser needs Arduino's `SD` library (`arduino-cli lib install SD`);
`tools/build_roms.sh` builds it against a copy with the CH32 pin-map fix.

## Tools

* `chg_headless game.bin [seconds] [out.ppm] [--press btn@sec[:dur]]... [--save] [--sd folder|card.img]`
  runs without a window and reports how fast it ran, for testing and profiling.
* `xw_test` checks the XW decoder (`ctest` runs it).

## Calibration

`tests/sketches/chg_cal` is the one the cycle model is built from: 51 assembly
loops (`bench.S`), each once in flash and once in RAM where it matters, whose
layout is fixed to the byte — sequential 32- and 16-bit code of two lengths,
misaligned branch targets, loads, stores and load-use against RAM, flash and
peripherals, store and load pairs, divides, multiplies, branches, and
`writePixels`' byte swap as gcc builds it. It prints cycles per iteration (x100)
over USB serial and keeps them in `cal_results[]`, which
`chg_headless chg_cal.bin 3 NUL --mem <address of cal_results> 51` dumps from the
emulator, so the two can be compared line by line.

`tests/sketches/chg_bench` is the quick check: nine loops (SPI by hand from
flash and from RAM, SPI by DMA, ALU loops in flash and in RAM, divides, flash
table loads, the pixel byte swap, `delay(100)`) timed in microseconds and shown
on the screen. `chg_bench.bin` is prebuilt. On a real CHGame and in the
emulator they agree to within 1%.

`tests/sketches/chg_flashtest` exercises the flash page write the games use for
saves: green is a pass, and a white bar is added per run.

## Layout

| | |
|---|---|
| `src/rv32.c` | the CPU: decoder, execution, traps, interrupts, HPE |
| `src/bus.c` | memory map and peripherals |
| `src/st7735.c` | the display controller and panel |
| `src/audio.c` | the buzzer, turned into samples |
| `src/machine.c` | reset and the run loop |
| `src/loader.c` | `.bin`/`.hex`/`.elf` loading, save files |
| `src/main.c` | SDL3 front end |
| `web/shell.html` | the page around the web build |

## Licence

MIT, see `LICENSE`. The embedded CHGame bootloader (`src/bootloader_image.c`)
is Kevin Bates' CH32SerialBoot, also MIT, with its notice in that file. SDL3
is Zlib licensed.
