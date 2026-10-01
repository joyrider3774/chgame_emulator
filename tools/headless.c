/*
 * Runs a program with no window: for testing and for measuring speed.
 *
 *   chg_headless game.bin [seconds] [out.ppm] [--press BTN@sec[:dur]]...
 *
 * Prints how fast the emulation ran against real time and writes what the
 * screen shows at the end as a PPM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "machine.h"
#include "sdcard.h"
#include "loader.h"
#include "audio.h"

#ifdef _WIN32
#include <windows.h>
static double now_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#endif

static void write_ppm(const char *path, const uint16_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n128 128\n255\n");
    for (int i = 0; i < 128 * 128; i++) {
        const uint16_t c = px[i];
        const uint8_t rgb[3] = { (uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

typedef struct { uint8_t btn; double at, dur; } Press;

static uint8_t btn_by_name(const char *s)
{
    static const struct { const char *n; uint8_t b; } t[] = {
        { "up", CHG_BTN_UP }, { "down", CHG_BTN_DOWN }, { "left", CHG_BTN_LEFT }, { "right", CHG_BTN_RIGHT },
        { "a", CHG_BTN_A }, { "b", CHG_BTN_B }, { "select", CHG_BTN_SELECT }, { "start", CHG_BTN_START },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(s, t[i].n)) return t[i].b;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--sd-make")) {
        /* chg_headless --sd-make card.img [folder] [MB]: a card image */
        const char *folder = argc >= 4 && atoi(argv[3]) == 0 ? argv[3] : NULL;
        char err[512];
        if (!sdcard_make_image(argv[2], folder, (uint32_t)atoi(argv[argc - 1]), err, sizeof err)) {
            fprintf(stderr, "%s\n", err);
            return 1;
        }
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: chg_headless game.bin [seconds] [out.ppm] [--press btn@sec[:dur]]... "
                        "[--shots prefix] [--save] [--sd folder|card.img]\n"
                        "       chg_headless --sd-make card.img [folder] [MB]\n");
        return 2;
    }
    static ChgMachine m;
    static ChgAudio audio;
    chg_init(&m);
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--no-bootloader"))
            chg_use_bootloader = false;
    char err[256];
    if (!chg_load_program(&m, argv[1], err, sizeof err)) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    chg_reset(&m, true);

    double seconds = 5;
    const char *out = "screen.ppm";
    const char *shots = NULL;
    const char *wav_path = NULL;
    bool no_piezo = true;      /* the bare pin signal; --piezo for the piezo filter */
    bool save = false;
    uint32_t mem_addr = 0, mem_count = 0;
    Press presses[64];
    int npress = 0;
    int pos = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            char name[32];
            double at = 0, dur = 0.15;
            const char *spec = argv[++i];
            const char *atp = strchr(spec, '@');
            if (!atp) continue;
            snprintf(name, sizeof name, "%.*s", (int)(atp - spec), spec);
            sscanf(atp + 1, "%lf:%lf", &at, &dur);
            if (npress < 64) presses[npress++] = (Press){ btn_by_name(name), at, dur };
        } else if (!strcmp(argv[i], "--mem") && i + 2 < argc) {
            /* dump words of RAM at the end, e.g. a results array found with nm */
            mem_addr = (uint32_t)strtoul(argv[i + 1], NULL, 16);
            mem_count = (uint32_t)strtoul(argv[i + 2], NULL, 0);
            i += 2;
        } else if (!strcmp(argv[i], "--no-bootloader")) {
            /* handled before loading */
        } else if (!strcmp(argv[i], "--save")) {
            save = true;
        } else if (!strcmp(argv[i], "--sd") && i + 1 < argc) {
            /* the microSD card: a folder or an image (made, empty, if missing) */
            if (!(m.sd = sdcard_open(argv[++i], 0, err, sizeof err))) {
                fprintf(stderr, "SD card: %s\n", err);
                return 1;
            }
        } else if (!strcmp(argv[i], "--shots") && i + 1 < argc) {
            shots = argv[++i];
        } else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            wav_path = argv[++i];
        } else if (!strcmp(argv[i], "--no-piezo")) {
            no_piezo = true;
        } else if (!strcmp(argv[i], "--piezo")) {
            no_piezo = false;
        } else if (pos == 0) {
            seconds = atof(argv[i]);
            pos++;
        } else {
            out = argv[i];
        }
    }

    char save_path[1100];
    chg_save_path(argv[1], save_path, sizeof save_path);
    if (save && chg_load_save(&m, save_path))
        chg_reset(&m, true);
    chg_audio_init(&audio, 48000, m.cycles);
    audio.piezo = !no_piezo;
    static float samples[48000];
    double peak = 0;
    /* --wav: the sound as the front end would play it, 48 kHz 16-bit mono */
    FILE *wav = wav_path ? fopen(wav_path, "wb") : NULL;
    uint32_t wav_samples = 0;
    if (wav) {
        static const uint8_t blank[44] = { 0 };
        fwrite(blank, 1, 44, wav);
    }

    const uint64_t step = CHG_HCLK / 100;  /* 10 ms slices, as a front end would */
    const uint64_t total = (uint64_t)(seconds * CHG_HCLK);
    const double t0 = now_s();
    uint64_t instr_est = 0;
    int shot = 0;
    static uint16_t px[128 * 128];
    while (m.cycles < total) {
        const double t = (double)m.cycles / CHG_HCLK;
        uint8_t held = 0;
        for (int i = 0; i < npress; i++)
            if (t >= presses[i].at && t < presses[i].at + presses[i].dur) held |= presses[i].btn;
        chg_set_buttons(&m, held);
        chg_run(&m, m.cycles + step);
        const int n = chg_audio_render(&audio, &m, samples, 48000);
        for (int i = 0; i < n; i++) if (samples[i] > peak) peak = samples[i];
        if (wav) {
            for (int i = 0; i < n; i++) {
                float v = samples[i] * 32767.0f;
                v = v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v;
                const int16_t s16 = (int16_t)v;
                fwrite(&s16, 2, 1, wav);
            }
            wav_samples += (uint32_t)n;
        }
        if (shots && (int)t != shot) {
            shot = (int)t;
            char name[512];
            snprintf(name, sizeof name, "%s_%03d.ppm", shots, shot);
            st7735_render(&m.lcd, px);
            write_ppm(name, px);
        }
    }
    const double wall = now_s() - t0;
    if (wav) {
        /* the RIFF header, now the length is known */
        uint8_t h[44];
        const uint32_t bytes = wav_samples * 2;
        const uint32_t fields[] = { 36 + bytes, 16, 1 | 1u << 16, 48000, 48000 * 2, 2 | 16u << 16, bytes };
        memcpy(h, "RIFF", 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 36, "data", 4);
        const int at[] = { 4, 16, 20, 24, 28, 32, 40 };
        for (int k = 0; k < 7; k++)
            for (int b = 0; b < 4; b++) h[at[k] + b] = (uint8_t)(fields[k] >> (8 * b));
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fclose(wav);
    }
    (void)instr_est;
    printf("emulated %.2f s in %.3f s wall: %.1fx real time (%.0f MHz equivalent)\n",
           seconds, wall, seconds / wall, seconds * CHG_HCLK / wall / 1e6);
    printf("game %.1f fps (LCD frame starts)  guest %.2f MIPS (%.2f cycles/instr)\n",
           m.lcd.frame_starts / seconds, m.instret / seconds / 1e6, (double)m.cycles / (double)m.instret);
    printf("pc %08x  frames written %u  display %s  led %d  audio peak %.3f\n", m.cpu.pc,
           m.lcd.frames_written, m.lcd.display_on ? "on" : "off", m.led, peak);
    if (m.cpu.faulted)
        printf("FAULT: mcause %u at %08x mtval %08x\n", m.cpu.fault_cause, m.cpu.fault_pc, m.cpu.fault_tval);
    if (getenv("CHG_REGS")) {
        static const char *abi[32] = { "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1",
            "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
            "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };
        for (int i = 1; i < 32; i++)
            printf("%-4s %08x%s", abi[i], m.cpu.x[i], i % 6 == 0 ? "\n" : "  ");
        printf("\nmstatus %08x mepc %08x mcause %08x trap depth %d\n", m.cpu.mstatus, m.cpu.mepc,
               m.cpu.mcause, m.cpu.trap_depth);
    }
    for (uint32_t i = 0; i < mem_count; i++) {
        const uint32_t off = mem_addr - CHG_RAM_BASE + i * 4;
        uint32_t v = 0;
        if (off + 4 <= CHG_RAM_SIZE) memcpy(&v, m.ram + off, 4);
        printf("mem %08x %u\n", mem_addr + i * 4, v);
    }
    if (save && m.flash_written && chg_write_save(&m, save_path))
        printf("saved %s\n", save_path);
    st7735_render(&m.lcd, px);
    write_ppm(out, px);
    sdcard_close(m.sd);
    return 0;
}
