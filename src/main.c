/*
 * Dangerous Dave in the Haunted Mansion -- SDL3 port.
 *
 * Uses the original game data files from the game directory:
 *   EGATILES.DD2, LEVEL01..08.DD2, S_DAVE.DD2, S_CHUNK1/2.DD2,
 *   S_FRANK.DD2, S_MASTER.DD2 and DD2.EXE (for the sprite size table).
 *
 * The engine model follows the disassembled original: a fixed object list
 * with per-object "react" (update) functions, a tile map with derived
 * collision attributes, and EGA 4-plane 320x200 graphics.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "dd2.h"
#include "dd2_actorinfo.h"
#include "video.h"
#include "dd2_uitiles.h"
#include "input.h"
#include "platform.h"
#include "dd2_tables.h"
#include "sound.h"
#include "objects.h"
#include "level.h"
#include "dave.h"
#include "star.h"

/* ------------------------------------------------------------------ EGA */
static const uint32_t ega_pal[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500,
    0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF,
    0xFFFF55, 0xFFFFFF
};

/* EGA Attribute Controller palettes, verbatim from DGROUP
   0x2878/0x2889/0x289A/0x28AB (17 bytes each: 16 attribute -> DAC mappings
   plus the overscan).  The fade routines 0x7F6C (out: 3->0) and 0x7F13
   (in: 0->3) walk these four levels with a 6-tick delay per step, and the
   level map screen (0x139F) fades in with them -- that is why the reference
   map frames look shaded.  DAC 0..7 / 0x18..0x1F map onto the 16 EGA RGB
   entries. */
static const uint8_t atc_pal[4][16] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07 },
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F },
    { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F },
};
static uint32_t cur_pal[16];

static uint32_t dac_rgb(int d)
{
    if (d >= 0 && d < 8) return ega_pal[d];
    if (d >= 0x18 && d < 0x20) return ega_pal[8 + (d - 0x18)];
    return ega_pal[0];
}

static void apply_fade(int level)
{
    int a;
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    for (a = 0; a < 16; a++) cur_pal[a] = dac_rgb(atc_pal[level][a]);
}

static dd2_assets A;
static const char *g_datadir = ".";
static int level = 1;
static Uint64 tick;
static uint8_t     fb[DD2_W * DD2_H];          /* indexed framebuffer */
static uint32_t    rgba[DD2_W * DD2_H];

/* Convert the indexed framebuffer to RGBA through the EGA palette.  Every
   --shot/--record path must call this before write_bmp(); the splash and the
   map screen used to write the stale (all-black) `rgba`, so their recorded
   frames were black and the attract regression could not see anything. */
static void fb_to_rgba(void)
{
    int i;
    for (i = 0; i < DD2_W * DD2_H; i++) rgba[i] = cur_pal[fb[i] & 15];
}

/* --------------------------------------------------------------- video */

static void draw_sprite(int idx, int x, int y, int flip);

static int mask_invert = -1;      /* -1 = auto, 0/1 = forced (debug) */

/* The flash variant of the sprite draw.  0x2B0C queues the sprite; when the
   object's hit timer (+0x46, reached as the scratch object's [0x8C1A]) is
   non-zero, the react uses 0x2C2D instead, which ORs 0x8000 into the queued
   record pointer (0x2CF9).  The queue consumer tests it at 0x2812 and calls
   0x61B7, whose inner loop is `lodsb / not al / stosb` -- the sprite bytes are
   written INVERTED over the whole w*h rectangle (no mask). */
static void draw_sprite_flash(int idx, int x, int y)
{
    const dd2_sprite *s;
    int i, j;
    if (idx < 0 || idx >= A.nsprites) return;
    s = &A.sprites[idx];
    if (!s->px) return;
    for (j = 0; j < s->h; j++) {
        int fy = y + j;
        if (fy < 0 || fy >= DD2_H) continue;
        for (i = 0; i < s->w; i++) {
            int fx = x + i;
            if (fx < 0 || fx >= DD2_W) continue;
            /* Only the opaque pixels: inverting the whole w*h box also painted
               the transparent border and produced a solid "garbage" square. */
            if (s->mask && !s->mask[j * s->w + i]) continue;
            /* 0x61B7: the flash blitter sets GC3 = 0x10 (function select =
               OR) and SEQ2 = 0x0F (all four planes), then writes `not al` of
               the sprite's MASK plane.  The EGA mask is 0 = opaque / 1 =
               transparent, so for the monster's pixels `not 0` = 0xFF in every
               plane -> colour 15, i.e. the whole silhouette turns WHITE; the
               transparent border keeps the background.  (The earlier port
               inverted the colour instead, which is why the flash did not look
               like the original.) */
            fb[fy * DD2_W + fx] = 15;
        }
    }
}

static void draw_sprite(int idx, int x, int y, int flip)
{
    const dd2_sprite *s;
    int i, j;
    if (mask_invert < 0) {
        const char *e = getenv("DD2_INVERT_MASK");
        mask_invert = e ? atoi(e) : 0;
    }
    if (idx < 0 || idx >= A.nsprites) return;
    s = &A.sprites[idx];
    if (!s->px) return;
    for (j = 0; j < s->h; j++) {
        int fy = y + j;
        if (fy < 0 || fy >= DD2_H) continue;
        for (i = 0; i < s->w; i++) {
            int sx = flip ? s->w - 1 - i : i;
            int fx = x + i, on;
            if (fx < 0 || fx >= DD2_W) continue;
            on = s->mask[j * s->w + sx] != 0;
            if (mask_invert) on = !on;
            if (on) fb[fy * DD2_W + fx] = s->px[j * s->w + sx];
        }
    }
}

/* --- where to look for the game data -----------------------------------
   The port is built with the tables (sprites, tileattr, actorinfo) compiled
   in, but the *.DD2 files stay external.  Previously the directory was taken
   exactly from the argument (default "."), so launching dd2.exe by
   double-clicking from bin\Release failed with "cannot load DD2 data" and
   the window simply never appeared.  Now the directory is searched: argument -> exe directory -> <exe>\data -> repository next to it. */
