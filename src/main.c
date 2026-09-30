/*
 * The SDL3 front end: window, input, audio, and the loop that decides how
 * much to run.
 *
 * Audio is the master clock. Every frame the machine is run for exactly the
 * cycles the audio device still needs samples for, which keeps emulated time
 * locked to real time with no drift and no gaps in the sound. Without an
 * audio device the wall clock is used instead.
 *
 * Written against SDL's main callbacks so the same file runs natively and
 * under Emscripten, where the browser owns the main loop.
 */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "loader.h"
#include "audio.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define AUDIO_RATE      48000
#define AUDIO_TARGET    (AUDIO_RATE / 20)       /* keep ~50 ms queued */
#define MAX_SLICE_S     (1.0 / 15.0)            /* never catch up more than this at once */
#define FAST_FORWARD    8
#define BAR_H           24

static const char *help_text[] = {
    "Arrows / WASD    d-pad",
    "X, Space / Z     A / B",
    "Enter / RShift   START / SELECT",
    "F1               this help",
    "F2               reset",
    "F3               open a program",
    "P  /  hold Tab   pause / fast forward",
    "+ / -            volume",
    "F9               stats overlay",
    "F10              screenshot",
    "F11, Alt+Enter   fullscreen",
    "Esc              quit",
    "",
    "Drop a .bin, .hex or .elf on the window",
};

/* What the F9 overlay shows, measured over the last second */
typedef struct {
    Uint64 t0;              /* ns, start of the current second */
    Uint64 run_ns;          /* host time spent inside chg_run this second */
    uint64_t cycles0, instret0;
    uint32_t lcd0;
    int frames;             /* frames presented this second */
    /* the last complete second */
    float render_fps, game_fps, speed, mhz, mips, host_load, max_speed;
} Stats;

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *screen;
    SDL_AudioStream *audio_stream;
    ChgMachine *m;
    ChgAudio audio;
    char program[1024];
    char save_path[1100];
    bool loaded;
    bool paused;
    bool help;
    char message[256];
    Uint64 message_until;
    Uint64 last_ticks;
    double wall_carry;
    uint16_t pixels[128 * 128];
    float samples[AUDIO_RATE];
    SDL_Gamepad *pads[8];
    uint8_t pad_buttons;
    bool stats;
    Stats st;
} App;

static void show_message(App *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(app->message, sizeof app->message, fmt, ap);
    va_end(ap);
    app->message_until = SDL_GetTicks() + 2500;
}

static void write_save(App *app)
{
    if (app->loaded && app->m->flash_written && chg_write_save(app->m, app->save_path)) {
#ifdef __EMSCRIPTEN__
        /* push the IDBFS copy out so the save survives a reload */
        EM_ASM(FS.syncfs(false, function(err) {}););
#endif
    }
}

static bool load_program(App *app, const char *path)
{
    char err[256];
    write_save(app);
    if (!chg_load_program(app->m, path, err, sizeof err)) {
        show_message(app, "%s", err);
        return false;
    }
    SDL_strlcpy(app->program, path, sizeof app->program);
#ifdef __EMSCRIPTEN__
    const char *base = SDL_strrchr(path, '/');
    base = base ? base + 1 : path;
    char tmp[1100];
    SDL_snprintf(tmp, sizeof tmp, "/saves/%s", base);
    chg_save_path(tmp, app->save_path, sizeof app->save_path);
#else
    chg_save_path(path, app->save_path, sizeof app->save_path);
#endif
    chg_load_save(app->m, app->save_path);
    chg_reset(app->m, true);
    chg_audio_init(&app->audio, AUDIO_RATE, app->m->cycles);
    if (app->audio_stream) SDL_ClearAudioStream(app->audio_stream);
    app->loaded = true;
    app->paused = false;

    const char *name = SDL_strrchr(path, '/');
    const char *bname = SDL_strrchr(path, '\\');
    if (bname > name) name = bname;
    name = name ? name + 1 : path;
    char title[512];
    SDL_snprintf(title, sizeof title, "CHGame Emulator - %s", name);
    SDL_SetWindowTitle(app->window, title);
    show_message(app, "%s", name);
    return true;
}

#ifdef __EMSCRIPTEN__
/* the page's file picker writes the chosen file into the virtual file
   system and hands its path over here */
static App *web_app;

EMSCRIPTEN_KEEPALIVE int chg_web_load(const char *path)
{
    return web_app && load_program(web_app, path);
}
#endif

#ifndef __EMSCRIPTEN__
static void SDLCALL file_chosen(void *userdata, const char *const *files, int filter)
{
    (void)filter;
    App *app = userdata;
    if (files && files[0])
        load_program(app, files[0]);
}
#endif

