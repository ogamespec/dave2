/* platform.c -- SDL3 platform layer.  See platform.h for the layer split.
 *
 * Everything here is host-facing: window, renderer, texture, audio device,
 * keyboard/gamepad, timers and logging.  No game logic lives in this file.
 */
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "video.h"
#include "dd2_icon.h"

#define PLAT_PIT_HZ 1193182

static SDL_Window   *p_win;
static SDL_Renderer *p_ren;
static SDL_Texture  *p_tex;
static uint32_t     *p_rgba;
static int           p_w, p_h;
static int           p_quit;

/* ---------------------------------------------------------------- system */
uint32_t Plat_Ticks(void) { return SDL_GetTicks(); }

const char *Plat_BasePath(void) { return SDL_GetBasePath(); }

int write_bmp(const char *path, const void *pixels, int w, int h)
{
    const uint32_t *px = pixels;
    FILE *f = fopen(path, "wb");
    unsigned char hdr[54] = { 'B', 'M' };
    int i, size = w * h * 3, pad = (4 - (w * 3) % 4) % 4;
    unsigned char z[3] = { 0, 0, 0 };
    if (!f) return 0;
    *(int *)(hdr + 2) = 54 + size + pad * h;
    *(int *)(hdr + 10) = 54;
    *(int *)(hdr + 14) = 40;
    *(int *)(hdr + 18) = w;
    *(int *)(hdr + 22) = -h;
    *(short *)(hdr + 26) = 1;
    *(short *)(hdr + 28) = 24;
    fwrite(hdr, 1, 54, f);
    for (i = 0; i < w * h; i++) {
        unsigned char bgr[3] = { (unsigned char)(px[i] & 0xFF),
                                 (unsigned char)((px[i] >> 8) & 0xFF),
                                 (unsigned char)((px[i] >> 16) & 0xFF) };
        fwrite(bgr, 1, 3, f);
        if ((i + 1) % w == 0 && pad) fwrite(z, 1, pad, f);
    }
    fclose(f);
    return 1;
}

int Plat_ReadPixelsBmp(const char *path)
{
    SDL_Surface *surf = SDL_RenderReadPixels(p_ren, NULL);
    int ok = 0;
    if (surf) {
        SDL_Surface *conv = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_XRGB8888);
        if (conv) {
            ok = write_bmp(path, conv->pixels, conv->w, conv->h);
            SDL_DestroySurface(conv);
        }
        SDL_DestroySurface(surf);
    }
    return ok;
}