static int has_game_data(const char *dir)
{
    char p[1400];
    FILE *f;
    if (!dir || !*dir) return 0;
    snprintf(p, sizeof p, "%s/EGATILES.DD2", dir);
    f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static const char *resolve_datadir(const char *want)
{
    static char buf[1400];
    static const char *sub[] = { "", "/data", "/../re/ref/orig",
                                 "/../../re/ref/orig", "/../../../re/ref/orig" };
    const char *base = Plat_BasePath();
    int i;

    if (has_game_data(want)) return want;
    if (has_game_data(".")) return ".";
    if (base) {
        size_t n = strlen(base);
        for (i = 0; i < (int)(sizeof sub / sizeof sub[0]); i++) {
            snprintf(buf, sizeof buf, "%.*s%s", (int)(n > 0 && (base[n-1] == '/' ||
                     base[n-1] == '\\') ? n - 1 : n), base, sub[i]);
            if (has_game_data(buf)) return buf;
        }
    }
    return want;
}

/* -------------------------------------------------------------------------
 * 0x1227 -- the level map screen.  Original:
 *   loads PROGPIC.DD2 (0x7862(DS:0x53F, 0x9326)), copies its 4 planes
 *   into video memory (0x9076(i) -- this is EGA setup: map mask = 1<<i,
 *   read plane = i), then the panels: "LEVEL n" (table DS:0x552, stride 9) and
 *   "LOADING" (DS:0x593) via the centred output 0x8851.
 *   Then 0x75D7 -- waiting for a key.
 *
 * In the port the picture is shown by Video_ShowPicAt.  NOT ported: the grid
 * of level icons (0x7D6B, loop 0x1350) and the frames (0x7FC9).
 * ---------------------------------------------------------------------- */
#define BLANK_FRAMES 2             /* black frames between screens (boot_04/05) */
static int blank_hold = BLANK_FRAMES;  /* screen blanking on the transition */
/* 0x6720/0x6855-0x6B0E: the INTRO.DD2 logo flying in.  star_di is the
   animation step (0 = not started, 1..300 = running, 301 = done/hold). */
static int star_di, star_hold;
static int star_tone;                  /* divisor of the frame's 0xC7FA tone */
static int map_hold;               /* how many frames to hold the map screen */
static int map_drawn;              /* the map is already drawn in this pass */





/* Show the current frame immediately.  Needed by screens that WAIT for a key
   (status, configs): they draw into video memory, and the main loop is
   blocked meanwhile, so without an explicit present the screen stays invisible. */
static void present_now(void)
{
    Video_ToIndexed(fb, DD2_W, DD2_H);      /* game layer: EGA planes -> indices */
    Plat_Present(fb, DD2_W, DD2_H, cur_pal);
}

/* 0x7F13 (fade in, si = 0..3) and 0x7F6C (fade out, si = 3..0): the level
   [0x97A8] is written into all four sub-palettes (0x2888 + si*0x11) and the
   block is set with int 0x10 AX=0x1002, then 0x8EC3(6) waits six ticks.  So a
   fade is four palette steps six ticks apart -- the port steps the DAC table
   (apply_fade) and shows each step. */
static void fade_steps(int from, int to)
{
    int step = (from <= to) ? 1 : -1, l, i;
    for (l = from; ; l += step) {
        apply_fade(l);
        for (i = 0; i < 6; i++) Video_TickDelay();
        Plat_Present(fb, DD2_W, DD2_H, cur_pal);
        if (l == to) break;
    }
}

/* 0x055B -- STATUS SCREEN (TAB).  Original: a 14x14 frame (0x81AE), cursor
   down 4, then centred lines 'Score' / 'High Score' / 'Daves' with the
   numbers (sprintf + 0x8851) and a key wait (0x75F1).  The high score in the
   original is taken via the far pointer [0xA6E0]. */
static unsigned int high_score_lo, high_score_hi;

static void Screen_Status(void)
{
    Video_FromIndexed(fb, DD2_W, DD2_H);   /* keep the level behind the panel */
    char buf[24];
    int n, i, sx;
    const dd2_sprite *sp = &A.sprites[1];
    Video_CenterPanel(&A, 0x0E, 0x0E);            /* 0x81AE(0x0E, 0x0E) */
    dd2_text_y += 4;                              /* 0x0575 */
    Video_PrintStrC("Score\n");                   /* 0x057A, string 0x2FD */
    snprintf(buf, sizeof buf, "%u", dd2_score_lo + (dd2_score_hi * 65536u));
    Video_PrintStrC(buf);                         /* 0x059D */
    Video_PrintStr("\n\n");                        /* 0x05A2, string 0x304 */
    Video_PrintStrC("High Score\n");              /* 0x05AB, string 0x307 */
    snprintf(buf, sizeof buf, "%u", high_score_lo + (high_score_hi * 65536u));
    Video_PrintStrC(buf);
    Video_PrintStr("\n\n");                        /* 0x05D3, string 0x313 */
    Video_PrintStrC("Daves\n");                   /* 0x05DC, string 0x316 */
    /* 0x05E5..0x0621: min(lives,8) icons of sprite 1, x = 0xA4 - 8*n + 0x10*i,
       y = [0x422A].  The original draws them instead of a number. */
    n = dd2_lives > 8 ? 8 : dd2_lives;
    sx = 0xA4 - 8 * n;
    Video_ToIndexed(fb, DD2_W, DD2_H);
    /* 0x0606: `mov di,1` .. 0x061E `cmp di,[bp-2]` / 0x0621 `jl` -- di runs
       1..n-1, so the original draws n-1 icons (si advances from 0xA4-8n). */
    for (i = 1; i < n; i++)
        draw_sprite(1, sx + 0x10 * (i - 1) + (sp->ox >> 4), dd2_text_y + (sp->oy >> 4), 0);
    Plat_Present(fb, DD2_W, DD2_H, cur_pal);
    Input_WaitKey();                              /* 0x0623: call 0x75F1 */
}

/* 0x6F5C -- KEY CONFIG SCREEN (F3) and 0x6BB2 -- 'press a button:'
   (F4, joystick setup).  The original draws a frame and iterates over the
   actions, waiting for a press; the port shows its current bindings. */
/* 0x6F5C -- F3: Keyboard Configuration.  The frame is 0x81AE(0x16, 0x0E); the
   original prints the title and the list, shows the current bindings at
   DS:0x979C+2*i (north/east/south/west) and DS:0x9744/0x9745 (button1/2),
   puts a block cursor (tile 0x1B) at the prompt, waits for a key (0x87B9 =
   0x75F1), accepts '1'..'6' as the action and the NEXT key as the new binding,
   then loops.  Any other key exits and clears [0x97D6] (= keyboard mode). */
static void Screen_KeyConfig(void)
{
    int base_y, cur_col, cur_row, i, sel;
    Video_FromIndexed(fb, DD2_W, DD2_H);   /* keep the level behind the panel */
    Video_CenterPanel(&A, 0x16, 0x0E);                  /* 0x6F64 */
    Video_PrintStrC("Keyboard Configuration");          /* 0x6F72: DS:0x2A65 */
    dd2_text_y += 0x0A;                                 /* 0x6F7B */
    Video_PrintStrC("----------------------");          /* 0x6F80: DS:0x2A7C */
    Video_PrintStr("\n1 north");                        /* 0x6F89..: DS:0x2A93 */
    Video_PrintStr("\n2 east\n3 south\n4 west\n5 button1\n6 button2");
    Video_PrintStr("\n\nModify which action: ");        /* 0x6FC2: DS:0x2ACB */
    base_y  = dd2_text_top;                             /* [0x97D0] */
    cur_col = (dd2_text_x + 7) >> 3;                    /* 0x6FC8 */
    cur_row = dd2_text_y;                               /* 0x6FD7 */

    for (;;) {
        /* 0x6FDD..0x7066: rewrite the current bindings */
        for (i = 0; i < 4; i++) {
            dd2_text_x  = dd2_text_left + 0x60;
            dd2_text_y  = base_y + (i + 2) * 10;
            Video_PrintStr(":");
            Video_PrintStr(Input_KeyName(Input_ActionKey(i * 2)));
        }
        dd2_text_x = dd2_text_left + 0x60; dd2_text_y = base_y + 0x3C;
        Video_PrintStr(":"); Video_PrintStr(Input_KeyName(Input_BtnKey(0)));
        dd2_text_x = dd2_text_left + 0x60; dd2_text_y = base_y + 0x46;
        Video_PrintStr(":"); Video_PrintStr(Input_KeyName(Input_BtnKey(1)));
        Video_Tile8(&A, 0x1B, cur_col * 8, cur_row);    /* 0x707A: the cursor */
        present_now();

        i = Input_WaitKey();                            /* 0x708A: call 0x87B9 */
        sel = (i > 0) ? Plat_ScancodeChar(i) : 0;
        if (sel < '1' || sel > '6') {                   /* 0x7096..0x70A5 */
            if (sel >= '0' && sel <= '9') continue;     /* 0x718A: re-prompt */
            break;                                      /* 0x7199: [0x97D6] = 0 */
        }
        sel -= '1';
        Video_PrintChar(sel + '1');                     /* 0x70B8: echo the digit */
        Video_PrintStr("\nPress the new key: ");        /* 0x70BD: DS:0x2AE8 */
        present_now();
        i = Input_WaitKey();                            /* 0x70C6..0x7110 */
        if (i <= 0) break;
        if (sel < 4) Input_SetActionKey(sel * 2, i);    /* 0x7110: DS:0x979C+2*si */
        else         Input_SetBtnKey(sel - 4, i);       /* 0x7120/0x712B */
        if (getenv("DD2_TRACE_MENU"))
            LOGF("keycfg: action %d -> %s (%#04x)", sel,
                 Input_KeyName(i), i);
    }
    dd2_input_mode = 0;                                         /* 0x7199 */
}

/* 0x6BB2 -- F4: Joystick Configuration (the menu calls it with di = 1, which
   selects device mode 2).  Frame 0x81AE(0x18, 0x10); a spinner (tiles
   0x0F..0x16, 0x8EC3(3) apart) runs while waiting for a button; the raw axes
   of the UPPER LEFT and LOWER RIGHT corners are captured and turned into the
   four thresholds (0x6D5E-0x6DAD):
       step = (max - min) / 6 ; low = min + step ; high = max - step
   then the device mode becomes 2 (0x6DB6) and the "button 1 (A)ttack, or
   (J)ump ?" question (0x6DC4-0x6DE9) sets [0xA6DE]. */
static void Screen_JoyConfig(void)
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, sx, sy, tile, k, i;
    uint32_t t0 = Plat_Ticks();

    Video_FromIndexed(fb, DD2_W, DD2_H);   /* keep the level behind the panel */
    Video_CenterPanel(&A, 0x18, 0x10);                      /* 0x6BBD */
    Video_PrintStrC("Joystick Configuration\n");            /* 0x6BCB: DS:0x2906 */
    Video_PrintStrC("----------------------\n");            /* 0x6BD4: DS:0x291E */
    sx = dd2_text_left + 0x60;
    sy = dd2_text_y;
    /* The original draws the frame and the prompt (0x6BBB-0x6BF7) BEFORE it
       spins for a button, so the screen must be visible even without a pad.
       The port used to return before drawing anything, which is why F4 showed
       nothing.  Without a pad the original waits forever (its only escape is
       the [0x2B83] no-intro flag); here a key dismisses the prompt instead. */
    if (!Plat_PadRaw(NULL, NULL)) {
        if (getenv("DD2_TRACE_MENU")) LOGF("joycfg: no joystick -- prompt only");
        dd2_text_x = dd2_text_left; dd2_text_y = sy;
        Video_PrintStr("Hold the joystick in the\nUPPER LEFT corner and\npress a button:");
        present_now();
        Input_WaitKey();
        return;
    }

    for (k = 0; k < 2; k++) {
        /* 0x6BE6/0x6BEF: the two prompts (plain print).  The second one is
           DS:0x2977 = "\n\nHold the joystick in the\n" and is printed after a
           0x8EC3(8) wait (0x6CA2); the text cursor is NOT reset in between,
           otherwise the second prompt overwrites the first. */
        if (k == 0) {
            dd2_text_x = dd2_text_left; dd2_text_y = sy;
        } else {
            for (i = 0; i < 8; i++) Video_TickDelay();      /* 0x6CA2: 0x8EC3(8) */
        }
        Video_PrintStr(k == 0
            ? "Hold the joystick in the\nUPPER LEFT corner and\npress a button:"
            : "\n\nHold the joystick in the\nLOWER RIGHT corner and\npress a button:");
        present_now();
        sy = dd2_text_y;     /* 0x915A uses the current [0x422A] for the spinner */
        /* 0x6BF8-0x6C39: spinner + wait for a button */
        tile = 0x0F;
        for (;;) {
            Video_Tile8(&A, tile, (sx >> 3) * 8, sy);       /* 0x6C0A: 0x915A */
            present_now();
            for (i = 0; i < 3; i++) Video_TickDelay();      /* 0x6C19: 0x8EC3(3) */
            Plat_PumpEvents();
            if (++tile > 0x16) tile = 0x0F;                 /* 0x6C22 */
            if (Plat_PadButtons() & 3) break;               /* 0x72E8 -> button */
            if (g_fast && Plat_Ticks() - t0 > 250) return;
        }
        if (k == 0) Plat_PadRaw(&x0, &y0);
        else        Plat_PadRaw(&x1, &y1);
    }
    /* 0x6D5E-0x6DAD: the thresholds */
    {
        int stepx = (x1 - x0) / 6, stepy = (y1 - y0) / 6;
        Plat_PadSetThresholds(x0 + stepx, x1 - stepx, y0 + stepy, y1 - stepy);
    }
    dd2_input_mode = 2;                                     /* 0x6DB6 */
    if (getenv("DD2_TRACE_MENU"))
        LOGF("joycfg: x %d..%d y %d..%d -> thr %d/%d %d/%d mode=%d", x0, x1, y0, y1,
             x0 + (x1 - x0) / 6, x1 - (x1 - x0) / 6,
             y0 + (y1 - y0) / 6, y1 - (y1 - y0) / 6, dd2_input_mode);
    Video_PrintStr("\n\nShould button 1 (A)ttack,\nor (J)ump ?");   /* 0x6DC4 */
    present_now();
    k = Input_WaitKey();                                    /* 0x6DCD: 0x87B9 */
    k = (k > 0) ? Plat_ScancodeChar(k) : 0;
    dd2_button_swap = (k == 'A' || k == 'a') ? 1 : 0;       /* 0x6DD3-0x6DE9 */
}

/* 0x239 -- F1 "Dangerous Dave Commands" screen: panel 0x812F(0x16,0x1E),
   a centred title and two centred separators with the left-aligned command
   list, then 0x87B9 (wait for a key). */
static void Screen_Info(void)
{
    Video_FromIndexed(fb, DD2_W, DD2_H);   /* keep the level behind the panel */
    Video_CenterPanel(&A, 0x1E, 0x16);            /* 0x0239: 0x812F(w=0x1E, h=0x16) */
    Video_PrintStrC("Dangerous Dave Commands\n");          /* DS:0x132 */
    Video_PrintStrC("-----------------------\n\n");        /* DS:0x14B */
    Video_PrintStr("F2  : Sound on / off\n");              /* DS:0x165 */
    Video_PrintStr("F3  : Keyboard mode / custom\n");      /* DS:0x17B */
    Video_PrintStr("F4  : Joystick mode\n");               /* DS:0x199 */
    Video_PrintStr("F5  : Reset game\n");                  /* DS:0x1AE */
    Video_PrintStr("ESC : Quit\n");                        /* DS:0x1C0 */
    Video_PrintStr("TAB : Status window\n\n");             /* DS:0x1CC */
    Video_PrintStr("CTRL: Button 1 (jump)\n");             /* DS:0x1E2 */
    Video_PrintStr("  DOWN + CTRL : Drop down\n");         /* DS:0x1F9 */
    Video_PrintStr("ALT : Button 2 (fire)\n");             /* DS:0x214 */
    Video_PrintStr("UP / DOWN : Aim gun\n");               /* DS:0x22B */
    Video_PrintStr("LEFT / RIGHT : move\n");               /* DS:0x240 */
    present_now();
    Input_WaitKey();                              /* 0x87B9 */
}

/* 0x1E90/0x8C2C dispatch completeness counters (used by `make sweep`). */
int dd2_unported_fn, dd2_unported_react;
static int map_lives;                 /* icons to draw on the map screen */
static int last_loaded_level = -1;    /* [0x4A98] */
static int map_loading = 1;           /* [0x97AE] != [0x4A98] -> show LOADING */
static int game_to_title;             /* game over + "N" -> back to the title */
static int game_reset_pending;        /* [0x8C34] = 1 -> restart the game (F5) */

/* 0x1227 -- level map screen, faithful layout:
     PROGPIC.DD2 full screen; panel 0x7FC9(0,0,10,3); the level title string
     (DS:0x54B + 9*(level-1), "LEVEL n") centred in the panel; life icons
     (sprite 1) at y=0xA5, x=0x2C+0x10*i; the score at (0xF4,0xA6) and the high
     score at (0xF4,0xB9); and, only for a level that is not the current one,
     the LOADING panel 0x7FC9(0,5,10,8) with "LOADING" (DS:0x593). */
static int map_screen(int levelno)
{
    char buf[24];
    Video_ShowPicAt("PROGPIC.DD2", 0);                  /* 0x123E */
    Video_SetScroll(0);                                 /* 0x12BD: 0x90A8(0,0) */
    dd2_base = 0;                                       /* [0x2FFE] = 0 */
    dd2_text_left = 0; dd2_text_right = 0x140;          /* [0x9798]/[0x979A] */
    dd2_text_y = 5;                                     /* 0x12D5 */
    Video_Panel(&A, 0, 0, 10, 3);                       /* 0x12E7 */
    dd2_text_y += 3;                                    /* 0x12ED -> 11 */
    if (levelno >= 1 && levelno <= 8) {
        /* DS:0x54B + 9*(level-1): "LEVEL n" (the literal, because the
           generated string table only holds the menu strings). */
        snprintf(buf, sizeof buf, "LEVEL %d\n", levelno);
        Video_PrintStrC(buf);                           /* 0x132A */
    }
    map_lives = dd2_lives > 8 ? 8 : dd2_lives;          /* 0x133A..0x1346 */
    dd2_text_x = 0xF4; dd2_text_y = 0xA6;               /* 0x136B */
    snprintf(buf, sizeof buf, "%u", dd2_score_lo + (dd2_score_hi * 65536u));
    Video_PrintStr(buf);                                /* 0x8911 */
    dd2_text_x = 0xF4; dd2_text_y = 0xB9;
    snprintf(buf, sizeof buf, "%u", high_score_lo + (high_score_hi * 65536u));
    Video_PrintStr(buf);
    if (map_loading) {                                  /* 0x145E */
        dd2_text_left = 0; dd2_text_right = 0x140;
        dd2_text_y = 5;
        Video_Panel(&A, 0, 5, 10, 8);                   /* 0x7FC9(0,5,0xA,8) */
        dd2_text_y += 3;                                /* 0x147C -> 51 */
        Video_PrintStrC("LOADING\n");                   /* DS:0x593 */
    }
    return map_lives;
}

/* 0x394F for Dave: his door record (0x870) -- is fn 0x394F, but the level
   data lives here, so dave.c calls through the pointer. */
static void dave_door(void)
{
    if (dd2_door_desc)
        Level_Door(A.level.tiles, A.level.w, A.level.h, A.intile, dd2_door_desc);
}

/* 0x3A5B for Dave (record 0x834): the door opening animation + tile stamp. */
static void dave_door_open(void)
{
    if (getenv("DD2_TRACE_DOOR"))
        LOGF("door open: desc=%#06x col=%d row=%d tile=%#06x", dd2_door_desc,
             dd2_scan_col, dd2_scan_row, dd2_door_tile);
    if (dd2_door_desc)
        Level_DoorOpen(A.level.tiles, A.level.w, A.level.h, dd2_door_desc,
                       dd2_scan_col, dd2_scan_row, dd2_door_tile, NULL);
}

/* Extra sprites that an object draws itself (not one per object):
   for example the STARTDAVE rope 0x4E73/0x4F0A -- a series of sprites 0x9A plus 0x99.
   The original calls 0x2B0C from the handlers, so there is the same queue here. */