static void open_dialog(App *app)
{
#ifdef __EMSCRIPTEN__
    /* no native dialog in a browser: the page's own file picker does it */
    (void)app;
    EM_ASM(document.getElementById('file').click(););
#else
    static const SDL_DialogFileFilter filters[] = {
        { "CHGame programs", "bin;hex;elf" },
        { "All files", "*" },
    };
    SDL_ShowOpenFileDialog(file_chosen, app, app->window, filters, 2, NULL, false);
#endif
}

static void screenshot(App *app)
{
    SDL_Surface *s = SDL_CreateSurfaceFrom(128, 128, SDL_PIXELFORMAT_RGB565, app->pixels, 256);
    if (!s) return;
    char path[1200];
    for (int i = 0; i < 1000; i++) {
        SDL_snprintf(path, sizeof path, "%s_%03d.bmp", app->loaded ? app->program : "chgame", i);
        SDL_IOStream *probe = SDL_IOFromFile(path, "rb");
        if (!probe) break;
        SDL_CloseIO(probe);
    }
    if (SDL_SaveBMP(s, path)) show_message(app, "Saved %s", path);
    else show_message(app, "Screenshot failed: %s", SDL_GetError());
    SDL_DestroySurface(s);
}

static uint8_t keyboard_buttons(void)
{
    const bool *k = SDL_GetKeyboardState(NULL);
    uint8_t b = 0;
    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) b |= CHG_BTN_UP;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) b |= CHG_BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) b |= CHG_BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) b |= CHG_BTN_RIGHT;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_K]) b |= CHG_BTN_A;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_J]) b |= CHG_BTN_B;
    if (k[SDL_SCANCODE_RETURN] && !(SDL_GetModState() & SDL_KMOD_ALT)) b |= CHG_BTN_START;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE]) b |= CHG_BTN_SELECT;
    return b;
}

static uint8_t gamepad_buttons(App *app)
{
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        SDL_Gamepad *g = app->pads[i];
        if (!g) continue;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= CHG_BTN_UP;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= CHG_BTN_DOWN;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= CHG_BTN_LEFT;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= CHG_BTN_RIGHT;
        const Sint16 lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
        const Sint16 ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        if (lx < -16000) b |= CHG_BTN_LEFT;
        if (lx > 16000) b |= CHG_BTN_RIGHT;
        if (ly < -16000) b |= CHG_BTN_UP;
        if (ly > 16000) b |= CHG_BTN_DOWN;
        /* the face button in the east is A, as on the CHGame's own layout */
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST)) b |= CHG_BTN_A;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) b |= CHG_BTN_B;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) b |= CHG_BTN_START;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK)) b |= CHG_BTN_SELECT;
    }
    return b;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[])
{
    SDL_SetAppMetadata("CHGame Emulator", "0.1", "com.joyrider3774.chgame_emulator");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    App *app = SDL_calloc(1, sizeof(App));
    app->m = SDL_malloc(sizeof(ChgMachine));
    if (!app || !app->m) return SDL_APP_FAILURE;
    *appstate = app;
    chg_init(app->m);

    const int scale = 4;
    if (!SDL_CreateWindowAndRenderer("CHGame Emulator", 128 * scale, 128 * scale + BAR_H,
                                     SDL_WINDOW_RESIZABLE, &app->window, &app->renderer)) {
        SDL_Log("window: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetRenderVSync(app->renderer, 1);
    app->screen = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 128, 128);
    SDL_SetTextureScaleMode(app->screen, SDL_SCALEMODE_NEAREST);

    SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, AUDIO_RATE };
    app->audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (app->audio_stream) SDL_ResumeAudioStreamDevice(app->audio_stream);
    else SDL_Log("no audio: %s", SDL_GetError());
    chg_audio_init(&app->audio, AUDIO_RATE, 0);

#ifdef __EMSCRIPTEN__
    /* saves live in the browser's IndexedDB. The page reads them in
       (FS.syncfs) before it hands over the first program, see shell.html */
    web_app = app;
    EM_ASM(
        FS.mkdir('/saves');
        FS.mount(IDBFS, {}, '/saves');
    );
#endif

    for (int i = 1; i < argc; i++)
        if (!SDL_strcmp(argv[i], "--no-bootloader"))
            chg_use_bootloader = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            load_program(app, argv[i]);
            break;
        }
    }
    if (!app->loaded)
        show_message(app, "Drop a CHGame .bin here, or press F3");
    app->last_ticks = SDL_GetTicksNS();
    app->st.t0 = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *e)
{
    App *app = appstate;
    switch (e->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_DROP_FILE:
        load_program(app, e->drop.data);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        for (int i = 0; i < 8; i++)
            if (!app->pads[i]) { app->pads[i] = SDL_OpenGamepad(e->gdevice.which); break; }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        for (int i = 0; i < 8; i++)
            if (app->pads[i] && SDL_GetGamepadID(app->pads[i]) == e->gdevice.which) {
                SDL_CloseGamepad(app->pads[i]);
                app->pads[i] = NULL;
            }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) break;
        switch (e->key.key) {
#ifndef __EMSCRIPTEN__
        case SDLK_ESCAPE: return SDL_APP_SUCCESS;
#endif
        case SDLK_F1: app->help = !app->help; break;
        case SDLK_F2:
            if (app->loaded) {
                write_save(app);
                chg_reset(app->m, false);
                show_message(app, "Reset");
            }
            break;
        case SDLK_F3: open_dialog(app); break;
        case SDLK_P:
            app->paused = !app->paused;
            show_message(app, app->paused ? "Paused" : "Running");
            break;
        case SDLK_F9: app->stats = !app->stats; break;
        case SDLK_F10: screenshot(app); break;
        case SDLK_F11:
            SDL_SetWindowFullscreen(app->window, !(SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN));
            break;
        case SDLK_RETURN:
            if (e->key.mod & SDL_KMOD_ALT)
                SDL_SetWindowFullscreen(app->window, !(SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN));
            break;
        case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS:
            app->audio.volume = SDL_min(app->audio.volume + 0.1f, 1.5f);
            show_message(app, "Volume %d%%", (int)(app->audio.volume * 100 + 0.5f));
            break;
        case SDLK_MINUS: case SDLK_KP_MINUS:
            app->audio.volume = SDL_max(app->audio.volume - 0.1f, 0.0f);
            show_message(app, "Volume %d%%", (int)(app->audio.volume * 100 + 0.5f));
            break;
        }
        break;
    }
    return SDL_APP_CONTINUE;
}