/* ------------------------------------------------------------------ misc */
void Plat_Log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ----------------------------------------------------------------- video */
int Plat_VideoInit(int scale)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        Plat_Log("SDL_Init: %s", SDL_GetError());
        return 0;
    }
    /* 0x72E8 get the gamepad/joystick subsystem up: SDL_GetGamepads() reports
       nothing until SDL_INIT_GAMEPAD (or SDL_INIT_JOYSTICK) was requested, so
       without this the whole joystick path (0x72E8/0x73AE/0x7386, the F4
       calibration screen) was unreachable.  Failure is not fatal: a machine
       without joystick drivers must still run the game. */
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
        Plat_Log("gamepad subsystem unavailable: %s", SDL_GetError());
    p_win = SDL_CreateWindow("Dangerous Dave in the Haunted Mansion (SDL3 port)",
                             scale * 320, scale * 200, 0);
    if (!p_win) { Plat_Log("CreateWindow: %s", SDL_GetError()); return 0; }
    {   /* window/taskbar icon: the game's own marker sprite (see dd2_icon.h) */
        SDL_Surface *ic = SDL_CreateSurfaceFrom(DD2_ICON_W, DD2_ICON_H_,
                                                SDL_PIXELFORMAT_RGBA32,
                                                (void *)dd2_icon_rgba,
                                                DD2_ICON_W * 4);
        if (ic) { SDL_SetWindowIcon(p_win, ic); SDL_DestroySurface(ic); }
    }
    {   /* Headless (dummy driver) runs skip the original's timer pauses
           (0x7938 = 0x64 ticks); a real window keeps them. */
        const char *drv = SDL_GetCurrentVideoDriver();
        g_fast = (drv && strcmp(drv, "dummy") == 0);
    }
    p_ren = SDL_CreateRenderer(p_win, NULL);
    if (!p_ren) {
        Plat_Log("default renderer failed (%s), trying software", SDL_GetError());
        p_ren = SDL_CreateRenderer(p_win, "software");
    }
    if (!p_ren) { Plat_Log("CreateRenderer: %s", SDL_GetError()); return 0; }
    /* XRGB: the EGA palette is opaque; an alpha channel here would make the
       texture blend to nothing (black window) on some backends. */
    p_tex = SDL_CreateTexture(p_ren, SDL_PIXELFORMAT_XRGB8888,
                              SDL_TEXTUREACCESS_STREAMING, 320, 200);
    if (!p_tex) { Plat_Log("CreateTexture: %s", SDL_GetError()); return 0; }
    SDL_SetTextureBlendMode(p_tex, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(p_tex, SDL_SCALEMODE_NEAREST);
    SDL_SetRenderLogicalPresentation(p_ren, 320, 200,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    p_rgba = malloc(320 * 200 * sizeof *p_rgba);
    if (!p_rgba) { Plat_Log("out of memory"); return 0; }
    p_w = 320; p_h = 200;
    return 1;
}

void Plat_VideoQuit(void)
{
    free(p_rgba); p_rgba = NULL;
    if (p_tex) SDL_DestroyTexture(p_tex);
    if (p_ren) SDL_DestroyRenderer(p_ren);
    if (p_win) SDL_DestroyWindow(p_win);
    p_tex = NULL; p_ren = NULL; p_win = NULL;
}

void Plat_MessageBox(const char *title, const char *msg)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, NULL);
}

void Plat_Quit(void) { SDL_Quit(); }

void Plat_SetTitle(const char *title)
{
    if (p_win) SDL_SetWindowTitle(p_win, title);
}

/* Show one indexed frame.  `pal` holds 16 opaque XRGB colours. */
void Plat_Present(const uint8_t *idx, int w, int h, const uint32_t *pal)
{
    int i, n = w * h;
    if (!p_tex || !p_rgba) return;
    if (w > p_w) w = p_w;
    if (h > p_h) h = p_h;
    n = w * h;
    for (i = 0; i < n; i++) p_rgba[i] = pal[idx[i] & 15];
    SDL_UpdateTexture(p_tex, NULL, p_rgba, w * 4);
    SDL_SetRenderDrawColor(p_ren, 0, 0, 0, 255);
    SDL_RenderClear(p_ren);
    SDL_RenderTexture(p_ren, p_tex, NULL, NULL);
    SDL_RenderPresent(p_ren);
}

static void (*p_idle_hook)(void);

void Plat_SetIdleHook(void (*fn)(void)) { p_idle_hook = fn; }

void Plat_Delay(int ms)
{
    /* Sleep in small slices and give the caller's idle hook a chance to run:
       the original's 140 Hz timer ISR keeps the PC-speaker sequence going
       while the main loop sits inside a blocking animation (0x8EC3), so the
       sound must not wait for the next game frame. */
    while (ms > 0) {
        int step = ms > 7 ? 7 : ms;
        SDL_Delay((Uint32)step);
        ms -= step;
        if (p_idle_hook) p_idle_hook();
    }
}

/* ----------------------------------------------------------------- audio */
static SDL_AudioStream *p_stream;
static volatile int     p_divisor;      /* 0 = silence */
static volatile int     p_gated;        /* 1 = direct 0xC7FA tone: gate it */
static double           p_phase;        /* square-wave phase 0..1 */
static int              p_gate_pos;     /* sample position inside the gate period */
static double           p_level;        /* current unipolar speaker level */
static double           p_dc_x, p_dc_y; /* DC-blocking (coupling capacitor) state */
static double           p_lp;           /* speaker-cone low-pass state */
static int              p_said;         /* diagnostics: first tone reported */