#define DD2_MAXEXTRA 96
static struct { int spr, wx, wy, slot; } extra_spr[DD2_MAXEXTRA];
static int nextra;

static void extra_sprite(int spr, int wx, int wy)
{
    if (nextra >= DD2_MAXEXTRA) return;
    extra_spr[nextra].spr  = spr;
    extra_spr[nextra].wx   = wx;
    extra_spr[nextra].wy   = wy;
    extra_spr[nextra].slot = dd2_react_slot;   /* queued by this slot's reaction */
    nextra++;
}

/* 0x3B6F: SOUTH table by tile coordinates -- needed by the sight for
   pull-up (it fetches the tile and checks its behaviour). */
static int tile_south(int row, int col)
{
    int t;
    if (row < 0 || col < 0 || row >= A.level.h || col >= A.level.w) return 0;
    t = A.level.tiles[row * A.level.w + col];
    return (t >= 0 && t <= 857) ? A.south[t] : 0;
}

/* REACTION dispatcher (`call [0x8C2C]`, field +0x10 of the record).  Behaviours
   land here that the original calls through the reaction pointer, not as a
   per-frame function. */
static void obj_react(dd2_obj *o)
{
    if (!o) return;
    switch (o->react) {
    case 0x4338:                   /* walker: stand at a wall, fall without floor */
    case 0x4380:
        Obj_WalkerReact(o, A.level.tiles, A.north, A.east, A.west,
                        A.level.w, A.level.h, o->react == 0x4338);
        break;
    case 0x2E55:                   /* Dave: drawing + cull off screen */
        Obj_DrawCullReact(o);
        break;
    case 0x2EC9:                   /* Dave, reload: collision + timer */
        Obj_ReloadReact(o, A.level.tiles, A.north, A.south, A.east, A.west,
                        A.level.w, A.level.h);
        break;
    case 0x2E6B:                   /* MASTER/MASTCLAW: timer + cull */
        Obj_TimerCullReact(o);
        break;
    case 0x2FF6:                   /* SLIME/SPIDER: ceiling and turn-around */
        Obj_SlimeSpiderReact(o, A.level.tiles, A.south, A.north, A.east, A.west,
                             A.level.w, A.level.h);
        break;
    case 0x48B2:                   /* 0x48B2: slime crawling (records 0xE4C/0xE74) */
        Obj_SlimeCrawl(o, A.level.tiles, A.north, A.south, A.east, A.west,
                       A.level.w, A.level.h);
        break;
    case 0x36A4:                   /* CHUNK: landing and bounce off a wall */
        Obj_ChunkLand(o, A.level.tiles, A.north, A.south, A.east, A.west,
                      A.level.w, A.level.h, 0x0C6C);
        break;
    case 0x3707:                   /* SPARKCHUNK: touched -- goes out */
        Obj_SparkReact(o, A.level.tiles, A.north, A.south, A.east, A.west,
                       A.level.w, A.level.h);
        break;
    case 0x53D2:                   /* DELBERT: vertical push-out */
        Obj_DelbertReact(o, A.level.tiles, A.north, A.south, A.level.w, A.level.h);
        break;
    case 0x4E73:                   /* rope STARTDAVE (variant 1) */
        Obj_RopeReact1(o, A.level.tiles, A.south, A.east, A.west,
                       A.level.w, A.level.h);
        break;
    case 0x4F0A:                   /* rope STARTDAVE (variant 2) */
        Obj_RopeReact2(o, A.level.tiles, A.east, A.west, A.level.w, A.level.h);
        break;
    case 0x4B4B:                   /* wolf: turn at a wall + jump */
        Obj_WolfReact(o, A.level.tiles, A.north, A.east, A.west,
                      A.level.w, A.level.h);
        break;
    case 0x4C30:                   /* 0x4C30: wolf bounce off a wall/floor (0xFC8) */
        Obj_WolfBounce(o, A.level.tiles, A.north, A.south, A.east, A.west,
                       A.level.w, A.level.h);
        break;
    case 0x4DB1:                   /* skull: full collision */
        Obj_SkullReact(o, A.level.tiles, A.north, A.south, A.east, A.west,
                       A.level.w, A.level.h);
        break;
    case 0x503B:                   /* fireball: bounces and fades */
        Obj_FrankSparkReact(o, A.level.tiles, A.north, A.south, A.east, A.west,
                            A.level.w, A.level.h);
        break;
    case 0x4623:                   /* ZOMBDOWN: stand up if there is a floor */
        Obj_ZombieDownReact(o, &dd2_objs[0], A.level.tiles, A.south, A.north,
                            A.level.w, A.level.h);
        break;
    case 0x47B6:                   /* knife: push-out along X, sticks in */
        Obj_KnifeReact(o, A.level.tiles, A.east, A.west, A.level.w, A.level.h);
        break;
    case 0x43A9:                   /* Dave, jump/fall: contact physics */
        Dave_PhysReact(o, A.level.tiles, A.north, A.south, A.east, A.west,
                       A.level.w, A.level.h);
        break;
    case 0x2F11:                   /* patrol AI (22 records) */
        Obj_WalkAI(o, A.level.tiles, A.north, A.east, A.west, A.level.w, A.level.h);
        break;
    case 0x2EB0:                   /* 0x2EB0: SLIMERDYSW/NW -- full push-out + cull */
        Obj_PushOutX(o, A.level.tiles, A.east, A.west, A.level.w, A.level.h);
        Obj_PushOutY(o, A.level.tiles, A.north, A.south, A.level.w, A.level.h);
        Obj_OffscreenCheck(o);
        break;
    case 0x53BF:                   /* DELBERT win record 0x1158: draw only */
        break;
    default:
        dd2_unported_react++;
        if (getenv("DD2_TRACE_UNPORTED"))
            LOGF("unported react %#06x (info %#05x type %d)", o->react, o->info, o->type);
        break;
    }
}

/* Per-frame record function dispatcher (`call word ptr [bx+0xC]`).
   Dave (slot 0) goes its own way, the rest -- by the record's function address.
   Unported functions are silently skipped (their addresses are visible in
   COVERAGE.md as "not started"). */
static void obj_perframe(dd2_obj *o)
{
    const unsigned char *ai;
    int fn;
    if (o == &dd2_objs[0]) { Dave_React(o); return; }
    if (o->info < DD2_ACTORINFO_BASE) return;
    ai = dd2_actorinfo_blob + (o->info - DD2_ACTORINFO_BASE);
    fn = ai[0x0C] | (ai[0x0D] << 8);
    switch (fn) {
    case 0x394F:                   /* record 0x870: transfer through the door */
        if (dd2_door_desc)
            Level_Door(A.level.tiles, A.level.w, A.level.h, A.intile, dd2_door_desc);
        break;
    case 0x3504:                   /* bonus disappears after 0x3C */
        Obj_BonusTimer(o, 0x3C);
        break;
    case 0x351E:                   /* BONUS1UP disappears after 0xB4 */
        Obj_BonusTimer(o, 0xB4);
        break;
    case 0x4C2C:                   /* wolf jump: gravity */
        Obj_WolfJump(o);
        break;
    case 0x4A0F:                   /* GHOSTW: the ghost chases Dave */
        Obj_GhostChase(o, &dd2_objs[0]);
        break;
    case 0x50D8:                   /* MASTER: walking and placing turrets */
        Obj_MasterWalk(o);
        break;
    case 0x51FF:                   /* MASTER: fan of four projectiles */
        Obj_MasterAttack(o);
        break;
    case 0x52D2:                   /* MASTCLAW: homing in on Dave */
        Obj_MastClaw(o, &dd2_objs[0]);
        break;
    case 0x5377:                   /* MASTFIRE: projectile flight */
        Obj_MastFire(o);
        break;
    case 0x53F8:                   /* DELBERT4: game completed */
        Obj_DelbertWin(o);
        break;
    case 0x310D:                   /* ZATTACK: turn towards Dave */
        Obj_FaceDave(o, &dd2_objs[0]);
        break;
    case 0x366B:                   /* CHUNK: accelerate + gravity -> fall */
        Obj_ChunkFly(o, 0x0C44);
        break;
    case 0x3696:                   /* SPARKCHUNK: accelerate + gravity */
        Obj_SparkChunkFly(o);
        break;
    case 0x476B:                   /* hunchback throws a knife */
        Obj_HunchThrow(o);
        break;
    case 0x49D1:                   /* the ghost became solid */
        Obj_GhostSolid(o);
        break;
    case 0x496F:                   /* the ghost noticed Dave */
        Obj_GhostStartDave(o, &dd2_objs[0]);
        break;
    case 0x4D13:                   /* skull: homing projectile */
        Obj_SkullAI(o, &dd2_objs[0]);
        break;
    case 0x4F9A:                   /* Frankenstein: animation by timer */
        Obj_FrankAnim(o);
        break;
    case 0x4FB4:                   /* Frankenstein: fireball throw */
        Obj_FrankThrow(o);
        break;
    case 0x5006:                   /* fireball: flight */
        Obj_SparkFly(o);
        break;
    case 0x5025:                   /* fireball: lifetime timer */
        Obj_SparkTimer(o);
        break;
    case 0x4E07:                   /* spider: attack downwards / turn towards Dave */
        Obj_SpiderAI(o, &dd2_objs[0]);
        break;
    case 0x4878:                   /* slime: random turn-arounds */
        Obj_SlimeWander(o);
        break;
    case 0x4AE1:                   /* wolf-runner: lunge at Dave + jump */
        Obj_WolfAI(o, &dd2_objs[0]);
        break;
    case 0x46D4:                   /* hunchback-walker: chase/retreat/turn */
        Obj_HunchbackAI(o, &dd2_objs[0]);
        break;
    case 0x452A:                   /* zombie: chasing Dave (partially) */
        Obj_ZombieAI(o, &dd2_objs[0], A.level.tiles, A.south, A.north,
                     A.level.w, A.level.h);
        break;
    default:
        dd2_unported_fn++;
        if (getenv("DD2_TRACE_UNPORTED"))
            LOGF("unported fn %#06x (info %#05x type %d)", fn, o->info, o->type);
        break;                     /* not ported */
    }
}

/* 0x192E takes the sprite bbox from the spriteInfo table; the port builds it
   itself from the sprite mask (dd2.c), so the values are supplied from here. */
/* 0x192E (0x1962-0x19B9): left = x + rec[+8] + rec[+0x1C],
   top = y + rec[+0x0A] + rec[+0x1E], right = x + rec[+0x0C] + rec[+0x1C],
   bottom = y + rec[+0x0E] + rec[+0x1E].  The blitter adds the same origin
   (0x2B26), so the collision box and the drawn sprite agree only when the
   origin is included. */
static void port_sprite_bbox(int sprite, int *l, int *t, int *r, int *b)
{
    const unsigned char *rec = dd2_sprite_record(sprite);
    int ox, oy;
    if (!rec) { *l = *t = *r = *b = 0; return; }
    ox = (int16_t)(rec[0x1C] | (rec[0x1D] << 8));
    oy = (int16_t)(rec[0x1E] | (rec[0x1F] << 8));
    *l = (int16_t)((int16_t)(rec[0x08] | (rec[0x09] << 8)) + ox);
    *t = (int16_t)((int16_t)(rec[0x0A] | (rec[0x0B] << 8)) + oy);
    *r = (int16_t)((int16_t)(rec[0x0C] | (rec[0x0D] << 8)) + ox);
    *b = (int16_t)((int16_t)(rec[0x0E] | (rec[0x0F] << 8)) + oy);
}

/* Direction tables for the hitscan ray (0x3C4C):
   0 = NORTH [0x4A92], 1 = SOUTH [0x4A94], 2 = EAST [0x4DDC], 3 = WEST [0x4DD8]. */
static int hs_tile_solid(int tile, int dir)
{
    if (tile < 0 || tile > 857) return 0;
    switch (dir) {
    case 0:  return A.north[tile];
    case 1:  return A.south[tile];
    case 2:  return A.east[tile];
    default: return A.west[tile];
    }
}

/* ------------------------------------------------------- game level */
/* The level is assembled with the original's means: loading, camera limits,
   spawning from the tag plane (0x53FF), the player is slot 0 of the object array. */
static void game_start_level(int n);      /* defined below */

/* 0x0BEF ("Sprite Test", reached from the debug key at 0x3A0) was removed:
   it was never called, mixed the two render models (Video_* writes dd2_vram,
   draw_sprite writes fb), never presented a frame, and passed NULL to
   Input_Frame.  The production screens (status / key / joy config, map) are
   implemented below. */

/* 0x21B0 -- GAME RESTART (after death/from the map screen):
     if [0x4DCE] == 0 -> [0x97AE] = 0
     [0x8C34] = 0 ; score ([0x9320..0x9322]) = 0 ; [0x8C3A..0x8C3C] = 0
     [0x8BC8] = 4                       ; FOUR lives
     [0x97AE]++                         ; level number
     [0x4A98] = -1
     0x7F6C() ; 0x7FB2(0x40)
     if [0x2B83] == 0 -> 0x1227()       ; MAP SCREEN
     0x75D7()                           ; keyboard reset
     [0x8BCA] = 1 (one object) ; reset timers ; [0x8C38] = 0
     memset(0x520A) ; 0x0DAD() (level load) ; [0x4A98] = [0x97AE]
     0x7938() ; 0x1CB0() (camera) ; [0x9076] = 8 (ammo) ; 0x1EAB()
   So this is "reset and start over", not a separate screen. */
