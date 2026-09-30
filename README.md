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
* **Buzzer** on PB10 — driven by GPIO or by TIM1 CH2 PWM (partial remap). The pin
  is integrated exactly over every audio sample, so tones come out clean at any
  frequency.
* **LED** on PB9, shown in the status bar.
* **Saves** — flash pages a game writes are kept in `<game>.sav` beside it.

* **The bootloader** — programs start the way a CHGame starts them: the real
  CHGame bootloader (MIT, from the board package, embedded in
  `src/bootloader_image.c`) sits in the first 12 KB, the loader writes the
  metadata page at 0xF700 the way the uploader does, and the bootloader checks
  the image's CRC and jumps to it. `--no-bootloader` starts at 0x3000 instead.

USB is present as registers only (the core's CDC code runs, no host ever
enumerates it, so `Serial` output is dropped exactly as on a board with no PC
attached). The microSD slot is not emulated.

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
`tools/make_web.sh serve` does all of it: builds, copies `roms/` beside the
page with a `games.json` for its Games menu, and serves it on
http://127.0.0.1:8000/CHGame_Emulator.html with Python.
Programs load from:

* **Open file…** — a file on this computer
* the **URL box**, or `CHGame_Emulator.html?rom=https://…/game.bin`
* `CHGame_Emulator.html?rom=game.bin` — a file in the same folder as the page
* a `games.json` beside the page (`["blips.bin", {"file": "znax.bin", "name": "Znax"}]`)
  fills a Games menu

Saves go to the browser's IndexedDB.

## Running

```sh
./build/CHGame_Emulator path/to/game.bin
```

or drop a file on the window, or press F3.

| | |
|---|---|
| D-pad | Arrow keys or WASD, gamepad d-pad or left stick |
| A / B | X or Space / Z — gamepad east / south button |
| START / SELECT | Enter / Right Shift or Backspace — gamepad Start / Back |
| Help | F1 |
| Reset | F2 |
| Open | F3 |
| Pause / fast forward | P / hold Tab |
| Stats overlay (game fps, speed, MIPS, host load) | F9 |
| Screenshot | F10 |
| Fullscreen | F11 or Alt+Enter |
| Volume | + / - |

The game fps in the overlay is counted at the display: a new frame is a write
window that starts higher up the screen than the one before it.

## Tools

* `chg_headless game.bin [seconds] [out.ppm] [--press btn@sec[:dur]]... [--save]`
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