/* Runs the machine for this frame and feeds the audio it produced */
static void emulate(App *app)
{
    const Uint64 now = SDL_GetTicksNS();
    const double elapsed = (double)(now - app->last_ticks) / 1e9;
    app->last_ticks = now;
    if (!app->loaded || app->paused)
        return;

    ChgMachine *m = app->m;
    chg_set_buttons(m, keyboard_buttons() | gamepad_buttons(app));
    const Uint64 run0 = SDL_GetTicksNS();

    const bool fast = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB];
    uint64_t cycles;
    if (fast) {
        cycles = (uint64_t)(SDL_min(elapsed, MAX_SLICE_S) * CHG_HCLK * FAST_FORWARD);
        chg_run(m, m->cycles + cycles);
        chg_audio_skip(&app->audio, m);
    } else if (app->audio_stream) {
        const int queued = SDL_GetAudioStreamQueued(app->audio_stream) / (int)sizeof(float);
        int need = AUDIO_TARGET - queued;
        if (need < 0) need = 0;
        if (need > (int)(AUDIO_RATE * MAX_SLICE_S)) need = (int)(AUDIO_RATE * MAX_SLICE_S);
        /* the machine runs to where the audio already rendered ends, plus
           what the device needs */
        const uint64_t until = (uint64_t)(app->audio.pos + need * app->audio.cycles_per_sample);
        if (until > m->cycles) chg_run(m, until);
        const int n = chg_audio_render(&app->audio, m, app->samples, AUDIO_RATE);
        if (n) SDL_PutAudioStreamData(app->audio_stream, app->samples, n * (int)sizeof(float));
    } else {
        app->wall_carry += SDL_min(elapsed, MAX_SLICE_S) * CHG_HCLK;
        cycles = (uint64_t)app->wall_carry;
        app->wall_carry -= (double)cycles;
        chg_run(m, m->cycles + cycles);
        chg_audio_skip(&app->audio, m);
    }

    app->st.run_ns += SDL_GetTicksNS() - run0;

    /* saves go to disk a second after the program stops writing flash */
    if (m->flash_written && m->cycles - m->flash_write_cycle > CHG_HCLK)
        write_save(app);
}

static void draw_text(SDL_Renderer *r, float x, float y, const char *s)
{
    SDL_RenderDebugText(r, x, y, s);
}

/* Once a second: what happened since the last time */
static void update_stats(App *app)
{
    Stats *st = &app->st;
    ChgMachine *m = app->m;
    st->frames++;
    const Uint64 now = SDL_GetTicksNS();
    const double secs = (double)(now - st->t0) / 1e9;
    if (secs < 1.0)
        return;
    const double cycles = (double)(m->cycles - st->cycles0);
    const double run = (double)st->run_ns / 1e9;
    st->render_fps = (float)(st->frames / secs);
    st->game_fps = (float)((m->lcd.frame_starts - st->lcd0) / secs);
    st->mhz = (float)(cycles / secs / 1e6);
    st->speed = (float)(cycles / secs / CHG_HCLK * 100.0);
    st->mips = (float)((double)(m->instret - st->instret0) / secs / 1e6);
    st->host_load = (float)(run / secs * 100.0);
    st->max_speed = run > 0 ? (float)(cycles / run / CHG_HCLK) : 0;
    st->t0 = now;
    st->run_ns = 0;
    st->frames = 0;
    st->cycles0 = m->cycles;
    st->instret0 = m->instret;
    st->lcd0 = m->lcd.frame_starts;
}

