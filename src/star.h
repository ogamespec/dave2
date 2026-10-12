/* star.h -- star engine (STARPIC/INTRO.DD2 phase); see star.c and
   re/audit/emu87.md for the addresses (0x63D6, 0x6246, 0x649C, 0x6720). */
#ifndef DD2_STAR_H
#define DD2_STAR_H
#include <stdint.h>

/* 0x63D6 tables: the original allocates 0x1000 paragraphs and fills two
   0x8000-byte blocks, indexed [di/2-1][si] with di = 2,4,..,0x100. */
extern uint8_t dd2_star_mul[128][256];   /* (di*si)>>8            */
extern uint8_t dd2_star_div[128][256];   /* ((si<<8)/di) & 0xFF   */
extern uint8_t dd2_star_shift[0x140];    /* DS:0x9098             */
extern uint8_t dd2_star_phase[0x140];    /* DS:0x91DC             */
extern int     dd2_star_x0, dd2_star_y0, dd2_star_x1, dd2_star_y1;  /* [0x907C..] */

void Star_TablesInit(void);              /* 0x63D6 */

/* INTRO.DD2: 256x64, 4 planes, 32 bytes per row per plane (0x6246/0x67F6). */
#define STAR_LOGO_W      256
#define STAR_LOGO_H      64
#define STAR_LOGO_BPR    32
#define STAR_LOGO_PLANES 4
int            Star_LoadLogo(const char *datadir);   /* 0 = ok */
const uint8_t *Star_LogoPlane(int p);                /* p = 0..3 */
unsigned       Star_LogoPixel(int x, int y);         /* 0..15 */

/* 0x6246 -- span encoder: per pixel column (w*8 of them) a list of runs,
   each run = (start_y, length, colours[length]); a run with length 0 ends the
   column's list.  Scanning a column stops at the first pixel of colour 1
   (0x6372: `cmp [bp-0x20],1 / je` ends the run and moves to the next column). */
#define STAR_MAXRUNS 8
typedef struct { int y, len; unsigned char col[STAR_LOGO_H]; } StarRun;
typedef struct { int nruns; StarRun run[STAR_MAXRUNS]; } StarCol;
extern StarCol dd2_star_spans[STAR_LOGO_W];
void Star_EncodeSpans(void);                         /* 0x6246 */
unsigned Star_SpansPixel(int x, int y);              /* decode back (test) */
/* Draw the encoded spans into an 8-bit pixel buffer (colour indices 0..15).
   Scale 1:1 for now -- the scaling of 0x649C uses the 0x63D6 tables and is the
   next step; this is the blit primitive it will call. */
void Star_DrawSpans(unsigned char *dst, int dw, int dh);

#endif