/* The intro sets its tone directly (0xC7FA) once per 70 Hz frame; the 140 Hz
   sound ISR silences the speaker at 0x8DCA on its next tick, so the tone lives
   0..7 ms and the fly-in is a ~70 Hz buzz, not a note.  Doing that with frame
   timing jitter smears the buzz away (measured: 1.8% of the energy below
   120 Hz against 15.9% in the original), so the gate is applied here,
   sample-accurately: 3.6 ms of tone per 1/70 s. */
#define PLAT_GATE_PERIOD  (44100 / 70)
#define PLAT_GATE_ON      (44100 * 36 / 10000)

static void SDLCALL plat_audio_cb(void *userdata, SDL_AudioStream *st,
                                  int additional, int total)
{
    static Sint16 buf[4096];
    int n, i;
    (void)userdata; (void)total;
    if (additional <= 0) return;
    n = additional / (int)sizeof(Sint16);
    if (n > (int)(sizeof buf / sizeof buf[0])) n = (int)(sizeof buf / sizeof buf[0]);
    for (i = 0; i < n; i++) {
        int d = p_divisor;
        int on = 1;
        if (d > 0 && p_gated) {
            if (p_gate_pos >= PLAT_GATE_PERIOD) p_gate_pos -= PLAT_GATE_PERIOD;
            on = (p_gate_pos < PLAT_GATE_ON);
            p_gate_pos++;
        } else {
            p_gate_pos = 0;
        }
        if (d <= 0 || !on) {
            p_level = 0.0;
            if (d <= 0) p_phase = 0.0;
        } else {
            double f = (double)PLAT_PIT_HZ / (double)d;   /* tone frequency */
            double step = f / 44100.0;
            p_phase += step;
            if (p_phase >= 1.0) p_phase -= (double)(long)p_phase;
            /* The PC speaker is driven unipolar (0 / +5 V), not as a symmetric
               square.  That matters: a symmetric burst averages to zero, so the
               70 Hz gate produces no fundamental and the fly-in loses its
               "brrr".  Measured against the original, the unipolar burst puts
               the 70 Hz line above the tone, as in the hardware. */
            p_level = (p_phase < 0.5) ? 6000.0 : 0.0;
            if (!p_said) {
                p_said = 1;
                Plat_Log("sound: first tone (divisor %d)", d);
            }
        }
        /* The speaker is capacitor-coupled, so take the DC back out; the 70 Hz
           gate survives (measured: low/tone ratio 2.95 -> 2.87). */
        {
            double y = p_level - p_dc_x + 0.99858 * p_dc_y;   /* ~10 Hz HP */
            p_dc_x = p_level;
            p_dc_y = y;
            /* ...and the cone rolls the top off: a one-pole ~2 kHz low-pass
               brings the band balance in line with the reference recording
               (4-9 kHz: 10.8% here vs 10.9% original, was 18.4%). */
            p_lp += 0.248 * (y - p_lp);
            y = p_lp;
            if (y >  32000.0) y =  32000.0;
            if (y < -32000.0) y = -32000.0;
            buf[i] = (Sint16)y;
        }
    }
    SDL_PutAudioStreamData(st, buf, n * (int)sizeof(Sint16));
}

void Plat_AudioInit(void)
{
    SDL_AudioSpec spec;
    if (p_stream) return;
    /* The audio subsystem must be up before the stream can be opened;
       without this the stream failed to open and the game was silent. */
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    /* The PC-speaker engine changes the PIT divisor far faster than a game
       frame: the intro's 0xC7FA tone is cut by the 140 Hz ISR (see
       Sound_Tick), and the sound effects are stepped at 140 Hz too.  A large
       device buffer smears those steps into a hum, so keep it small. */
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "128");
    SDL_zero(spec);
    spec.format   = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq     = 44100;
    p_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                         &spec, plat_audio_cb, NULL);
    if (p_stream) {
        SDL_ResumeAudioStreamDevice(p_stream);
        Plat_Log("sound: stream opened (%d Hz, mono, S16)", spec.freq);
    } else {
        Plat_Log("sound: cannot open audio device: %s", SDL_GetError());
    }
}