static void game_restart(void)
{
    dd2_score_lo = dd2_score_hi = 0;      /* [0x9320..0x9322] */
    dd2_lives = 4;                        /* [0x8BC8] = 4 */
    score_extra_reset();                  /* [0x8C3A..0x8C3C] = 0 */
    dd2_door_flag = 0;                    /* [0x8C38] = 0 */
    last_loaded_level = -1;               /* 0x21ED: [0x4A98] = 0xFFFF --
                                             the map screen always shows LOADING */
    level = 1;
    game_start_level(level);              /* map + level load */
}

/* 0x21C5: the same reset, but starting at the level the player chose in the
   "Continue from level N ?" prompt (0x2566: `sub [0x97AE],2` then the `inc`
   at 0x21E9 leaves [0x97AE]-1). */
static void game_restart_at(int n)
{
    dd2_score_lo = dd2_score_hi = 0;
    dd2_lives = 4;
    score_extra_reset();
    dd2_door_flag = 0;                    /* [0x8C38] = 0 */
    last_loaded_level = -1;               /* 0x21ED: [0x4A98] = 0xFFFF */
    level = n;
    game_start_level(level);
}

/* 0x87B9 + 0xBDAF: wait for a key and report whether it was 'Y'
   (0x2561: `cmp ax, 0x59`).  Key identification is platform glue; the port's
   scancode for Y is 0x15. */
static int wait_yes(void)
{
    int sc = Input_WaitKey();                     /* 0x87B9 */
    int ch = Plat_ScancodeChar(sc);               /* 0xBDAF */
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    return (ch == 'Y') || (sc == 0x15);
}

/* 0x23B9..0x2453: "New High Score!" panel.  Shown when the final score beats
   the stored high score; the caller then stores it (0x245D). */
static void Screen_HighScore(unsigned score, unsigned old)
{
    Video_FromIndexed(fb, DD2_W, DD2_H);   /* keep the level behind the panel */
    char buf[24];
    Video_CenterPanel(&A, 0x12, 0x0B);            /* 0x81AE(0x0B, 0x12) */
    dd2_text_y += 3;                              /* 0x23C7 */
    Video_PrintStrC("New High Score!\n");         /* DS:0x6F7 */
    dd2_text_y += 5;                              /* 0x23D5 */
    Video_PrintStrC("Score\n");                    /* DS:0x708 */
    snprintf(buf, sizeof buf, "%u", score);
    Video_PrintStrC(buf);                         /* 0xC4AB + 0x8851 */
    Video_PrintStr("\n\n");                        /* DS:0x70F, 0x876C */
    Video_PrintStrC("Old\n");                     /* DS:0x712 */
    snprintf(buf, sizeof buf, "%u", old);
    Video_PrintStrC(buf);
    Video_PrintStr("\n");                          /* DS:0x717 */
    dd2_text_y += 5;                              /* 0x243C */
    Video_PrintStrC("Congratulations!\n");        /* DS:0x719 */
    Video_PrintStrC("");                          /* DS:0x72B */
    present_now();
    Input_WaitKey();                              /* 0x75F1 */
}

/* 0x24EF..0x2564: "Continue from level N ?" panel (0x7FC9(7,0x14,0x20,0x17)).
   Returns 1 when the player answered 'Y'. */
static int Screen_Continue(int n)
{
    int yes;
    Video_FromIndexed(fb, DD2_W, DD2_H);   /* keep the level behind the panel */
    char buf[48];
    Video_Panel(&A, 7, 0x14, 0x20, 0x17);         /* 0x7FC9 */
    dd2_text_y += 3;                              /* 0x2505 */
    snprintf(buf, sizeof buf, "Continue from level %d ?", n);
    Video_PrintStrC(buf);                         /* 0x254B */
    present_now();
    /* 0x75D7 (key reset) then 0x87B9 (wait) */
    yes = wait_yes();
    if (getenv("DD2_TRACE_MENU"))
        LOGF("continue prompt (level %d): %s", n, yes ? "YES" : "no");
    return yes;
}

/* Sound debug: log every effect request, then call Sound_SFX. */

/* Debug helper: log every effect request, then hand it to the engine. */
/* The original advances the PC-speaker sequence from the 140 Hz timer ISR,
   independently of the main loop.  Driving it with a fixed two ticks per
   frame stalled the sound whenever the game blocked (death animation, fades),
   which made e.g. the death sound appear only after the level restarted.
   This pumps the sequence from the wall clock instead; Plat_Delay calls it
   through the idle hook, so it also runs inside animations. */
static void sound_pump(void)
{
    static uint32_t last;
    uint32_t now = Plat_Ticks();
    int ticks;
    if (!last) { last = now; return; }
    if (now < last) { last = now; return; }
    ticks = (int)((now - last) * 140 / 1000);
    if (ticks <= 0) return;
    last += (uint32_t)ticks * 1000 / 140;
    while (ticks-- > 0) Sound_Tick();
}

static void sfx_logged(int id)
{
    if (getenv("DD2_TRACE_SFX")) LOGF("sfx %d", id);
    Sound_SFX(id);
}

/* 0x21F3 (per-life entry of the level loop 0x21B0) + the level-start block
   0x2229..0x2268: reset [0x8C38], load the level (0x0DAD), spawn the tags
   (0x53FF), set camera limits (0x0ED0) and the camera (0x1CB0), show the map
   screen (0x1227) and start the level with full ammo ([0x9076] = 8). */
static void game_start_level(int n)
{
    if (!dd2_load_level(&A, g_datadir, n)) return;
    level = n;
    /* 0x2208/0x2243: the map screen shows LOADING only when the level being
       started is not the one already loaded (a death restarts the same level). */
    map_loading = (last_loaded_level != n);
    last_loaded_level = n;
    dd2_door_flag = 0;                 /* 0x2229: [0x8C38] = 0 */
    Obj_ClearAll();
    Dave_Init();                       /* 0x8BB2..: cmd, latches, ammo, dir */
    /* 0x2248 (level start block): [0x9076] = 8 -- the level starts with
       FULL ammo (0x2262: mov word ptr [0x9076], 8).  The same place calls
       0x1CB0 (camera setup) and once 0x1EAB. */
    dave_ammo = 8;
    dd2_sprite_bbox = port_sprite_bbox;   /* 0x192E -> sprite table */
    Level_SpawnInit();
    dd2_spawn_tiles = A.level.tiles;   /* needed by conditional tag 3 (0x47E6) */
    dd2_spawn_north = A.north;
    Level_SpawnTags(A.level.objs, A.level.w, A.level.h);
    Level_SetCameraLimits(A.level.w, A.level.h);
    /* 0x2208: level map screen before the start.  In the original the screen
       stays up for the whole level load (a floppy read of LEVELnn.DD2 plus the
       0x7938 pause of 0x64 ticks = 0.71 s) and there is NO key wait: 0x1227
       ends with a plain ret, 0x220B only RESETS the key table.  The port loads
       instantly, so the load is approximated by holding the map.  It is NOT drawn here:
       after game_start_level main calls Video_AttractBegin(), which
       reloads STARPIC and would overwrite the map. */
    map_hold = 180;                     /* ~2.6 s: load + 0x7938 (0x64 ticks) */
    /* Debug: place Dave at a given point (to test enemy AI,
       which is only updated near the camera). */
    if (getenv("DD2_DAVE_AT")) {
        int x, y;
        if (sscanf(getenv("DD2_DAVE_AT"), "%d,%d", &x, &y) == 2) {
            dd2_objs[0].x = (int16_t)x; dd2_objs[0].y = (int16_t)y;
            Obj_Box(&dd2_objs[0]);
            dave_x = (int16_t)x;
        }
    }
    map_drawn = 0;
    Level_InitCamera();                /* 0x1CB0 */
    dd2_sprite_bbox = port_sprite_bbox;   /* 0x192E -> sprite table */
    dd2_obj_perframe = obj_perframe;   /* call [x+0xC]: per-frame function dispatcher */
    dd2_sfx          = sfx_logged;     /* 0x8CB2: sound (+ tracing) */
    dd2_extra_sprite = extra_sprite;   /* 0x2B0C for the rope and similar */
    dd2_tile_south   = tile_south;     /* 0x3B6F: SOUTH table for pull-up */
    dd2_door_fn      = dave_door;      /* 0x394F: door for Dave's record 0x870 */
    dd2_door_open_fn = dave_door_open; /* 0x3A5B: door for Dave's record 0x834 */
    dd2_frame_react  = obj_react;      /* call [0x8C2C]: reaction dispatcher */
    dd2_pair_react   = Obj_PairReact;  /* call [obj+0x56]: object-vs-object (0x41E7) */
    /* 0x3C4C: the hitscan march needs the tile plane and its direction tables. */
    dd2_hs_tiles = A.level.tiles;
    dd2_hs_w = A.level.w;
    dd2_hs_h = A.level.h;
    dd2_hs_solid = hs_tile_solid;
    dave_info = dd2_objs[0].info;
    /* 0x226E: right after loading the level (and after the 0x2240 load, the
       0x2249 0x7938 wait and the 0x2259 camera setup) the original runs ONE
       object frame -- 0x2268 sets [0x5206] = 1, 0x226E calls 0x1EAB, 0x2271
       clears it.  Without it the first rendered frame of every level was one
       object update behind the original. */
    Obj_DeltaReset();                  /* 0x2214-0x2226: delta/tick counters */
    Obj_Frame();
}

/* -------------------------------------------------------------------------
 * Post-frame block of the level loop (0x226E..0x239B) plus the tail of
 * 0x1EAB (0x2137..0x21AF).  [0x8C38] is the single "something happened"
 * flag:  < 0  Dave was hit (the value selects the death animation)
 *        ==0  keep playing       ==0x64  boss defeated, wait for cleanup
 *        ==1  level completed
 * Returns 1 when the level/game was restarted or advanced (the caller then
 * continues with the new state), 0 while the current level keeps running.
 * ---------------------------------------------------------------------- */
static void show_center_text(const char *s, int w, int h)
{
    int i;
    Video_FromIndexed(fb, DD2_W, DD2_H);   /* keep the level behind the panel */
    Video_CenterPanel(&A, w, h);       /* 0x81AE */
    Video_PrintStrC(s);                /* 0x8851 */
    for (i = 0; i < 90; i++) {         /* hold for ~1.5 s */
        present_now();
        Plat_PumpEvents();
        Plat_Delay(16);
    }
}

/* 0x91F8: blit one death sprite.  The source is the UI tile bank (the same
   four plane segments 0x915A uses), the plane offset is [0x320C] + rec[+4]
   with [0x320C] = module[0x2A] = 0x100; the blit is 4 planes x rec[+0] bytes
   per row x rec[+2] rows, opaque (GC5 = 0, SEQ2 = 1/2/4/8). */
static void death_blit(int spr, int px, int py)
{
    int y, x, p, off;
    if (spr < 0 || spr >= DD2_NDEATH) return;
    off = 0x100 + dd2_death[spr].off;
    for (y = 0; y < (int)dd2_death[spr].h; y++) {
        int fy = py + y;
        if (fy < 0 || fy >= DD2_H) continue;
        for (x = 0; x < (int)dd2_death[spr].w * 8; x++) {
            int fx = px + x, sh = 7 - (x & 7), c = 0;
            int o = off + y * dd2_death[spr].w + (x >> 3);
            if (fx < 0 || fx >= DD2_W) continue;
            if (o < 0 || o >= (int)sizeof dd2_uitiles[0]) continue;
            for (p = 0; p < 4; p++)
                if ((dd2_uitiles[p][o] >> sh) & 1) c |= 1 << p;
            fb[fy * DD2_W + fx] = (unsigned char)c;
        }
    }
}

/* 0x10D0 -- the ending.  It starts with `0x91F8(7, 8, 0x28)`: arg3 = 0x28 is
   sprite 40 of the DEATH bank ("THEEND", 26 bytes x 56 rows), drawn at byte
   offset 7 (pixel x 56) and row 8, followed by 0x8EC3(0xB4) ticks.  Then five
   pages, each in a 0x81AE(0x1C, 5) frame with centred 0x8851 lines and a
   key wait (0x75D7 + 0x75F1); pages 3, 4 and 5 shift the cursor by 0x1169 (+9),
   0x1194 (+4) and 0x11C8 (+0x0E) because they have fewer lines. */
