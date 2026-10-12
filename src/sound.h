/* sound.h -- PC-speaker sound engine, restored from the original.
 *
 * Original procedures:
 *   0x8CB2(id)  -- queue a sound: si = id*16, priority = es:[si+2]; if it is
 *                 BELOW the current one ([0x2CA0]) the sound is ignored,
 *                 otherwise [0x2CA0] = priority and [0x2C9E] = es:[si]
 *                 (the sequence pointer).
 *   0x8DA2      -- tick (140 Hz, divisor 0x2147 at 0x16D8): reads one WORD
 *                 from the sequence and advances by 2; 0 = pause,
 *                 0xFFFF = end, otherwise a PIT divisor: freq = 1193182/d.
 *   0x8CE8      -- stop: pointer cleared, speaker off (0x61 &= 0xFD).
 *
 * The engine is pure game code: it produces a divisor and hands it to the
 * platform layer (Plat_AudioTone), which generates the square wave.
 */
#ifndef DD2_SOUND_ENGINE_H
#define DD2_SOUND_ENGINE_H

void Sound_SFX(int id);                    /* 0x8CB2 */
void Sound_Tick(void);                     /* 140 Hz tick, call twice per frame */
void Sound_Stop(void);                     /* 0x8CE8 */
void Sound_SetEnabled(int on);             /* [0x2C97]: 0 = speaker muted (0x8DD3) */
int  Sound_Playing(void);                  /* 0 if the queue is empty */
int  Sound_CurrentDivisor(void);           /* current PIT divisor (0 = silence) */
void Sound_EffectByName(const char *name); /* self-test helper: play by name */

#endif