void Plat_AudioQuit(void)
{
    if (p_stream) SDL_DestroyAudioStream(p_stream);
    p_stream = NULL;
}

void Plat_AudioTone(int divisor) { p_divisor = divisor; p_gated = 0; }
/* The intro's direct 0xC7FA tone: same divisor, but gated at 70 Hz in the audio
   callback to reproduce the ISR cut (0x8DCA) without frame-timer jitter. */
void Plat_AudioToneGated(int divisor) { p_divisor = divisor; p_gated = (divisor > 0); }
/* 1 while such a tone is active: the idle 140 Hz ISR must not silence it,
   because the callback already emulates that cut. */
int Plat_AudioGated(void) { return p_gated; }

int Plat_AudioReady(void) { return p_stream != NULL; }

/* ----------------------------------------------------------------- input
 * The rest of the port uses DOS scan codes (the original's INT 9 codes), so
 * the platform keeps a translated 128-entry state array and refreshes it once
 * per frame.  SDL key codes never leave this file.
 */
static int dos_scancode_char(int sc);

static bool dos_keys[128];

/* SDL scancode -> DOS scan code (set 1).  Only the keys the game uses. */
static const struct { SDL_Scancode sdl; int dos; } key_map[] = {
    { SDL_SCANCODE_ESCAPE,    0x01 }, { SDL_SCANCODE_BACKSPACE, 0x0E },
    { SDL_SCANCODE_TAB,       0x0F }, { SDL_SCANCODE_RETURN,    0x1C },
    { SDL_SCANCODE_LCTRL,     0x1D }, { SDL_SCANCODE_LSHIFT,    0x2A },
    { SDL_SCANCODE_RSHIFT,    0x36 }, { SDL_SCANCODE_LALT,      0x38 },
    { SDL_SCANCODE_SPACE,     0x39 }, { SDL_SCANCODE_CAPSLOCK,  0x3A },
    { SDL_SCANCODE_F1,        0x3B }, { SDL_SCANCODE_F2,        0x3C },
    { SDL_SCANCODE_F3,        0x3D }, { SDL_SCANCODE_F4,        0x3E },
    { SDL_SCANCODE_F5,        0x3F }, { SDL_SCANCODE_F6,        0x40 },
    { SDL_SCANCODE_F7,        0x41 }, { SDL_SCANCODE_F8,        0x42 },
    { SDL_SCANCODE_F9,        0x43 }, { SDL_SCANCODE_F10,       0x44 },
    { SDL_SCANCODE_F11,       0x57 }, { SDL_SCANCODE_F12,       0x58 },
    { SDL_SCANCODE_UP,        0x48 }, { SDL_SCANCODE_DOWN,      0x50 },
    { SDL_SCANCODE_LEFT,      0x4B }, { SDL_SCANCODE_RIGHT,     0x4D },
    { SDL_SCANCODE_HOME,      0x47 }, { SDL_SCANCODE_END,       0x4F },
    { SDL_SCANCODE_PAGEUP,    0x49 }, { SDL_SCANCODE_PAGEDOWN,  0x51 },
    { SDL_SCANCODE_Y,         0x15 }, { SDL_SCANCODE_N,         0x31 },
    { SDL_SCANCODE_A,         0x1E }, { SDL_SCANCODE_B,         0x30 },
    { SDL_SCANCODE_C,         0x2E }, { SDL_SCANCODE_D,         0x20 },
    { SDL_SCANCODE_E,         0x12 }, { SDL_SCANCODE_F,         0x21 },
    { SDL_SCANCODE_G,         0x22 }, { SDL_SCANCODE_H,         0x23 },
    { SDL_SCANCODE_I,         0x17 }, { SDL_SCANCODE_J,         0x24 },
    { SDL_SCANCODE_K,         0x25 }, { SDL_SCANCODE_L,         0x26 },
    { SDL_SCANCODE_M,         0x32 }, { SDL_SCANCODE_O,         0x18 },
    { SDL_SCANCODE_P,         0x19 }, { SDL_SCANCODE_Q,         0x10 },
    { SDL_SCANCODE_R,         0x13 }, { SDL_SCANCODE_S,         0x1F },
    { SDL_SCANCODE_T,         0x14 }, { SDL_SCANCODE_U,         0x16 },
    { SDL_SCANCODE_V,         0x2F }, { SDL_SCANCODE_W,         0x11 },
    { SDL_SCANCODE_X,         0x2D }, { SDL_SCANCODE_Z,         0x2C },
    { SDL_SCANCODE_1,         0x02 }, { SDL_SCANCODE_2,         0x03 },
    { SDL_SCANCODE_3,         0x04 }, { SDL_SCANCODE_4,         0x05 },
    { SDL_SCANCODE_5,         0x06 }, { SDL_SCANCODE_6,         0x07 },
    { SDL_SCANCODE_7,         0x08 }, { SDL_SCANCODE_8,         0x09 },
    { SDL_SCANCODE_9,         0x0A }, { SDL_SCANCODE_0,         0x0B },
    { SDL_SCANCODE_MINUS,     0x0C }, { SDL_SCANCODE_EQUALS,    0x0D },
    { SDL_SCANCODE_COMMA,     0x33 }, { SDL_SCANCODE_PERIOD,    0x34 },
    { SDL_SCANCODE_SLASH,     0x35 }, { SDL_SCANCODE_SEMICOLON, 0x27 },
    { SDL_SCANCODE_APOSTROPHE,0x28 }, { SDL_SCANCODE_GRAVE,     0x29 },
    { SDL_SCANCODE_LEFTBRACKET, 0x1A }, { SDL_SCANCODE_RIGHTBRACKET, 0x1B },
    { SDL_SCANCODE_BACKSLASH, 0x2B },
    { SDL_SCANCODE_UNKNOWN, 0 }
};