static void draw_stats(App *app, int w)
{
    const Stats *st = &app->st;
    const ChgMachine *m = app->m;
    char lines[8][64];
    int n = 0;
    SDL_snprintf(lines[n++], 64, "game   %5.1f fps", st->game_fps);
    SDL_snprintf(lines[n++], 64, "speed  %5.1f%%  %5.2f MHz", st->speed, st->mhz);
    SDL_snprintf(lines[n++], 64, "guest  %5.2f MIPS", st->mips);
    SDL_snprintf(lines[n++], 64, "host   %5.1f%% cpu  max %.1fx", st->host_load, st->max_speed);
    SDL_snprintf(lines[n++], 64, "render %5.1f fps", st->render_fps);
    SDL_snprintf(lines[n++], 64, "pc     %08X", m->cpu.pc);
    SDL_Renderer *r = app->renderer;
    float bw = 0;
    for (int i = 0; i < n; i++) bw = SDL_max(bw, (float)SDL_strlen(lines[i]) * 8);
    const float x = (float)w - bw - 12;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
    SDL_FRect box = { x - 4, 4, bw + 8, 12.0f * (float)n + 6 };
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 120, 255, 120, 255);
    for (int i = 0; i < n; i++)
        draw_text(r, x, 8 + 12.0f * (float)i, lines[i]);
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    App *app = appstate;
    emulate(app);

    SDL_Renderer *r = app->renderer;
    int w, h;
    SDL_GetCurrentRenderOutputSize(r, &w, &h);
    SDL_SetRenderDrawColor(r, 24, 24, 28, 255);
    SDL_RenderClear(r);

    const bool full = (SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN) != 0;
    const int bar = full ? 0 : BAR_H;
    int avail_h = h - bar;
    int scale = SDL_min(w / 128, avail_h / 128);
    float size = scale >= 1 ? (float)(scale * 128) : (float)SDL_min(w, avail_h);
    SDL_FRect dst = { (w - size) / 2.0f, (avail_h - size) / 2.0f, size, size };

    st7735_render(&app->m->lcd, app->pixels);
    SDL_UpdateTexture(app->screen, NULL, app->pixels, 128 * 2);
    SDL_RenderTexture(r, app->screen, NULL, &dst);

    if (!full) {
        /* the LED, then what is going on */
        const float by = (float)(h - bar);
        SDL_SetRenderDrawColor(r, 40, 40, 46, 255);
        SDL_FRect barRect = { 0, by, (float)w, (float)bar };
        SDL_RenderFillRect(r, &barRect);
        if (app->m->led) SDL_SetRenderDrawColor(r, 255, 40, 40, 255);
        else SDL_SetRenderDrawColor(r, 70, 20, 20, 255);
        SDL_FRect led = { 8, by + 8, 8, 8 };
        SDL_RenderFillRect(r, &led);
        SDL_SetRenderDrawColor(r, 200, 200, 200, 255);
        char status[128];
        if (app->m->cpu.faulted)
            SDL_snprintf(status, sizeof status, "CPU fault %u at %08X", app->m->cpu.fault_cause, app->m->cpu.fault_pc);
        else
            SDL_snprintf(status, sizeof status, "%s%.0f fps  %.0f%%", app->paused ? "PAUSED  " : "",
                         app->st.game_fps, app->st.speed);
        draw_text(r, 24, by + 8, status);
        draw_text(r, (float)w - 8 * 9, by + 8, "F1: help");
    }

    if (app->help) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 210);
        SDL_FRect all = { 0, 0, (float)w, (float)h };
        SDL_RenderFillRect(r, &all);
        SDL_SetRenderDrawColor(r, 230, 230, 230, 255);
        for (size_t i = 0; i < sizeof(help_text) / sizeof(help_text[0]); i++)
            draw_text(r, 16, 16 + 12.0f * (float)i, help_text[i]);
    }
    if (app->stats)
        draw_stats(app, w);
    if (SDL_GetTicks() < app->message_until) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        const float tw = (float)SDL_strlen(app->message) * 8;
        SDL_SetRenderDrawColor(r, 0, 0, 0, 180);
        SDL_FRect box = { 4, 4, tw + 8, 16 };
        SDL_RenderFillRect(r, &box);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        draw_text(r, 8, 8, app->message);
    }
    SDL_RenderPresent(r);

    update_stats(app);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    (void)result;
    App *app = appstate;
    if (!app) return;
    write_save(app);
    SDL_free(app->m);
    SDL_free(app);
}
