# CLAUDE.md

Working notes for continuing this repository with Claude (or by hand). The
README describes the emulator for users; this file covers how to work on it:
setup, verification, the hardware calibration loop, design decisions and the
traps already fallen into.

## What this is

A real emulator (not a simulator) for Kevin Bates' **CHGame** handheld:
WCH CH32X035G8U6 (QingKe V4C RISC-V, RV32IMAC + WCH "XW" extension, 48 MHz,
62 KB flash, 20 KB SRAM), ST7735S 128x128 on SPI1, 8 buttons, piezo on PB10,
LED on PB9, microSD (SPI mode, PB11 CS). It executes the compiled Arduino `.bin`
exactly as the device does, booting through the real bootloader. C11 + SDL3,
native and Emscripten from the same source.

**The owner's priorities, in order:** emulator speed, then screen, CPU timing
and buzzer accuracy. High-level shortcuts that simulate an API instead of the
hardware are not wanted: model the hardware.

Modelled on the owner's earlier TinyJoypad emulator
(`c:/github/Tinyjoypad_Emulator`, simavr + SDL3), but the CPU here is our own.

## Layout

| Path | What |
|---|---|
| `src/machine.h` | the whole machine as one struct (`ChgMachine`): CPU, PFIC, SysTick, GPIO, SPI, DMA, timers, flash controller, buzzer log, LCD. Pin map in the header comment |
| `src/rv32.c` | CPU: decoder (incl. XW), predecoded instruction cache, execution loop, traps/PFIC/HPE, **cycle model** (see top comment) |
| `src/bus.c` | memory map and all peripherals; everything lazily evaluated from cycle timestamps |
| `src/machine.c` | reset and `chg_run()` scheduler loop |
| `src/st7735.c` | display controller + panel mapping |
| `src/audio.c` | buzzer pin integrated exactly per audio sample |
| `src/loader.c` | `.bin`/`.hex`/`.elf`, bootloader install + metadata page, `.sav` files |
| `src/bootloader_image.c` | the real CHGame bootloader (MIT), generated from the board package `.bin` — do not edit. Since 2026-10-09 board package 0.3.0's `chgame_boot_nomenu.bin` ("USB Only": the 0.2.4 boot decision, no SD menu, same metadata page); before that 0.2.2/0.2.4's `chgame_bootloader.bin`. Regenerate it the same way for a new release |
| `src/sdspi.c` | the microSD card in SPI mode: command frames, R1/R3/R7, data tokens, CMD18 streaming, CMD24/25 writes. Selected by PB11 low in `spi_deliver()` (bus.c) |
| `src/sdcard.c` | card storage, image file or folder built into an in-memory FAT32 card and synced back, formatter, `sdcard_make_image` |
| `src/main.c` | SDL3 front end using SDL main callbacks (works under Emscripten unchanged); F6 GIF recording (save dialog natively, showSaveFilePicker or a download in the browser); R or the gamepad's north button turns the screen by quarter turns as in the ESPboy emulator (the frame is turned after st7735_render, so GIFs and screenshots match; the d-pad is remapped before chg_set_buttons; kept per program by file name: rotations.txt in the pref folder, localStorage `chgame_rotation:<name>` on the web, apart from the ESPboy emulator's `espboy_rotation:`) |
| `src/gif.c/.h` | GIF encoder, shared with the ESPboy emulator: per-frame exact palette (3-3-2 when over 256 colours), identical frames merged, emulated-time delays; `chg_headless --gif out.gif` |
| `web/shell.html` | page around the web build: file picker, URL box, `?rom=`, `?sd=`, `games.json` menu, card manager overlay |
| `web/sdcard.html`, `web/sdtools.js` | the card manager page (`?card=chgame`) and the zip / FAT-image reader + zip writer. **Identical copies in aka_emulator/web: change both.** The manager edits the IDBFS database (`/chgame/sdcard`, store `FILE_DATA`, key = full path, `{timestamp, mode, contents}`) directly; the overlay ejects the card (`chg_web_sd_eject`, pauses) and reinserts it after `syncfs(true)` |
| `tools/headless.c` | `chg_headless`: run without a window, report speed, dump screen/RAM/registers |
| `tools/build_roms.sh` | builds every known CHGame program into `roms/` (gitignored), then runs `make_chg.py` |
| `build/carts/` | games taken from their GitHub release cart (`.chgame`) instead of built, by `build_roms.sh` (`CARTS`: bateske's EthansCritters, whose compiled-in card hash only matches the release's 44 MB CRITTERS.DAT, and OtherRealm): `rev0.bin` to `roms/bateske/<name>.bin`, the box art beside it as `.png`, the SD files in `build/carts/<name>/sdcard/` |
| `tools/make_chg.py` | every ROM as a `.CHG` package (CHGame's SD menu format) into `chg/` (gitignored), 8.3 names, `chg/INDEX.TXT` |
| `tools/build_bootloaders.sh` | builds the six CHGame bootloaders (`build.sh` variants as its `tools/dist.sh`: the board package's five, `chgame_sdboot`, `_static`, `chgame_sdvisual`, `_static`, `chgame_boot_nomenu`, plus `chgame_sdboot_locked`) in a temp copy, only the binaries to `bootloaders/` (gitignored); they came out byte-identical to 0.3.0's |
| `tools/make_web.sh` | Emscripten build + the games listed in `web/games.json` (60, frozen: new games go into roms/, chg/ and the games site only, never into the web emulator (owner, 2026-10-09), `c:/github/chgames`, `tools/build_site.py`), `serve` to host with Python |
| `tests/xw_test.c`, `tests/xw_golden.txt` | XW decoder vs WCH's own assembler output |
| `tests/sketches/chg_cal/` | **the calibration sketch** the cycle model is fitted to (51 asm loops) |
| `tests/sketches/chg_bench/` | quick 9-loop timing check shown on the LCD (`chg_bench.bin` prebuilt) |
| `tests/sketches/chg_flashtest/` | flash page write / save persistence check |
| `tests/sketches/chg_lcdread/`, `chg_lcdtest/` | display controller checks: read-back over USB serial, and 16 glass steps on A (see ST7735 below) |

## Environment (the owner's Windows machine)

- Shell: Git Bash (POSIX) and PowerShell are both available. MSYS2 mingw64
  toolchain: gcc 16, cmake 4.4, ninja, **SDL3 installed** (static lib used).
- Emscripten SDK: `c:/github/emsdk` (6.0.4; SDL3 via `-sUSE_SDL=3`). Its Node:
  `c:/github/emsdk/node/22.16.0_64bit/bin/node.exe`, its Python likewise.
- Arduino IDE 2 at `c:/arduino2`; its CLI:
  `c:/arduino2/resources/app/lib/backend/resources/arduino-cli.exe`.
  **Always pass `--config-file ~/.arduinoIDE/arduino-cli.yaml`** — the bare
  CLI does not know the IDE's sketchbook (`OneDrive/Documenten/Arduino`;
  its old CHGfx 1.2.0 copy was removed 2026-10-08: a sketchbook library is
  taken ahead of the board package's own, and that one clashed with 0.3.0's
  CHGame library).
- CHGame board package: `~/AppData/Local/Arduino15/packages/CHGame/` **0.3.0**
  (installed 2026-10-08, from
  `https://github.com/bateske/CHGame/releases/latest/download/package_chgame_index.json`),
  FQBN `CHGame:ch32v:rev0` with menus `opt=oslto(default)|osstd|o2std|...`,
  `rtlib`, `periph=game|full`, `usb=serial|uploadonly`, `boot=sdmenu|sdstatic|sdvisual|sdvisualstatic|nomenu`.
  It carries the libraries CHGfx 1.3.1, CHSd and CHGame (whose examples are
  the casino games and apps) and the five bootloaders in `bootloaders/CHGame/`;
  same memory map as 0.2.4. Every build also writes `<sketch>.ino.chg`.
  Toolchain `riscv-none-embed-gcc 8.2.0` in `tools/` there (objdump needs
  `-M xw` to disassemble XW instructions). The core's `tone()` is still an
  empty function with `periph=game`.
- Uploader: `.../tools/chgame-upload/<version>/chgame-upload.exe -port COM6 flash <bin> -run`.
- **A real CHGame is normally attached on COM6** and the owner has allowed
  flashing it for tests. Leave a game on it or tell the owner what's on it.
- Chrome (not Edge) is installed; Puppeteer (`puppeteer-core`) with emsdk's
  Node drives it for web tests.
- Game sources: only `c:/github/*_embedded` are the owner's CHGame games
  (don't grep all of `c:/github` — hundreds of unrelated repos). Others cloned
  next to this repo: **bateske's `CHGame`**, the source of truth since
  2026-10-02 (board package source, the SD menu bootloader, `tools/chgpack.py`
  which `make_chg.py` uses; their separate game repositories and CHGfx/CHCasino
  are frozen). Since 0.3.0 `build_roms.sh` builds the casino games and apps
  (CHSDtoUSB, CHSDtoSerial, CHStlView) from the **installed release's** copies
  (its CHGame library's `examples/Games` and `examples/Apps`, with each
  sketch's `tools/game.py` FQBN/DEFINES), and passes the release's CHGfx,
  CHGame and CHSd with `--library`, not the clone's newest commit. For what is
  not in it bateske's `NewBlocksColor CH32Doom CHMultiSprite FileBrowser`
  (CHStlView moved into CHGame's Apps), `CHGame-Ponglike` (poevoid) and
  filmote's `CHSpriteView` in `c:/github/filmote/CHSpriteView` (the original;
  bateske's `c:/github/CHSpriteView` is a modified copy, no longer built; the
  sketch folder must keep the .ino's name, hence the subfolder). The owner's
  games (`*_embedded`, and bunnymark_ports) build for 0.3.0 too since
  2026-10-08 (release scripts `CHGame:ch32v:rev0:opt=osstd,periph=game`, the
  workflows' `CORE_CHGAME: 0.3.0` and the new package URL). **No LTO** for
  them: gcc 8.2 miscompiles them with it (blips: measured on hardware
  2026-10-01, and the emulator showed the LTO build resetting into the
  bootloader at the title screen). Since 2026-10-09 their PlatformCHGame.cpp
  (one file, identical in all eight) takes the buttons (`chgame.boot()`,
  `chgame_readButtons()`, `chgame_exitToMenu()`) and the buzzer (`audio::`) from
  the CHGame library: every tone a library effect (pitch in 20 Hz steps, 510 ms
  steps chained; a tone with no length is 4 s, cut off by the next).
  That needs `dot_a_linkage=true` in the libraries (bateske/CHGame PR #20; added
  by hand to the installed 0.3.0 and by the games' workflows after installing
  it): without it CHGfx's strong `DMA1_Channel3_IRQHandler` keeps its 8 KB
  framebuffer in every sketch that includes CHGame.h (+9.5 KB RAM). Puzzleland
  only fits with its clouds in fixed point (no soft-float, -1.9 KB).

## Build and verify

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/xw_test.exe tests/xw_golden.txt            # must say 0 wrong
tools/build_roms.sh 4                              # roms/ (needs the clones above)
tools/make_web.sh serve                            # web build on :8000
```

Headless runner (use it for every change; the GUI can't be seen from a session):

```sh
./build/chg_headless.exe game.bin 12 out.ppm --press a@4 --press start@6:0.2 --shots prefix --save
./build/chg_headless.exe game.bin 3 NUL --mem 200009b8 51      # dump RAM words
CHG_REGS=1 ./build/chg_headless.exe game.bin 4 NUL             # dump registers at the end
./build/chg_headless.exe game.bin 5 NUL --no-bootloader        # start at 0x3000
```

It prints speed (x real time), a game fps estimate, guest MIPS/CPI, the PC and
any CPU fault. Convert `.ppm` with PIL to look at it. Symbol addresses for
`--mem`/PC lookups come from the ELF in `build/roms/<group>/<name>/` via
`riscv-none-embed-nm`/`addr2line`.

**Regression check after any CPU/peripheral change** — all three must hold:
1. `xw_test`: 193 encodings, 0 wrong.
2. Calibration: build `tests/sketches/chg_cal`, run it headless with
   `--no-bootloader --mem <cal_results> 51`, compare with the hardware numbers
   (see below) — mean abs error was 0.03%, every case within 0.5%.
3. Every ROM in `roms/` for ~15 s with some button presses: no `FAULT`.

## The cycle model (don't change constants by guessing)

Fitted to hardware measurements, documented at the top of `src/rv32.c`:
flash code is fetch bound (a prefetcher reads a 32-bit word every 4 cycles,
depth 2); SRAM code fetches a word a cycle but shares the single SRAM port with
data (load 1 port cycle, store 2, back-to-back stores pair 2+1); loads/stores 2
core cycles; mul 1; divide runs in the background for 10 cycles; taken branch
restarts fetch after the in-flight word; a RAM load/store in the two
instructions before a taken branch hides the branch bubble; flash load from RAM
code costs 4 5/8 cycles (eighths are carried in `frac8`). Peripheral accesses
cost the same as RAM.

To re-measure: compile `tests/sketches/chg_cal`, flash it to COM6, open the
port with DTR on and read lines `name value` between `BEGIN`/`END`
(PowerShell `System.IO.Ports.SerialPort`, `DtrEnable=$true`). Values are
cycles per loop iteration x100. `cal_results[]` address from `nm`.
Hardware results of the last run (2026-09-30) — the reference:
`f_add32_8 4409, r_add32_8 1103, f_add32_24 10821, r_add32_24 2706,
f_add16_16 4008, r_add16_16 1905, f_add16_48 10420, r_add16_48 5110,
f_add32_8_mis 4811, r_add32_8_mis 1203, f_lw8 4410, r_lw8 1804, f_lw8_flash 7615,
r_lw8_flash 4008, f_lw8_per 4409, r_lw8_per 1805, f_lhu8 4409, r_lhu8 1805,
r_lhu8_flash 4008, f_sw8 4409, r_sw8 2205, f_sw8_per 4409, r_sw8_per 1905,
f_sb8 4410, r_sb8 2205, f_lwuse8 7615, r_lwuse8 2706, f_lwalu8 7615, r_lwalu8 2706,
f_divbig8 8016, r_divbig8 8015, f_divsmall8 8016, r_divsmall8 8016, f_mul8 4409,
r_mul8 1103, f_btaken8 7615, r_btaken8 1905, f_bnot8 4409, r_bnot8 1104,
f_jal8 7615, r_jal8 1905, r_swalu8 3508, r_swalu8_per 2706, r_sw2alu2 3107,
r_sw1alu7 1304, r_lw2alu2 2706, r_lw4 1003, r_lw1alu7 1203, r_mis16 1203,
r_swap_al 71731, r_swap_mis 77842`.
`chg_bench` on hardware: 16539 11009 11311 50095 16703 5017 5015 4280 99338 (us).

When adding a calibration loop, check the layout in the disassembly: under
`.option norvc` a `.balign 4` that needs a 2-byte pad is silently skipped (use
the `ALIGN4` macro in `bench.S`), and use `.option norelax`.

## Design decisions worth keeping

- **Speed first.** Predecoded `ChgInsn` cache per halfword for flash and RAM;
  RAM stores invalidate overlapping entries (games run hot loops from `.data`);
  flash entries are dropped when the flash controller writes a page.
  Peripherals are never ticked: each keeps a timestamp and computes its state
  when touched; `chg_schedule()` finds the next event (SysTick match, timer
  update, DMA completion with an interrupt). Any register write that could
  change interrupts calls `chg_kick()` to end the CPU slice. ~20-25x real time
  natively, ~8-13x in the browser. Keep the hot loop in `cpu_exec` lean.
- **Audio is the master clock** in the front end: each frame runs exactly the
  cycles the audio queue needs (`src/main.c emulate()`).
- **Boot like the device**: the real bootloader at 0x0000, the metadata page at
  0xF700 (magic "CHGM", version 1, length, CRC-32 ISO-HDLC over the app) written
  by the loader. This mattered: games read/jump into low flash (Ponglike prints
  from a NULL pointer; the bootloader's cleanup is part of the app's start).
- **QingKe specifics that games depend on**: `mtvec` mode 3 (vectored, table of
  absolute addresses); PFIC priorities and nesting (INTSYSCR 0x804 bit 1);
  **HPE** saves x1,x5-7,x10-17,x28-31 in the core on trap entry and mret
  restores them (`WCH-Interrupt-fast` handlers rely on it); **on nested traps
  the core also keeps mepc/mcause/mstatus per level** — without that CHGfx's
  DMA interrupt preempting SysTick corrupted CHBlackjack's stack. CSR 0x800
  aliases mstatus MIE/MPIE. With nesting on, MIE is not cleared on interrupt
  entry (the core's SysTick handler disables it itself).
- **XW instructions** (WCH's compressed byte/halfword forms): c.lbu/c.sb in
  quadrant 0 funct3 001/101, c.lhu/c.sh in quadrant 2 funct3 001/101, sp-relative
  forms in quadrant 0 funct3 100 with the sub-op in bits 6:5. Offsets decoded in
  `decode16()`; golden file generated by assembling every offset.
- **ST7735**: follows the ST7735S datasheet V1.1 (Sitronix, e.g.
  crystalfontz.com/controllers/Sitronix/ST7735S) except where the device
  differs, measured 2026-10-05 with `tests/sketches/chg_lcdread` (bit-bangs
  the bidirectional SDA line: IDs and frame memory read back over USB serial,
  no one needs to look) and `chg_lcdtest` (glass-only behaviour, stepped with
  A, the owner compared it with emulator screenshots: all 16 steps match the
  device exactly). Findings: the
  controller runs the 132x132 configuration (GM=01): MY mirrors over 132 rows,
  only rows 0-131 are shown, partial mode and scrolling work in 132 lines, but
  the memory has 162 rows (132-161 writable without MY, never shown). The
  glass shows the block reached with MADCTL 0xC8 at column 2, row 3 (screen
  (x,y) = GRAM(129-x, 128-y)). RGBSET is ignored: 16-bit R/B stored as v<<1
  (31 -> 63), 12-bit as v<<2|v>>2. RDDID reads 83 76 0F (inverse of the
  datasheet's). Frame memory is 18-bit; the RGB/BGR bit applies at scan time
  (BGR set = correct colours on this BGR panel); SWRESET keeps MADCTL/COLMOD;
  partial mode's non-display area is white. RAMRD in serial mode: 9 dummy
  bits (a byte and a clock) before the 18-bit data.
- **TIM UG only restarts a running counter**: measured on the device
  (2026-10-01): with CEN off, SWEVGR=UG loads ATRLR/CCR but leaves CNT
  (250 stays 250, and counting resumes from there); with CEN on, UG resets
  CNT to 0. CHChess's `tone()` (stop, reload, UG, start every 1 ms of a
  sweep) depends on it: with the reset every millisecond its swoops became a
  1 kHz buzz. CHChess's own `tools/audio/host` model resets regardless, which
  is wrong in the same way.
- **Piezo sound**: `src/piezo_filter.c` (256-tap minimum phase FIR at 48 kHz)
  is fitted by `tools/fit_piezo.py` to a phone recording of the device playing
  CHChess's capture effect (`tests/sketches/chg_sfxtest`, built with CHChess's `src/audio` copied in; `roms/sfxtest_capture.bin`
  is its build). Resonance ~5 kHz, the 500-2000 Hz tones 25-45 dB down. Off by
  default (owner's choice): F7 / `--piezo` (front end and chg_headless) turn it on.
  The device also adds the sweeps' 2nd harmonic (piezo non-linearity), which
  a linear filter cannot.
- **Buzzer**: PB10 as GPIO or TIM1 CH2 PWM (AFIO PCFR1 bits 17:15 = 1). State
  changes are logged with timestamps; `audio.c` integrates in closed form.
- **Saves**: flash pages the program writes go to `<rom>.sav`
  (4-byte address + 256 bytes, repeated); on the web IndexedDB `/chgame/saves`, the card `/chgame/sdcard` (IDBFS names the database after the mount point; the AKA emulator on the same site uses `/aka/...`. Builds before 2026-10-01 used the shared `/sdcard` and `/saves`: the page copies the old saves in once, see `fromShared` in shell.html).
- **Other bootloaders** (`--bootloader file.bin`, `chg_set_bootloader()` in
  loader.c; the built-in one stays the default). Made for CHGame's SD menu
  bootloader (`c:/github/CHGame/platform/bootloader/release/chgame_sdboot.bin`):
  it installs `/GAMES/*.CHG` packages into app flash through the flash
  controller. Without a program the flash starts erased with only the
  bootloader (`chg_load_bootloader_only()`), and its `.sav` is
  `<bootloader>.sav`. It depends on: the reset-cause flags in RCC RSTSCKR
  (`m->reset_flags`, bits 25-31, kept across resets until RMVF; a power-on
  reads PIN+POR+SFT, measured on the board because the factory boot code
  enters user flash with a software reset; a SYSRESET adds SFT), and on
  FLOCK/LOCK reading the lock state rather than what was written (it locks
  fast programming after every page and unlocks before the next; with the
  stored bit, only its first page was ever written). F4 / `chg_headless
  --back T` clear the boot request block (RAM 0x20000000, magic + ~magic) and
  do a software reset, as a CHGame casino game holding START does. Test card:
  pack built ROMs with `python c:/github/CHGame/tools/chgpack.py pack
  X.bin GAMES/X.CHG --title X`, run `chg_headless - 20 NUL --bootloader
  <copy of chgame_sdboot.bin> --sd card --press a@4 --save --shots s`.
- **USB** is a dumb register block: the core's CDC code runs, nothing
  enumerates, `Serial` output is dropped (as on a board with no PC).
- The game-fps number is a heuristic (a RAMWR window starting above the last
  one = new frame). It misreads some games (CH32Doom). Improve, don't trust.

## Board/package facts that matter

- App region 0x3000..0xF6FF (50944 bytes), metadata page 0xF700, 62 KB user
  flash, RAM 0x20000000 (first 16 bytes: boot-request block surviving warm reset).
- Flash alias at 0x08000000 (flash programming writes there). Erased = 0xFF.
- GPIOB CFGHR is write-only on the chip; the core keeps a RAM shadow
  (`CFGHR_tmpB`). Emulated reads return the written value.
- The owner's games' `tools/build_releases.py` list CHGame targets with defines
  (level packs); `build_roms.sh` parses them and passes the defines through
  `--build-property compiler.c(pp).extra_flags=...`. Puzzleland only fits that way.
- FileBrowser needs Arduino's SD library: `build_roms.sh` builds it against a copy
  with the CH32 pin-map fix (`arduino-cli lib install SD` once).
- Everything builds with board package 0.3.0 and its own CHGfx 1.3.1, named
  with `--library` (a sketchbook copy would otherwise win). The casino games
  need LTO (`opt=oslto,rtlib=nano,periph=game,usb=uploadonly`, their release
  options). `build_roms.sh` reuses `build/roms/<group>/<name>`: after a library
  change delete `build/roms`, or stale objects link (12 casino games failed
  with `undefined reference to gfx_begin` that way).
- `.CHG` packages (`make_chg.py`) carry the visual SD menu's picture since
  0.3.0 (CHGame's spec/chg.md, offset 0x060): bateske's sketches their
  `chgame.json` `cartImage`, the rest the games site's screenshot cut to 11
  colours. Beside them `chg/` gets CHGame's default `MENU.BG` (the text
  menu's logo and button bar; without it the list is on plain black),
  `COVER.PIC` and `SYSTEM.PIC`, made with its `tools/chcart`: copy all of
  `chg/` into a card's `GAMES/`. Test: a card folder with `GAMES/*.CHG`, `chg_headless - 13 NUL
  --bootloader <copy of the package's chgame_sdvisual.bin> --sd card --press
  down@4:0.15 ...` (UP/DOWN pages through the pictures).
- Parallel arduino-cli builds occasionally fail silently (shared library
  folder on OneDrive); `build_roms.sh` retries once.

## microSD

Test programs: CHStlView (lists `.STL` with triangle counts, renders them),
CHSpriteView (streams `/FIRE/*.BIN` at ~280 fps through DMA), FileBrowser
(text, GIF), `tests/sketches/chg_sdtest` (mkdir, a 3000-byte write over
several blocks, read back; results in `sd_result[]` for `--mem`). Sample
content: `c:/github/CHStlView/sample`, `c:/github/CHSpriteView/sample/FIRE`.
Card latency is not modelled (blocks are ready at once); SPI timing is.
`tests/sketches/chg_sdcheck` runs Ethan's Critters' card steps on the device
(init, mount, find CRITTERS.DAT, single reads, 200 streams at 24/12/6 MHz)
plus a hand-made identification logging every raw answer byte, results over
USB serial (BEGIN/END). Built with `--library` on a copy of the game's
vendored CHSd. Found 2026-10-09: CHSd's `cmd()` sends a command straight
after the previous R1, without the 8 idle clocks (N_RC) the SD spec asks for;
the owner's SanDisk 32 GB ("SK32G") then answers misaligned (`01`, `C1 7F`,
`3F`) and goes silent until power is cycled, so every CHSd program says no
card. The SD menu bootloader's `sd.c` sends `x(0xFF)` first and works.
`xfer(0xFF);` at the start of CHSd's `cmd()` fixes it (Ethan's Critters v1.0
rebuilt with it works on the device; reported to bateske). The emulated card
does not need the idle byte, so it cannot show this.

## Working conventions and traps

- Code style: C11, 4-space indent, explanatory block comments about *why*
  (hardware behaviour, measurements). Match it.
- Edit C sources with the editor tools, not Python/heredoc string replacement:
  heredoc-embedded Python turned `\n` inside C string literals into real
  newlines twice in this project.
- Use the scratchpad for temporary files; don't write test output into the
  repo (a stray `NUL` file and a `--save` PPM appeared once from bad paths).
- The GUI can't be observed from a session: verify with `chg_headless` +
  screenshots, and the web build with Puppeteer (script pattern: launch Chrome
  with its own profile, `?rom=roms/...`, keyboard to `#canvas`, F9 for stats,
  screenshot, collect console/page errors). `--virtual-time-budget` screenshots
  never finish because the main loop never idles.
- The owner is joyrider3774. Licence MIT (`LICENSE`); the embedded bootloader is
  MIT (Kevin Bates). Commit/push only when asked.

## Open items (as of 2026-09-30)

- Better game-fps measurement.
- NewBlocksColor runs at ~8x real time vs ~25x for most: profile why (timer
  interrupts for music? RAM code invalidation?).
- DMA start latency: chg_bench test 3 is 0.9% fast vs hardware.
- USB CDC host emulation, so `Serial` output (e.g. `CHGAME_TIMING` frame
  reports) becomes visible.
- Settings persistence (volume, window size) like the TinyJoypad emulator.