static void Screen_Ending(void)
{
    static const char *const page[5][4] = {
        { "Congratulations!\n", "You have freed Delbert from\n",
          "the wicked grasp of\n", "Silas' Master!\n" },                 /* 0x400.. */
        { "Delbert mentions that\n", "he  was thinking of going\n",
          "with his friends to \n", "Booger Hollow, where\n" },          /* 0x454.. */
        { "old Man Clinton's ghost is\n", "rumored to haunt his shack...", NULL, NULL },
        { "You thwack Delbert solidly\n", "on the head and hope he\n",
          "comes to his senses.", NULL },                                 /* 0x4EC.. */
        { "You win!", NULL, NULL, NULL },                                  /* 0x536 */
    };
    static const int dy[5] = { 0, 0, 9, 4, 0x0E };
    int p, i;

    Video_FromIndexed(fb, DD2_W, DD2_H);   /* keep the level behind the panel */
    death_blit(0x28, 7 * 8, 8);                 /* 0x10DC: 0x91F8(7, 8, 0x28) */
    present_now();
    for (i = 0; i < 0xB4; i++) Video_TickDelay();   /* 0x10E2: 0x8EC3(0xB4) */
    for (p = 0; p < 5; p++) {
        Video_CenterPanel(&A, 0x1C, 5);         /* 0x10EB/0x1123/0x115B/... */
        dd2_text_y += dy[p];                    /* 0x1169/0x1194/0x11C8 */
        for (i = 0; i < 4 && page[p][i]; i++)
            Video_PrintStrC(page[p][i]);        /* 0x10F9..0x1114 etc. */
        present_now();
        Input_WaitKey();                        /* 0x111D: 0x75D7 + 0x75F1 */
    }
}

/* 0x2295..0x2336: Dave's death animation.  The jump table at CS:0x2575 maps
   the death code (-8..-1) to the first frame of a 48x48 animation:
     -8 -> 0x23 SLICE1 (4 frames)   -7 -> 0x1E SKULL1   -6 -> 0x14 WOLF1
     -5 -> 0x19 GHOST1              -4 -> 0x0F SPIDER1  -3 -> 0x0A ZOMBIE1
     -2 -> 0x05 SLIME1              -1 -> 0x00 KNIFE1
   Each frame is blitted over the current frame and followed by 0x8EC3(0x14);
   any key ([0x2B83]) aborts.  The port used the main sprite bank (Dave's
   stand/walk sprites) and the wrong code order. */
static void play_death_anim(int code)
{
    static const struct { int start, count; } tab[8] = {
        { 0x23, 5 }, { 0x1E, 5 }, { 0x14, 5 }, { 0x19, 5 },
        { 0x0F, 5 }, { 0x0A, 5 }, { 0x05, 5 }, { 0x00, 4 }
    };
    int idx, i, k, px, py;
    if (code < -8) code = -8;
    if (code > -1) code = -1;
    idx = code + 8;                             /* -8 -> 0 ... -1 -> 7 */
    /* 0x22E0..0x230C: the camera is masked to 16-px units; the x result is a
       BYTE offset for 0x91F8 arg1 (so the pixel x is x_byte*8) and y is the
       destination row.  Both lose 2 bytes / 8 rows respectively. */
    px = ((dd2_objs[0].x - (dd2_cam_x & 0xFF00)) >> 4) - 16;
    py = ((dd2_objs[0].y - (dd2_cam_y & 0xFF00)) >> 4) - 8;
    for (i = 0; i < tab[idx].count; i++) {
        death_blit(tab[idx].start + i, px, py);
        Plat_Present(fb, DD2_W, DD2_H, cur_pal);
        for (k = 0; k < 0x14; k++) Video_TickDelay();   /* 0x8EC3(0x14) */
        Plat_PumpEvents();
        {
            const bool *keys = Plat_Keys();
            /* 0x2322: `cmp byte ptr [0x2B83],0 / jne 0x2338`.  [0x2B83] is the
               key-state byte for SCAN CODE 1, i.e. ESC -- the keyboard ISR
               writes the table at 0x8BFD as `[0x2B82 + scan]` (the port used to
               abort on ANY held key, so holding fire/jump cut the animation
               after one frame).  Holding ESC at startup also gates the intro
               (0x1773) and the map screen (0x21FF). */
            if (keys[0x01]) return;
        }
    }
}

static int level_after_frame(void)
{
    int i;

    /* 0x1EAB tail: 0x64 -> 1 once the ONLY active objects left are DELBERT
       (type 0x0C).  Any other live type keeps the wait flag at 0x64 (0x2162). */
    if (dd2_door_flag == 0x64) {
        dd2_door_flag = 1;
        for (i = 1; i < dd2_obj_count; i++) {
            dd2_obj *o = &dd2_objs[i];
            if (o->f02 != 0 && o->type != 0x0C) { dd2_door_flag = 0x64; break; }
        }
        /* 0x217F: level 8 -- the final boss level ends the game. */
        if (dd2_door_flag == 1 && level == 8) {
            dd2_door_flag = 0;                         /* 0x2186 */
            dd2_objs[1].info = 0x1144;                 /* 0x218C: [0x52A8] */
        }
    }
    if (dd2_door_flag == 0 || dd2_door_flag == 0x64)
        return 0;                                  /* still playing */

    LOGF("level state: flag=%d level=%d lives=%d", dd2_door_flag, level, dd2_lives);

    if (dd2_door_flag > 0) {                       /* 0x2376: level complete */
        LOGF("level complete -> next level");
        if (dd2_sfx) dd2_sfx(0x0B);                /* LEVELDON */
        level++;                                   /* 0x2376: inc [0x97AE] */
        if (level == 9)                            /* 0x2383: cmp [0x97AE],9 */
            Screen_Ending();                       /* 0x238A: call 0x10D0 */
        if (dd2_lives == 0 || level >= 9) {        /* 0x238D/0x2394 -> 0x239E */
            unsigned score = dd2_score_lo + (dd2_score_hi * 65536u);
            unsigned best  = high_score_lo + (high_score_hi * 65536u);
            LOGF("game over (lives=%d level=%d) -> high score / continue",
                 dd2_lives, level);
            if (dd2_sfx) dd2_sfx(9);               /* GAMEOVER */
            show_center_text("G A M E   O V E R", 0x11, 3);   /* 0x2343 */
            if (score > best) {                    /* 0x23A5: 32-bit compare */
                Screen_HighScore(score, best);     /* 0x23B9 */
                high_score_lo = dd2_score_lo;      /* 0x245D: [0xA6E0..0xA6E2] */
                high_score_hi = dd2_score_hi;
            }
            if (Screen_Continue(level - 1)) {      /* 0x24EF / 0x2561 */
                int n = level - 1;
                /* The original subtracts 2 and lets 0x21E9 add 1, giving the
                   level just shown; on level 1 that would be LEVEL00.DD2,
                   which does not exist, so it is clamped here. */
                if (n < 1) n = 1;
                game_restart_at(n);
            } else {
                game_to_title = 1;                 /* 0x256E: return to 0x1785 */
            }
        } else {
            game_start_level(level);               /* 0x239B: jmp 0x21F3 */
        }
        return 1;
    }

    /* 0x2295: Dave was hit. */
    if (dd2_sfx) dd2_sfx(0x0A);                    /* DEATHSOUND */
    play_death_anim(dd2_door_flag);                /* 0x229E..0x2336 */
    dd2_lives--;                                   /* 0x2338 */
    if (dd2_lives <= 0) {                          /* 0x2343: game over */
        unsigned score = dd2_score_lo + (dd2_score_hi * 65536u);
        unsigned best  = high_score_lo + (high_score_hi * 65536u);
        LOGF("game over -> high score / continue");
        if (dd2_sfx) dd2_sfx(9);                   /* GAMEOVER */
        show_center_text("G A M E   O V E R", 0x11, 3);   /* 0x2343 */
        if (score > best) {                        /* 0x23A5: 32-bit compare */
            Screen_HighScore(score, best);         /* 0x23B9 */
            high_score_lo = dd2_score_lo;          /* 0x245D: [0xA6E0..0xA6E2] */
            high_score_hi = dd2_score_hi;
        }
        if (Screen_Continue(level - 1)) {          /* 0x24EF / 0x2561 */
            int n = level - 1;
            /* The original subtracts 2 from [0x97AE] and lets 0x21E9 add 1,
               giving the level just displayed (current-1).  On level 1 that
               would be level 0 = LEVEL00.DD2, which does not exist, so clamp. */
            if (n < 1) n = 1;
            game_restart_at(n);
        } else {
            game_to_title = 1;                     /* 0x256E: return to 0x1785 */
        }
    } else {
        game_start_level(level);                   /* same level, one life less */
    }
    return 1;
}

/* 0x8E8D clock source: hundredths of a second (int 21h/AH=2Ch DL). */
static int clock_hundredths(void) { return (int)((Plat_Ticks() / 10) % 100); }

/* 0x41C prompts: draw the frame + line, then WAIT for a key (0x87B9).
   Or the original: 0x049D `call 0x87B9` / 0xBDAF, then 'N' (0x04A5) or 'Y'
   (0x04B5) act and ANY other key resumes play (0x04BC -> 0x62C).  The port
   used to stay in a polling menu_pending state, so e.g. SPACE left the
   "QUIT (Y/N)?" prompt hanging forever. */
static void menu_prompt(int code, int *running)
{
    Video_FromIndexed(fb, DD2_W, DD2_H);   /* keep the level behind the panel */
    switch (code) {
    case 0x01: Video_CenterPanel(&A, 0x0C, 1);
               Video_PrintStr(dd2_string(0x02F0)); break;   /* QUIT (Y/N)?  */
    case 0x3C: Video_CenterPanel(&A, 0x0D, 1);
               Video_PrintStr(dd2_string(0x02D1)); break;   /* Sound (Y/N)? */
    case 0x3F: Video_CenterPanel(&A, 0x12, 1);
               Video_PrintStr("RESET GAME (Y/N)?"); break;
    default: return;
    }
    present_now();
    {
        int sc = Input_WaitKey();                       /* 0x87B9 */
        int ch = (sc > 0) ? Plat_ScancodeChar(sc) : 0;  /* 0xBDAF */
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch == 'Y') {                                /* 0x04B5 */
            if (code == 0x01) *running = 0;             /* 0x0553: quit */
            else if (code == 0x3C) Sound_SetEnabled(1); /* 0x04BF */
            else if (code == 0x3F) game_reset_pending = 1;  /* 0x04E0 */
        } else if (ch == 'N') {                         /* 0x04A5 */
            if (code == 0x3C) Sound_SetEnabled(0);      /* 0x04AC */
        }
    }
}

/* 0x6B2B -- the attract screen's 'I' panel: 0x812F(0x18, 8) and six centred
   lines (DS:0x279E/0x27B1/0x27BA/0x27BC/0x27D1/0x27E8), then 0x75D7 + 0x75F1
   (a key wait).  It is drawn over the current star/title frame, so the frame is
   pushed into the planes first. */
static void Screen_AttractInfo(void)
{
    if (getenv("DD2_TRACE_MENU")) LOGF("attract: 'I' panel shown");
    Video_FromIndexed(fb, DD2_W, DD2_H);
    Video_CenterPanel(&A, 0x18, 8);                 /* 0x6B44 */
    dd2_text_y += 2;                                /* 0x6B4A */
    Video_PrintStrC("Dangerous Dave II\n");         /* 0x279E */
    Video_PrintStrC("v1.00du\n");                   /* 0x27B1 */
    Video_PrintStrC("\n");                          /* 0x27BA */
    Video_PrintStrC("A Gamer's Edge Game\n");       /* 0x27BC */
    Video_PrintStrC("Copyright (C) 1991 by\n");     /* 0x27D1 */
    Video_PrintStrC("Softdisk, Inc.\n");            /* 0x27E8 */
    present_now();
    Input_WaitKey();                                /* 0x6B85: 0x75D7 + 0x75F1 */
}
/* --------------------------------------------------------------- main */

/* --- startup log ------------------------------------------------------
   Written next to the exe so one can tell at which step a build on
   another machine crashes (there may be no window at all). */