/* DOS scan code -> printable ASCII, the usual set-1 layout. */
static int dos_scancode_char(int sc)
{
    static const char *row1 = "1234567890-=";
    static const char *row2 = "qwertyuiop[]";
    static const char *row3 = "asdfghjkl;'";
    static const char *row4 = "zxcvbnm,./";
    if (sc == 0x39) return ' ';
    if (sc == 0x0E) return 8;                 /* backspace */
    if (sc == 0x1C) return 0x0D;              /* enter */
    if (sc == 0x01) return 0x1B;              /* esc */
    if (sc >= 0x02 && sc <= 0x0D) return row1[sc - 0x02];
    if (sc >= 0x10 && sc <= 0x1B) return row2[sc - 0x10];
    if (sc >= 0x1E && sc <= 0x28) return row3[sc - 0x1E];
    if (sc >= 0x2C && sc <= 0x35) return row4[sc - 0x2C];
    return 0;
}

static void plat_refresh_keys(void)
{
    const bool *st = SDL_GetKeyboardState(NULL);
    int i;
    memset(dos_keys, 0, sizeof dos_keys);
    for (i = 0; key_map[i].sdl != SDL_SCANCODE_UNKNOWN; i++)
        if (st[key_map[i].sdl]) dos_keys[key_map[i].dos] = true;
}

void Plat_PumpEvents(void)
{
    SDL_Event ev;
    p_quit = 0;
    while (SDL_PollEvent(&ev))
        if (ev.type == SDL_EVENT_QUIT) p_quit = 1;
    plat_refresh_keys();
}

int Plat_QuitRequested(void) { return p_quit; }

const bool *Plat_Keys(void) { return dos_keys; }

