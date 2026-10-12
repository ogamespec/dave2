/* sound.c -- PC-speaker sound engine, restored from the original procedures.
 *
 * This is GAME code: it reproduces the original's priority queue (0x8CB2),
 * stop (0x8CE8) and the 140 Hz tick (0x8DA2) that walks the sequence and
 * updates the PIT divisor.  It contains no SDL: the resulting divisor is
 * handed to the platform layer through Plat_AudioTone(), and the platform
 * turns it into an actual square wave.
 */
#include <string.h>
#include "sound.h"
#include "platform.h"
#include "dd2_sound.h"

static const uint16_t *cur;         /* [0x2C9E] -- current position */
static const uint16_t *cur_end;     /* guard, in case the 0xFFFF is missing */
static int             cur_pri;     /* [0x2CA0] */
static int             cur_id;
static int             divisor;     /* current PIT divisor; 0 = silence */
static int             enabled = 1; /* [0x2C97]: 0 = sound off (F2 menu) */

/* 0x8CB2: queue a sound, honouring the priority. */
void Sound_SFX(int id)
{
    const dd2_sound *s;
    if (id <= 0 || id >= DD2_NSOUNDS) return;
    s = &dd2_sounds[id - 1];                 /* table is shifted: 0 is the header */
    if (!s->seq) return;
    if (s->priority < cur_pri) return;       /* 0x8CD6: lower priority -- ignore */
    cur_pri = s->priority;                   /* 0x8CDC */
    cur     = s->seq;                        /* 0x8CDF */
    cur_end = s->seq + 2000;
    cur_id  = id;
}

/* 0x8CE8: stop. */
void Sound_Stop(void)
{
    cur = NULL; cur_end = NULL; cur_pri = 0; cur_id = 0;
    divisor = 0;
    Plat_AudioTone(0);
}

int Sound_Playing(void) { return cur != NULL; }

int Sound_CurrentDivisor(void) { return divisor; }

/* [0x2C97]: the F2 menu writes this and the timer ISR checks it at 0x8DD3 --
   when it is zero the sequence still advances, but the speaker is left off. */
void Sound_SetEnabled(int on)
{
    enabled = on ? 1 : 0;
    if (!enabled) Plat_AudioTone(0);
}

/* Look a sound up by name -- self-test only (the original uses indices). */
void Sound_EffectByName(const char *name)
{
    int i;
    for (i = 0; i < DD2_NSOUNDS - 1; i++)
        if (strcmp(dd2_sounds[i].name, name) == 0 ||
            strncmp(dd2_sounds[i].name, name, strlen(dd2_sounds[i].name)) == 0) {
            Sound_Stop();
            Sound_SFX(i + 1);
            return;
        }
}

/* 140 Hz tick: one sequence word per tick (0x8DA2). */
void Sound_Tick(void)
{
    uint16_t v;
    if (!cur || cur >= cur_end) { if (cur) Sound_Stop(); return; }
    v = *cur++;                                  /* 0x8DAF */
    if (v == 0xFFFF) { Sound_Stop(); return; }   /* 0x8DBF: end of sequence */
    divisor = v;                                 /* 0 = pause (0x8DCA) */
    Plat_AudioTone(enabled ? divisor : 0);       /* 0x8DD3 checks [0x2C97] first */
}