static FILE *g_log;
void LOGF(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void log_open(void)
{
    char p[1400];
    const char *base = Plat_BasePath();
    if (base) snprintf(p, sizeof p, "%sdd2_startup.log", base);
    else      snprintf(p, sizeof p, "dd2_startup.log");
    g_log = fopen(p, "w");
    if (g_log) LOGF("dd2 start, log at %s", p);
}


/* 0x754B: the device is selected by [0x97D6] (dd2_input_mode): 0 = keyboard,
   2/3 = joystick (only after the F4 calibration; the boot value is 0, 0x1705).
   The original therefore never reads the pad in mode 0, and modes 2/3 differ
   in the button order.  Returns without touching anything in keyboard mode.
   (The 0x73AE axes and the 0x7386 button masks are applied here; the keyboard
   path of 0x754B is Input_Frame(keys).) */
static void input_apply_pad(void)
{
    int pc, btns;
    if (dd2_input_mode < 2) return;
    pc = Plat_PadCmd();                  /* raw index 0..8, -1 = no pad */
    /* 0x73AE maps the 3x3 grid through CS:0x7539; index 4 (centred) = 8.
       In joystick mode 0x754B calls 0x73AE instead of 0x71A5, so the keyboard
       is not read at all and the command comes solely from the stick. */
    dd2_cmd = (uint8_t)((pc >= 0) ? Input_CmdFromIndex(pc) : 8);
    btns = Plat_PadButtons();
    dd2_button1 = dd2_button2 = 0;
    if (dd2_input_mode == 2) {
        if (btns & 1) dd2_button1 = 1;
        if (btns & 2) dd2_button2 = 1;
    } else {
        if (btns & 1) dd2_button2 = 1;
        if (btns & 2) dd2_button1 = 1;
    }
    if (dd2_button_swap) {               /* [0xA6DE] */
        uint8_t t = dd2_button1; dd2_button1 = dd2_button2; dd2_button2 = t;
    }
}

/* 0x41C -- F-menu dispatcher, shared by the game loop (0x2134) and the title
   loop (0x157F).  `mk` is the menu key code returned by Input_MenuKey. */
static void menu_dispatch(int mk, int *running, bool *keys)
{
    switch (mk) {
    case 0x01: menu_prompt(0x01, running);          /* QUIT (Y/N)? */
        memset(keys, 0, sizeof(bool) * 128); break; /* 0x75D7 */
    case 0x0F: Screen_Status(); break;              /* 0x55B TAB: status */
    case 0x3B: Screen_Info(); break;                /* 0x239 F1: information */
    case 0x3C: menu_prompt(0x3C, running);          /* Sound (Y/N)? */
        memset(keys, 0, sizeof(bool) * 128); break;
    case 0x3F: menu_prompt(0x3F, running);          /* RESET GAME (Y/N)? */
        memset(keys, 0, sizeof(bool) * 128); break;
    case 0x3D: Screen_KeyConfig(); break;           /* 0x4C8 F3: keyboard */
    case 0x3E: Screen_JoyConfig(); break;           /* 0x4D1 F4: joystick */
    case 0x58:                                      /* 0x519 F12 -> 0x341 */
        dd2_god_mode = !dd2_god_mode;               /* xor [0x4A8C],1 */
        Video_CenterPanel(&A, 0x0F, 1);
        Video_PrintStrC(dd2_god_mode ? "God mode on" : "God mode off");
        present_now();
        break;
    default: break;
    }
}

int main(int argc, char **argv)
{
    const char *datadir = ".";
    const char *shot = NULL, *readback = NULL, *record = NULL;
    int rb_done = 0, rec_index = 0;
    int argi, frames = 0, walk = 0, fire = 0, jump = 0;
    char exepath[512];
    bool keys[128];            /* DOS scan codes */
    int camx = 0, camy = 0, running = 1, mode = 0;
    int menu_arm = 1;
    int attract = 0, attract_hold = 0, info_armed = 1;

    log_open();
    apply_fade(3);                     /* full EGA palette */
    /* 0x16F4: 0x8E8D(1) -- seed the "random" walk index from the clock ONCE
       per run.  DD2_RAND_SEED forces it for reproducible comparisons. */
    dd2_clock_hundredths = clock_hundredths;      /* int 21h/AH=2Ch */
    {
        const char *rs = getenv("DD2_RAND_SEED");
        if (rs && *rs) dd2_rand_set(atoi(rs)); else dd2_rand_seed(1);
    }
    LOGF("args=%d", argc);
    for (argi = 1; argi < argc; argi++) {
        if (!strcmp(argv[argi], "--level") && argi + 1 < argc) level = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "--shot") && argi + 1 < argc) shot = argv[++argi];
        else if (!strcmp(argv[argi], "--frames") && argi + 1 < argc) frames = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "--readback") && argi + 1 < argc) readback = argv[++argi];
        else if (!strcmp(argv[argi], "--record") && argi + 1 < argc) record = argv[++argi];
        else if (!strcmp(argv[argi], "--walk") && argi + 1 < argc) walk = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "--jump") && argi + 1 < argc) jump = atoi(argv[++argi]);
        else if (!strcmp(argv[argi], "--fire")) fire = 1;
        else if (!strcmp(argv[argi], "--attract")) attract = 1;
        /* strings parsed by separate branches must not end up in datadir */
        else if (!strcmp(argv[argi], "--sprshot") && argi + 2 < argc) argi += 2;
        else if (!strcmp(argv[argi], "--dumpspr") && argi + 1 < argc) argi += 1;
        else if (argv[argi][0] != '-') datadir = argv[argi];   /* data directory */
    }
    exepath[0] = 0;   /* game data is fully built into the port */
    if (argc > 3 && !strcmp(argv[1], "--sprshot")) {
        int idx = atoi(argv[2]);
        const char *out = argv[3];
        LOGF("loading assets from %s", datadir);
    if (!dd2_load_assets(&A, datadir, exepath)) { Plat_Log("load failed"); return 1; }
        memset(fb, 0, sizeof fb);
        draw_sprite(idx, 8, 8, 0);                       /* raw */
        draw_sprite(idx, 48, 8, 1);                      /* flipped */
        {   int cx, cy;                                   /* scaled x3 */
            for (cy = 0; cy < A.sprites[idx].h; cy++)
                for (cx = 0; cx < A.sprites[idx].w; cx++) {
                    int sx = 100 + cx * 3, sy = 8 + cy * 3, ox, oy;
                    if (!A.sprites[idx].mask[cy * A.sprites[idx].w + cx]) continue;
                    for (oy = 0; oy < 3; oy++)
                        for (ox = 0; ox < 3; ox++)
                            if (sx + ox < DD2_W && sy + oy < DD2_H)
                                fb[(sy + oy) * DD2_W + sx + ox] =
                                    A.sprites[idx].px[cy * A.sprites[idx].w + cx];
                }
        }
        fb_to_rgba();
        {   FILE *f;
            write_bmp(out, rgba, DD2_W, DD2_H);
            f = NULL; (void)f;
        }
        Plat_Log("sprite %d: %dx%d bbox x %d..%d y %d..%d", atoi(argv[2]),
                A.sprites[atoi(argv[2])].w, A.sprites[atoi(argv[2])].h,
                A.sprites[atoi(argv[2])].bx0, A.sprites[atoi(argv[2])].bx1,
                A.sprites[atoi(argv[2])].by0, A.sprites[atoi(argv[2])].by1);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--sndtest")) {
        /* run the original sequences through 0x8DA2 */
        const char *nm[] = { "JUMP", "LAND", "SHOTWALL", "OPENDOOR", "TREASURE", 0 };
        int i, k;
        for (i = 0; nm[i]; i++) {
            char line[512];
            int n = 0;
            Sound_EffectByName(nm[i]);
            line[0] = 0;
            for (k = 0; k < 40; k++) {
                int f;
                Sound_Tick();
                f = Sound_CurrentDivisor();
                n += snprintf(line + n, sizeof line - n, "%s%d", k ? " " : "", f);
                if (n > (int)sizeof line - 16) break;
            }
            Plat_Log("%-9s %s", nm[i], line);
        }
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--dumpspr")) {
        int i;
        LOGF("loading assets from %s", datadir);
    if (!dd2_load_assets(&A, datadir, exepath)) { Plat_Log("load failed"); return 1; }
        for (i = 0; i < 24; i++)
            Plat_Log("spr %3d  %-10s %3dx%-3d px=%p mask=%p off=%d", i,
                    A.sprites[i].name, A.sprites[i].w, A.sprites[i].h,
                    (void *)A.sprites[i].px, (void *)A.sprites[i].mask, A.sprites[i].off);
        {
            const dd2_sprite *sp = &A.sprites[1];
            int y, x;
            Plat_Log("--- sprite 1 (%s) %dx%d ---", sp->name, sp->w, sp->h);
            for (y = 0; y < sp->h; y++) {
                char row[64];
                for (x = 0; x < sp->w && x < 60; x++)
                    row[x] = sp->mask[y * sp->w + x] ? "0123456789ABCDEF"[sp->px[y*sp->w+x] & 15] : '.';
                row[x] = 0;
                Plat_Log("%s", row);
            }
        }
        return 0;
    }
    LOGF("datadir(before)=%s", datadir);
    datadir = resolve_datadir(datadir);
    LOGF("datadir(after)=%s", datadir);
    LOGF("loading assets from %s", datadir);
    if (!dd2_load_assets(&A, datadir, exepath)) {
        char msg[2048];
        snprintf(msg, sizeof msg,
                 "Game data (*.DD2) not found.\n\n"
                 "Looked in: %s\n\n"
                 "Put EGATILES.DD2, LEVEL01.DD2, S_*.DD2 and the other\n"
                 "files from the original disk next to dd2.exe (or in the\n"
                 "data subdirectory), or run:\n"
                 "    dd2.exe <path to the game directory>",
                 datadir);
        Plat_Log("cannot load DD2 data from '%s' (need EGATILES.DD2 etc.)", datadir);
        Plat_MessageBox("Dangerous Dave 2", msg);
        return 1;
    }
    LOGF("assets ok: nsprites=%d ntiles=%d", A.nsprites, A.ntiles);
    g_datadir = datadir;
    Video_SetDataDir(datadir);   /* before game_start_level: the map screen 0x1227 is there */
    if (!dd2_load_level(&A, datadir, level)) {
        Plat_Log("cannot load LEVEL%02d.DD2", level);
        return 1;
    }
    /* Sound requires an INITIALIZED audio subsystem: without this
       The audio stream is opened by the platform layer (Plat_AudioInit),
       which also brings up SDL_INIT_AUDIO first: without that the stream
       failed to open and the game was silent. */
    Plat_AudioInit();                  /* opens the output stream */
    Plat_SetIdleHook(sound_pump);      /* keep the 140 Hz sequence running in delays */
    Input_Init();
    /* 0x21B0/0x21E3: session start -- four lives, zero score. */
    dd2_lives = 4;                     /* [0x8BC8] = 4 */
    dd2_score_lo = dd2_score_hi = 0;   /* [0x9320..0x9322] */
    score_extra_reset();               /* [0x8C3A..0x8C3C] = 0 */
    game_start_level(level);           /* 0x0DAD + 0x53FF + 0x3741 */
    LOGF("level %d ok: %dx%d, objects=%d", level, A.level.w, A.level.h, dd2_obj_count);


    /* As in the original: the game starts with the splash -- STARPIC + two lines
       (0x6720), then the title-screen loop with scrolling (0x14F8/0x14A0). */
    Video_Clear();
    Video_AttractBegin();
    attract_hold = 15;             /* ~0.25 s: reference AVI shows STARPIC for
                                      only ~0.1-0.35 s before the 0x6855 logo
                                      animation starts (logo off-screen for the
                                      first ~0.65 s, so STARPIC alone ~0.8 s) */
    mode = 1;                      /* 1 = STARPIC, 2 = title loop, 0 = game */
    /* for headless level capture we do not run the splash */
    if (!attract && (shot || readback || record)) { attract_hold = 0; mode = 2; }

    if (!Plat_VideoInit(3)) return 1;      /* window, renderer, texture */

    /* Sound is one output stream owned by the platform (44.1 kHz, S16,
       square-wave callback).  The old second stream (astream, F32) was
       removed: it competed for the device with the correct one. */

    while (running) {
        if (getenv("DD2_TRACE_DAVE")) LOGF("loop: mode=%d hold=%d blank=%d tick=%d", mode, attract_hold, blank_hold, tick);
        int i;
        Plat_PumpEvents();                  /* events + fresh key state */
        if (Plat_QuitRequested()) running = 0;
        memcpy(keys, Plat_Keys(), sizeof keys);
        /* Debug input injection, available in EVERY mode (the attract too):
           DD2_HOLD=<scancode>[,<period>] holds (or pulses) a key. */
        if (getenv("DD2_HOLD")) {
            static int hsc = -1, hper;
            if (hsc < 0) {
                const char *e = getenv("DD2_HOLD"), *c;
                hsc = (int)strtol(e, (char **)&c, 0);
                hper = (*c == ',') ? (int)strtol(c + 1, NULL, 0) : 0;
                if (hsc < 0 || hsc >= 128) hsc = -1;
            }
            if (hsc >= 0 && (hper <= 0 || (tick / hper) % 2 == 0)) keys[hsc] = true;
        }
        /* Port convenience for level testing: 1..8 select a level, N = next.
           Gated behind DD2_LEVELKEYS: in the original these are ordinary
           keys, and N is a valid answer in the Y/N prompts, so the ungated
           shortcut made "N" in the Sound/Reset-Game prompt jump a level
           (the check ran every iteration, before the menu dispatcher). */
        if (getenv("DD2_LEVELKEYS")) {
            if (keys[0x02]) game_start_level(1);
            else if (keys[0x03]) game_start_level(2);
            else if (keys[0x04]) game_start_level(3);
            else if (keys[0x05]) game_start_level(4);
            else if (keys[0x06]) game_start_level(5);
            else if (keys[0x07]) game_start_level(6);
            else if (keys[0x08]) game_start_level(7);
            else if (keys[0x09]) game_start_level(8);
            else if (keys[0x31]) game_start_level(level % 8 + 1);
        }
        /* Menu debug: press a menu key for a few frames
           (DD2_MENU_TEST=TAB|F2|F5|F3|F4|ESC). */
        if (getenv("DD2_MENU_TEST") && tick > 70 && tick < 76) {
            const char *m = getenv("DD2_MENU_TEST");
            if (!strcmp(m, "TAB")) keys[0x0F] = true;
            else if (!strcmp(m, "F2")) keys[0x3C] = true;
            else if (!strcmp(m, "F5")) keys[0x3F] = true;
            else if (!strcmp(m, "F3")) keys[0x3D] = true;
            else if (!strcmp(m, "F4")) keys[0x3E] = true;
            else if (!strcmp(m, "ESC")) keys[0x01] = true;
        }

        if (mode) {                            /* splash */
            /* 0x6B2B: 'I' (scan code 0x17, stored as 0x97 by the ISR) opens the
               information panel while the attract is running. */
            if (keys[0x17] && info_armed) {
                info_armed = 0;
                Screen_AttractInfo();
                memset(keys, 0, sizeof keys);
            } else if (!keys[0x17]) {
                info_armed = 1;
            }
            if (mode == 1) {                   /* STARPIC + two lines (0x6720) */
                if (attract_hold > 0) attract_hold--;
                else if (star_di <= 300) {     /* 0x6855-0x6B0E: the logo flies in */
                    if (star_di == 0) {
                        /* 0x67F6: load INTRO.DD2 and span-encode it (0x6246).
                           The logo is drawn OVER the STARPIC star field: the
                           original's 0x9313 copy restores only the previous
                           logo's rectangle from the second page. */
                        star_di = 1;
                        star_hold = 200;       /* 0x6B93: 0xC8 retraces holding it */
                        Star_TablesInit();
                        if (Star_LoadLogo(Video_DataDir()) == 0) Star_EncodeSpans();
                        else star_di = 301;    /* no INTRO.DD2: skip the phase */
                    } else {
                        /* 0x6AB0 (inc di) + 0x6AC9 (di += [0x2C8D], the ticks
                           elapsed during the 0x90A8 retrace wait): about
                           1 + 2 = 3 steps per 70 Hz frame, so the fly-in takes
                           ~100 frames, not 300. */
                        star_di += 3;
                    }
                    /* 0xC7FA: the sweeping PC-speaker tone.  It is applied to
                       the platform just before the frame delay below, gated at
                       70 Hz there (the ISR cut), so frame jitter cannot smear
                       the "brrr" into a continuous note. */
                    star_tone = (star_di <= 300) ? Star_ProjectToneDivisor(star_di) : 0;
                } else if (star_hold > 0) {    /* hold the finished logo (0x6B93) */
                    if (star_hold == 200) star_tone = 0;   /* 0xC826: speaker off */
                    star_hold--;
                } else if (blank_hold > 0) {
                    /* In the original the screen GOES BLACK between STARPIC and the title:
                       in the reference recording boot_03 (STARPIC) -> boot_04/boot_05
                       (pure black) -> boot_06 (title).  The port
                       switched instantly, so the transition "was not
                       felt". */
                    if (blank_hold == BLANK_FRAMES) { fade_steps(3, 0); Video_Clear(); }
                    blank_hold--;
                } else { Video_TitleBegin(); fade_steps(0, 3); mode = 2; }
            } else {                           /* title loop (0x14F8) */
                Video_AttractFrame();
                /* 0x157F: the title loop also runs the F-menu dispatcher
                   0x41C (ESC/F1/F2..F5/TAB), and 0x1586-0x15AA restores the
                   picture bank (0x9313 row copy from 0x4000) that the menu
                   screen drew over. */
                {
                    int mk = Input_MenuKey(keys);
                    if (mk && menu_arm) {
                        menu_arm = 0;
                        menu_dispatch(mk, &running, keys);
                        Video_TitlePics();
                    } else if (!mk) {
                        menu_arm = 1;
                    }
                }
                if (attract) {
                    /* --attract: run the splash and capture frames */
                } else if (!shot && !readback && !record) {
                    /* 0x15CC-0x15DF: the game starts only on SPACE ([0x2BBB])
                       or a joystick button read by 0x754B (modes 2/3) -- the
                       previous port started on ANY key. */
                    int padstart = (dd2_input_mode >= 2) && (Plat_PadButtons() & 3);
                    if (keys[0x39] || padstart) {
                        /* 0x15E2: key-table reset + SFX(0x16), then
                           0x15EE `call 0x21B0` -- the game restart.  Without
                           it the level was reused as-is: after a game over the
                           stale death flag [0x8C38] was still negative, so
                           every visit to the title re-ran the death handler
                           and the life counter ran away into the negative. */
                        memset(keys, 0, sizeof keys);
                        if (dd2_sfx) dd2_sfx(0x16);
                        /* 0x15EE: call 0x21B0 (a new game, level 1).  The
                           --level override is a port test feature, so it
                           starts the requested level instead. */
                        if (level > 1) game_restart_at(level);
                        else           game_restart();
                        mode = 0; tick = 0;
                    }
                } else if (tick >= 1) {
                    if (level > 1) game_restart_at(level);   /* 0x15EE: 0x21B0 */
                    else           game_restart();
                    mode = 0; tick = 0;
                }
            }
            Video_ToIndexed(fb, DD2_W, DD2_H);
            /* 0x649C/0x6855: the flying INTRO logo, drawn over the black
               background of its own phase. */
            if (mode == 1 && star_di > 0 && blank_hold == BLANK_FRAMES)
                Star_ProjectFrame(fb, DD2_W, DD2_H, star_di, 0, 0, DD2_W - 1, 179);
            Plat_Present(fb, DD2_W, DD2_H, cur_pal);
            if (record) {
                char rp[600];
                fb_to_rgba();
                snprintf(rp, sizeof rp, "%s/port_%04d.bmp", record, rec_index++);
                write_bmp(rp, rgba, DD2_W, DD2_H);
            }
            if (shot && (int)tick >= 1 && frames <= 1) { fb_to_rgba(); write_bmp(shot, rgba, DD2_W, DD2_H); running = 0; }
            /* 0xC7FA programs the tone once per frame; the 140 Hz sound ISR
               silences the speaker at 0x8DCA on its next tick (0..7 ms), which
               turns the sweep into a ~70 Hz "brrr".  The platform applies that
               gate sample-accurately, so it no longer depends on frame timing. */
            if (mode == 1) {
                Plat_AudioToneGated(star_tone);
                /* 0x90A8 waits for the CRTC retrace: the intro runs at 70 Hz,
                   and the fly-in (100 steps) takes ~1.4 s.  Render costs ~6 ms,
                   so sleep 8 ms to land on ~14 ms per frame. */
                Plat_Delay(8);
            } else {
                Plat_Delay(16);
            }
            tick++;
            if (frames && (int)tick >= frames) running = 0;
            continue;
        }

        /* --- 0x1227: level map screen (before the first game frame) ----- */
        if (map_hold > 0) {
            if (!map_drawn) {
                map_screen(level);
                /* 0x220B: call 0x75D7 -- this is a RESET of the key table
                   ([0x2C02]/[0x2C04] = 0 and memset(0x2B82, 0, 0x80)), not a
                   key wait.  Without it, the held SPACE that started the
                   game immediately dismissed the map screen -- and it "never
                   showed up". */
                memset(keys, 0, sizeof keys);
                map_drawn = 1;
                apply_fade(0);              /* 0x139F: start from black */
            }
            map_hold--;
            /* 0x7F13 (fade in 0 -> 3, 6 ticks per step): the map screen is
               shown while the EGA palettes climb back to full brightness. */
            {
                int lv = (180 - map_hold) / 6;
                if (lv > 3) lv = 3;     /* the fade is 4 steps (0x7F13), the hold is longer */
                apply_fade(lv);
            }
            Video_ToIndexed(fb, DD2_W, DD2_H);
            /* 0x1350: life icons -- sprite 1 at x = 0x2C + 0x10*i, y = 0xA5.
               The map is composed in video memory, so the icons are blitted
               into the converted frame (draw_sprite writes fb). */
            {
                int li;
                const dd2_sprite *sp = &A.sprites[1];
                /* 0x134B: `mov di,1` .. 0x1366 `cmp di,[bp-0x10]` / `jl` --
                   n-1 icons from x = 0x2C. */
                for (li = 1; li < map_lives; li++)
                    draw_sprite(1, 0x2C + 0x10 * (li - 1) + (sp->ox >> 4),
                                0xA5 + (sp->oy >> 4), 0);
            }
            /* 0x13A5: the "you are here" marker -- sprite 0x43 at
               x = ((T + 0x40) >> 3) - 8, row = word[0x120 + level*2], with
               T = word[0x10E + level*2] (tables DS:0x10E/0x120). */
            if (level >= 1 && level <= 8) {
                static const int map_t[9]   = { 0, 0x14, 0x96, 0xB6, 0xE2,
                                                0xF4, 0x122, 0x46, 0x3E };
                static const int map_row[9] = { 0, 0x50, 0x16, 0x65, 0x50,
                                                0x50, 0x60, 0x6A, 0x92 };
                /* 0x13B7-0x13CB: the record is `0x43*4 + ((T & 7) >> 2)`,
                   i.e. bits 2-3 of T select one of the pre-shifted records
                   (phase 1 is the same sprite shifted 4 px).  0x1406-0x141B:
                   the destination is a BYTE offset -- row*[0x3004] + x + base
                   -- so the pixel x is x_byte*8. */
                int mx = (((map_t[level] + 0x40) >> 3) - 8) * 8
                         + (((map_t[level] & 7) >> 2) * 4);
                draw_sprite(0x43, mx, map_row[level], 0);
            }
            Plat_Present(fb, DD2_W, DD2_H, cur_pal);
            if (record) {
                char rp[600];
                fb_to_rgba();
                snprintf(rp, sizeof rp, "%s/port_%04d.bmp", record, rec_index++);
                write_bmp(rp, rgba, DD2_W, DD2_H);
            }
            Plat_Delay(16);
            tick++;
            if (frames && (int)tick >= frames) running = 0;
            continue;
        }

        if (getenv("DD2_TRACE_DAVE")) LOGF("A: after attract, mode=%d", mode);
        /* --- update ------------------------------------------------- */
        if (walk > 0) { keys[0x4D] = true; walk--; }
        if (dd2_input_mode < 2) Input_Frame(keys);   /* 0x71A5: keyboard */
        input_apply_pad();                           /* 0x73AE: joystick (mode 2/3) */

        /* 0x41C: F-MENU DISPATCHER.  By the key code the original goes to
           separate handlers, each draws a frame (0x81AE) and a line,
           waits for Y/N (0x87B9 + 0xBDAF -> [0x931C]) and either acts or
           returns to the game.  Codes: ESC 0x01, TAB 0x0F, F1 0x3B,
           F2 0x3C, F3 0x3D, F4 0x3E, F5 0x3F, F12 0x58. */
        {
            int mk = Input_MenuKey(keys);
            if (getenv("DD2_TRACE_MENU")) LOGF("menu: tick=%d mk=%#x arm=%d", tick, mk, menu_arm);
            if (mk && menu_arm) {
                menu_arm = 0;
                menu_dispatch(mk, &running, keys);           /* 0x41C */
            } else if (!mk) menu_arm = 1;
        }
        /* --fire: hold the fire button, but NOT from the first frame -- the latch
           [0x8BC6] is armed only when the button is released (0x1EE1). */
        if (fire && tick > 70) keys[0x38] = true;

        /* --- update: 0x2585 (delta) + 0x754B (input) + 0x1EAB (frame) -- */
        if (walk > 0) { keys[0x4D] = true; walk--; }
        if (jump > 0) { keys[0x1D] = true; jump--; }
        if (dd2_input_mode < 2) Input_Frame(keys);   /* 0x71A5: keyboard */
        input_apply_pad();                           /* 0x73AE: joystick (mode 2/3) */
        dave_cmd         = dd2_cmd;        /* [0x8BB2] */
        dave_button1     = dd2_button1;    /* [0x8BB4] jump */
        dave_button2     = dd2_button2;    /* [0x8BB6] fire  */
        dave_frame_delta = 1;              /* 70 Hz: one "pair" of ticks */
        /* 0x1EBF: call 0x2585 -- frame delta from the timer ([0x8C6A]) */
        dd2_ticks += 2;                    /* 70 Hz rate -> 2 ticks per frame */
        sound_pump();                      /* 0x8DA2: 140 Hz sound tick */
        Obj_Delta();
        /* 0x1EB3 ([0x8BB8] = 8) is executed only ONCE per 0x1EAB call -- the
           internal frame loop jumps back to 0x1EBF (0x21A7), past it.  So
           [0x8BB8] is the persistent previous command; Dave_Init sets the
           initial 8.  (Setting it per frame here re-dispatched every frame and
           broke the 8-record walk chain.) */
        /* 0x1EE1: `if the fire button is NOT pressed -> [0x8BC6] = 1` -- THE LATCH
           IS ARMED.  Without this the sight (0x3B6F) and the stand (0x3891) never
           fired: they check `button AND latch`, and the latch was zero all the
           time. */
        if (game_reset_pending) {           /* 0x0510: [0x8C34] = 1 -> restart */
            game_reset_pending = 0;
            game_restart();
        }
        if (dd2_button2 == 0) dave_fire_latch = 1;
        if (getenv("DD2_TRACE_DAVE")) LOGF("B: before Obj_Frame");
        Obj_Frame();                       /* 0x1EAB: three passes */
        /* 0x1AC9: push-out from solid tiles along X (before this 0x1D76
           saved +0x22/+0x26 -- the edges before the move). */
        /* Dave's collision -- NOT here: it is done by the reactions of his records
           (0x4338/0x4380 for stand, walk, sight and 0x43A9 for jump and
           fall), as in the original.  The homemade per-frame call is removed. */
        if (getenv("DD2_TRACE_DAVE")) {
            dd2_obj *d = &dd2_objs[0];
            LOGF("dave t=%d x=%d y=%d xm=%d ym=%d f2c=%d grav=%d info=%#06x spr=%d",
                    tick, d->x, d->y, dd2_xmove, dd2_ymove, d->f2c, dave_jump_vy,
                    (unsigned short)d->info, d->sprite);
        }
        if (getenv("DD2_TRACE_OBJS") && (tick % 20) == 0) {
            int q;
            for (q = 1; q < DD2_MAXOBJ; q++) {
                dd2_obj *o = &dd2_objs[q];
                if (!o->type) continue;
                LOGF("  obj%d type=%d x=%d y=%d odir=%d f40=%d info=%#06x xm=%d react=%#06x",
                     q, o->type, o->x, o->y, o->dir, o->f40, (unsigned short)o->info,
                     dd2_xmove, (unsigned short)o->react);
            }
        }
        Level_Camera();                    /* 0x1BDC */
        /* 0x206B: scan tiles under Dave's bbox -- fills [0x906C] etc. */
        Level_ScanTiles(A.level.tiles, A.level.objs, A.level.w, A.level.h,
                        A.intile, dd2_objs[0].tx_l, dd2_objs[0].tx_t,
                        dd2_objs[0].tx_r, dd2_objs[0].tx_b);
        if (getenv("DD2_TRACE_DOOR"))
            LOGF("scan: bbox l=%d t=%d r=%d b=%d -> col=%d row=%d desc=%#06x type=%d tile=%#06x",
                 dd2_objs[0].tx_l, dd2_objs[0].tx_t, dd2_objs[0].tx_r, dd2_objs[0].tx_b,
                 dd2_scan_col, dd2_scan_row, dd2_door_desc, dd2_door_type, dd2_door_tile);
        camx = dd2_cam_x >> 4;
        camy = dd2_cam_y >> 4;

        /* --- render: tiles by camera + objects from the array ---------------- */
        if (getenv("DD2_TRACE_DAVE")) LOGF("render: frame drawn, tick=%d", tick);
        memset(fb, 0, sizeof fb);
        {
            int tx, ty, px, py, k;
            for (ty = 0; ty < DD2_H / 16 + 1; ty++)
                for (tx = 0; tx < DD2_W / 16 + 1; tx++) {
                    int lx = (camx >> 4) + tx, ly = (camy >> 4) + ty;
                    int idx, sx, sy;
                    if (lx < 0 || ly < 0 || lx >= A.level.w || ly >= A.level.h) continue;
                    idx = A.level.tiles[ly * A.level.w + lx];
                    if (idx >= A.ntiles) continue;
                    sx = lx * 16 - camx;
                    sy = ly * 16 - camy;
                    for (py = 0; py < 16; py++) {
                        int fy = sy + py;
                        if (fy < 0 || fy >= DD2_H) continue;
                        for (px = 0; px < 16; px++) {
                            int fx = sx + px;
                            if (fx < 0 || fx >= DD2_W) continue;
                            fb[fy * DD2_W + fx] = A.tiles[idx].px[py * 16 + px];
                        }
                    }
                }
            /* Objects: the sprite is seated so that its opaque part
               (bbox from 0x192E) matches the object's bbox -- as the original places the
               sprite at x + rec[+8..+0x0E] + rec[+0x1C/+0x1E]. */
            /* 0x200E-0x2062: the reactions -- and therefore the sprite queue
               (0x2B0C is called only from reactions) -- run BACK TO FRONT, so
               the sprites are drawn from the last slot down to slot 0.  Drawing
               forward put Dave (slot 0) underneath every enemy he overlapped. */
            for (k = DD2_MAXOBJ - 1; k >= 0; k--) {
                dd2_obj *o = &dd2_objs[k];
                if (o->type == 0) continue;
                /* 0x2B0C/0x2C2D are called only from the reactions, and the
                   original never queues sprite 0 ("STARTDAVE", 8x8).  Only the
                   four records 0x0EB0/0x0F3C (react == 0) and 0x102C/0x1040
                   (reacts 0x4E73/0x4F0A, which queue just the rope extras
                   0x9A/0x99) carry sprite 0, so drawing it showed a spurious
                   blob on the rope/ghost records. */
                if (o->sprite > 0 && o->sprite < A.nsprites) {
                    const dd2_sprite *sp = &A.sprites[o->sprite];
                    if (sp->px) {
                        int dx = ((o->x + sp->ox) >> 4) - camx;   /* 0x2B0C */
                        int dy = ((o->y + sp->oy) >> 4) - camy;
                        if (dx + sp->w >= 0 && dx < DD2_W &&
                            dy + sp->h >= 0 && dy < DD2_H) {
                            if (getenv("DD2_DEBUG_DRAW"))
                                Plat_Log("  draw spr=%d at (%d,%d) size %dx%d obj=(%d,%d) bbox=(%d,%d)",
                                        o->sprite, dx, dy, sp->w, sp->h, o->x, o->y,
                                        o->bbox_l, o->bbox_t);
                            /* 0x2B0C has NO mirroring: the direction is already
                               encoded in the sprite index (0x181E/0x1D2A pick
                               rec[+0] for dir<=0 and rec[+2] for dir>0). */
                            if (o->flash_draw)   /* +0x46: hit -> flash (0x2C2D/0x61B7), latched pre-decrement */
                                draw_sprite_flash(o->sprite, dx, dy);
                            else
                                draw_sprite(o->sprite, dx, dy, 0);
                        }
                    }
                }
                /* extra sprites queued by this slot's reaction (rope, ...) */
                for (i = 0; i < nextra; i++) {
                    const dd2_sprite *es;
                    int ex, ey;
                    if (extra_spr[i].slot != k) continue;
                    if (extra_spr[i].spr < 0 || extra_spr[i].spr >= A.nsprites) continue;
                    es = &A.sprites[extra_spr[i].spr];
                    if (!es->px) continue;
                    ex = ((extra_spr[i].wx + es->ox) >> 4) - camx;
                    ey = ((extra_spr[i].wy + es->oy) >> 4) - camy;
                    if (ex + es->w < 0 || ex >= DD2_W) continue;
                    if (ey + es->h < 0 || ey >= DD2_H) continue;
                    draw_sprite(extra_spr[i].spr, ex, ey, 0);
                }
            }
            /* 0x2060: ammo counter.  The sprite (19 + [0x9076]) is drawn at
               world coordinates (cam_x + 0x80, cam_y + 0x80), that is always
               at screen point (8, 8). */
            {
                int spr = dave_ammo + 0x13;          /* AMMO0 = sprite 19 */
                if (getenv("DD2_DEBUG_DRAW"))
                    Plat_Log("  AMMO spr=%d ammo=%d info=%#06x", spr, dave_ammo,
                            (unsigned short)dave_info);
                if (spr >= 0 && spr < A.nsprites)
                    draw_sprite(spr, 8, 8, 0);
            }
            /* Extra object sprites (rope etc.) -- world coordinates,
               transformed the same way as regular objects. */
            for (i = 0; i < nextra; i++) {     /* extras not tied to a slot */
                const dd2_sprite *sp;
                int dx, dy, spr = extra_spr[i].spr;
                if (extra_spr[i].slot >= 0) continue;
                if (spr < 0 || spr >= A.nsprites) continue;
                sp = &A.sprites[spr];
                if (!sp->px) continue;
                dx = ((extra_spr[i].wx + sp->ox) >> 4) - camx;
                dy = ((extra_spr[i].wy + sp->oy) >> 4) - camy;
                if (dx + sp->w < 0 || dx >= DD2_W) continue;
                if (dy + sp->h < 0 || dy >= DD2_H) continue;
                draw_sprite(spr, dx, dy, 0);
            }
            nextra = 0;      /* the queue is consumed; handlers refill it next frame */
        }
        if (getenv("DD2_TRACE_DAVE")) LOGF("C: before present");
        Plat_Present(fb, DD2_W, DD2_H, cur_pal);
        if (readback && !rb_done) {
            /* verify what the *renderer* actually produces (blend/scale path) */
            if (Plat_ReadPixelsBmp(readback))
                Plat_Log("readback -> %s", readback);
            rb_done = 1;
        }
        Plat_Delay(16);
        tick++;
        /* --record DIR: dump every rendered frame (port_%04d.bmp) so
           tools/frame_cmp.py can line them up against the DOSBox-X reference. */
        if (record) {
            char rp[600];
            fb_to_rgba();
            snprintf(rp, sizeof rp, "%s/port_%04d.bmp", record, rec_index++);
            write_bmp(rp, rgba, DD2_W, DD2_H);
        }
        if (record && frames && (int)tick >= frames) { running = 0; continue; }
        if (shot && (int)tick >= (frames ? frames : 1)) {
            fb_to_rgba();
            write_bmp(shot, rgba, DD2_W, DD2_H);
            Plat_Log("wrote %s (%d objects)", shot, dd2_obj_count);
            if (getenv("DD2_DEBUG_OBJ")) {
                for (i = 0; i < DD2_MAXOBJ; i++)
                    if (dd2_objs[i].type)
                        Plat_Log("  obj type=%d spr=%d pos=(%d,%d) bbox=(%d,%d,%d,%d) dir=%d info=%#06x",
                                dd2_objs[i].type, dd2_objs[i].sprite,
                                dd2_objs[i].x, dd2_objs[i].y,
                                dd2_objs[i].bbox_l, dd2_objs[i].bbox_t,
                                dd2_objs[i].bbox_r, dd2_objs[i].bbox_b,
                                dd2_objs[i].dir, (unsigned short)dd2_objs[i].info);
                {
                    int z = -1, k2;
                    for (k2 = 0; k2 < DD2_MAXOBJ; k2++) {
                        dd2_obj *q = &dd2_objs[k2];
                        if (q->type != 2) continue;
                        if (dd2_vp_l - 2 < q->tx_r && dd2_vp_t - 2 < q->tx_b &&
                            dd2_vp_l + 0x17 > q->tx_l && dd2_vp_t + 0x0C > q->tx_t) {
                            z = k2; break;
                        }
                    }
                    Plat_Log("  DAVE y=%d ymove=%d f22=%d f24=%d f26=%d f28=%d tx=(%d,%d,%d,%d) bbox_b=%d hit=(%d%d%d%d)",
                            dd2_objs[0].y, dd2_objs[0].ymove,
                            dd2_objs[0].f22, dd2_objs[0].f24, dd2_objs[0].f26, dd2_objs[0].f28,
                            dd2_objs[0].tx_l, dd2_objs[0].tx_t, dd2_objs[0].tx_r, dd2_objs[0].tx_b,
                            dd2_objs[0].bbox_b,
                            dd2_objs[0].hit_above, dd2_objs[0].hit_below,
                            dd2_objs[0].hit_left, dd2_objs[0].hit_right);
                    if (z >= 0)
                        Plat_Log("  ZOMBIE(on screen) slot=%d x=%d y=%d f40=%d info=%#06x dir=%d tx=(%d,%d) | update_calls=%d",
                                z, dd2_objs[z].x, dd2_objs[z].y, dd2_objs[z].f40,
                                (unsigned short)dd2_objs[z].info, dd2_objs[z].dir,
                                dd2_objs[z].tx_l, dd2_objs[z].tx_r, dd2_update_calls);
                    else
                        Plat_Log("  no zombie on screen; update_calls=%d visible=%d vp=%d,%d",
                                dd2_update_calls, dd2_visible_calls, dd2_vp_l, dd2_vp_t);
                }
                Plat_Log("  hook=%d nsprites=%d infobase=%#x", dd2_obj_perframe != 0,
                        A.nsprites, DD2_ACTORINFO_BASE);
                Plat_Log("  vp=(%d,%d) tx=(%d,%d,%d,%d)", dd2_vp_l, dd2_vp_t,
                        dd2_objs[0].tx_l, dd2_objs[0].tx_t,
                        dd2_objs[0].tx_r, dd2_objs[0].tx_b);
                Plat_Log("  cam=(%d,%d) player=(%d,%d) spr=%d cmd=%d/%d b1=%d info=%#06x react=%d calls=%d",
                        camx, camy, dd2_objs[0].x, dd2_objs[0].y, dd2_objs[0].sprite,
                        dd2_cmd, dave_cmd, dd2_button1, (unsigned short)dd2_objs[0].info,
                        dd2_objs[0].react, dave_react_calls);
            }
            running = 0;
        }
        /* Level loop post-frame block (0x226E..0x239B): death / level complete. */
        if (level_after_frame()) {
            if (game_to_title) {               /* 0x256E -> 0x1785: title again */
                game_to_title = 0;
                /* 0x14F8 is re-entered from scratch: it always runs the
                   0x7F6C fade-out (0x1505) and the 0x7F13 fade-in (0x1527). */
                fade_steps(3, 0);
                Video_TitleBegin();
                fade_steps(0, 3);
                mode = 2;
                tick = 0;
            }
            continue;
        }
    }

    Plat_AudioQuit();
    Plat_VideoQuit();
    Plat_Quit();
    return 0;
}