/* Wait for a key, bounded so the game (and the tests) can never hang. */
int Plat_WaitScancode(int timeout_ms)
{
    SDL_Event e;
    SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP);
    for (;;) {
        if (!SDL_WaitEventTimeout(&e, timeout_ms)) return 0;   /* timeout */
        if (e.type == SDL_EVENT_QUIT) return -1;
        if (e.type != SDL_EVENT_KEY_DOWN) continue;
        {
            int i;
            for (i = 0; key_map[i].sdl != SDL_SCANCODE_UNKNOWN; i++)
                if (key_map[i].sdl == e.key.scancode) return key_map[i].dos;
        }
    }
}

int Plat_ScancodeChar(int scancode) { return dos_scancode_char(scancode); }

const char *Plat_ScancodeName(int scancode)
{
    return SDL_GetScancodeName((SDL_Scancode)scancode);
}

/* ------------------------------------------------------- gamepad (0x72E8) */
static SDL_Gamepad *p_pad;
static int          p_pad_ok;

static void plat_open_pad(void)
{
    int n = 0;
    SDL_JoystickID *ids;
    /* A disconnected gamepad must not disable the joystick for the rest of the
       session: drop the dead handle and re-open on the next poll. */
    if (p_pad && !SDL_GamepadConnected(p_pad)) {
        SDL_CloseGamepad(p_pad);
        p_pad = NULL;
        p_pad_ok = 0;
    }
    ids = SDL_GetGamepads(&n);                      /* 0x72E8 */
    if (ids) {
        if (n > 0) {
            p_pad = SDL_OpenGamepad(ids[0]);
            p_pad_ok = p_pad ? 1 : 0;
        }
        SDL_free(ids);
    }
    if (p_pad) Plat_Log("gamepad: %s", SDL_GetGamepadName(p_pad));
}

/* 0x73AE: command from the analogue axes (threshold 500, 3x3 grid index). */
/* DS:0x9328/0x932E (x) and DS:0x9334/0x933A (y): the low/high thresholds the
   F4 calibration (0x6BB2 -> 0x6D5E-0x6DAD) computes from the two corner
   readings -- low = min + range/6, high = max - range/6.  Until F4 runs the
   original keeps [0x97D6] = 0 (0x1705) and never reads the joystick at all. */
static int p_thr[4] = { -500, 500, -500, 500 };

void Plat_PadSetThresholds(int xlo, int xhi, int ylo, int yhi)
{
    p_thr[0] = xlo; p_thr[1] = xhi; p_thr[2] = ylo; p_thr[3] = yhi;
}

int Plat_PadRaw(int *x, int *y)
{
    if (!p_pad_ok || !p_pad) {
        plat_open_pad();
        if (!p_pad_ok) return 0;
    }
    if (x) *x = SDL_GetGamepadAxis(p_pad, SDL_GAMEPAD_AXIS_LEFTX);
    if (y) *y = SDL_GetGamepadAxis(p_pad, SDL_GAMEPAD_AXIS_LEFTY);
    return 1;
}

int Plat_PadCmd(void)
{
    int ax, ay, dx, dy;
    if (!Plat_PadRaw(&ax, &ay)) return -1;
    dx = (ax > p_thr[1]) ?  1 : (ax < p_thr[0] ? -1 : 0);   /* horizontal */
    dy = (ay > p_thr[3]) ?  1 : (ay < p_thr[2] ? -1 : 0);   /* vertical   */
    /* 0x7462: index = 3*VERTICAL + HORIZONTAL + 4 (the axes were swapped
       here, so every pad direction produced the wrong index). */
    return dy * 3 + dx + 4;
}

/* 0x7386: joystick buttons (bit 0x10 -> 1, bit 0x20 -> 2). */
int Plat_PadButtons(void)
{
    if (!p_pad_ok || !p_pad) return 0;
    return (SDL_GetGamepadButton(p_pad, SDL_GAMEPAD_BUTTON_SOUTH) ? 1 : 0) |
           (SDL_GetGamepadButton(p_pad, SDL_GAMEPAD_BUTTON_EAST)  ? 2 : 0);
}
