/* platform.h -- SDL3 platform layer.
 *
 * The port is split in two layers:
 *
 *   game layer      -- code restored from the original DOS procedures, one C
 *                      function per original procedure, annotated with its
 *                      address (objects.c, dave.c, level.c, video.c,
 *                      sound.c, input.c, dd2.c).  The game layer must NOT
 *                      include SDL and must not know about windows,
 *                      renderers, audio devices or gamepads.
 *
 *   platform layer  -- this file and platform.c.  Everything that talks to
 *                      the host: window, renderer, texture, audio device,
 *                      keyboard/gamepad, timers, logging.
 *
 * The only bridge between them is the small interface below.
 */
#ifndef DD2_PLATFORM_H
#define DD2_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------- video */
int  Plat_VideoInit(int scale);                  /* window + renderer + texture */
void Plat_VideoQuit(void);
void Plat_Quit(void);                            /* shut SDL down */
/* Show one indexed frame (palette indices) through the given RGB palette. */
void Plat_Present(const uint8_t *idx, int w, int h, const uint32_t *pal);
void Plat_Delay(int ms);                         /* frame pacing */
/* Called during Plat_Delay so background work (the 140 Hz sound
   tick, as the original's timer ISR) keeps running while the game
   blocks in an animation. */
void Plat_SetIdleHook(void (*fn)(void));
void Plat_SetTitle(const char *title);
void Plat_MessageBox(const char *title, const char *msg);

/* ---------------------------------------------------------------- audio */
void Plat_AudioInit(void);                       /* open the output stream */
void Plat_AudioQuit(void);
/* Current square-wave divisor: 0 = silence, otherwise PIT_HZ / divisor Hz.
   The audio callback reads this value; the game layer only sets it. */
void Plat_AudioTone(int divisor);
/* Same, but the tone is chopped at 70 Hz by the audio callback, emulating the
   140 Hz sound ISR (0x8DA2 -> 0x8DCA) cutting a directly-programmed tone. */
void Plat_AudioToneGated(int divisor);
int  Plat_AudioGated(void);                      /* 1 while a gated tone plays */
int  Plat_AudioReady(void);                      /* 1 if a stream is open */

/* ---------------------------------------------------------------- input */
/* The game layer works with DOS scan codes, exactly like the original: it
   never sees SDL key codes.  The platform translates once per frame. */
void        Plat_PumpEvents(void);
const bool *Plat_Keys(void);                     /* DOS scancode -> pressed */
int         Plat_QuitRequested(void);            /* window closed */
int         Plat_WaitScancode(int timeout_ms);   /* next DOS code, 0 = timeout,
                                                    -1 = quit requested */
const char *Plat_ScancodeName(int scancode);     /* DOS code -> name */
int         Plat_ScancodeChar(int scancode);     /* DOS code -> ASCII (0 = none) */
int         Plat_PadCmd(void);                   /* -1 if no gamepad */
int         Plat_PadButtons(void);
int         Plat_PadRaw(int *x, int *y);        /* raw axes, 0 if no pad */
void        Plat_PadSetThresholds(int xlo, int xhi, int ylo, int yhi);

/* ---------------------------------------------------------------- system */
uint32_t    Plat_Ticks(void);                    /* milliseconds since init */
const char *Plat_BasePath(void);                 /* directory of the binary */
/* Save what the renderer actually produced (used by --readback). */
int         Plat_ReadPixelsBmp(const char *path);
int         write_bmp(const char *path, const void *pixels, int w, int h);

/* ----------------------------------------------------------------- misc */
void Plat_Log(const char *fmt, ...);             /* diagnostics */

#endif /* DD2_PLATFORM_H */
