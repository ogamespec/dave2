/* star.c -- the attract screen's star engine (STARPIC phase, INTRO.DD2).
 *
 * The original code lives in 0x6720 (the INTRO), with helpers:
 *   0x63D6  build the scale/phase tables
 *   0x6246  span-encode the 256x64 4-plane logo into a per-column run list
 *   0x649C  draw the logo spans with a scale
 *   0x6855-0x6B0E  per-frame x87 projection of the star field
 *
 * These addresses only became readable after un-emulating the Borland 8087
 * stubs (INT 0x34..0x3B -> FWAIT + the real x87 opcode); see
 * re/audit/emu87.md and re/tools/dd2unemu.py.
 *
 * This file currently implements the two objectively checkable stages:
 *   1. the tables of 0x63D6,
 *   2. the INTRO.DD2 logo (RLE -> 4 planes, 256x64),
 * so the decoder can be verified 1:1 against re/audit/intro-logo.png.
 * The span encoder (0x6246) and the renderer (0x649C) build on these.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd2.h"
#include "star.h"

/* ------------------------------------------------------------------ 0x63D6 */
/* The original mallocs 0x1000 paragraphs (64 KB) and fills two 0x8000-byte
   tables (0x1DA0/0x1DA8 pointers):
       0x6449: for (di = 2; di <= 0x100; di += 2)
       0x644B:   for (si = 0; si < 0x100; si++) {
       0x644D:       es:[ptr1++] = (di * si) >> 8;      // byte
       0x645E:       es:[ptr2++] = (si << 8) / di;       // byte (AL only!)
                 }
   plus, before them (0x63E7):
       DS:0x9098[i] = i >> 3          (i < 0x140)
       DS:0x91DC[i] = phase           (0x80 >>= 1, restarting at 0x80)
   and the frame window [0x907C]=0, [0x907E]=0, [0x9084]=0x13F, [0x9086]=0xB3.
   Note the division truncates to a byte (the original stores AL), so the
   value is ((si << 8) / di) & 0xFF. */
uint8_t dd2_star_mul[128][256];      /* (di*si)>>8,  di = 2*(i+1) */
uint8_t dd2_star_div[128][256];      /* ((si<<8)/di) & 0xFF          */
uint8_t dd2_star_shift[0x140];       /* DS:0x9098 */
uint8_t dd2_star_phase[0x140];       /* DS:0x91DC */
int     dd2_star_x0, dd2_star_y0, dd2_star_x1 = 0x13F, dd2_star_y1 = 0xB3;

void Star_TablesInit(void)
{
    int di, si, k = 0, ph = 0x80;

    for (si = 0; si < 0x140; si++) {                  /* 0x63E7 */
        dd2_star_shift[si] = (uint8_t)(si >> 3);      /* 0x63ED */
        dd2_star_phase[si] = (uint8_t)ph;             /* 0x63F3 */
        ph >>= 1;                                     /* 0x63FA */
        if (ph == 0) ph = 0x80;                       /* 0x6404 */
    }
    for (di = 2; di <= 0x100; di += 2, k++)           /* 0x6444/0x6478 */
        for (si = 0; si < 0x100; si++) {              /* 0x6449 */
            dd2_star_mul[k][si] = (uint8_t)((di * si) >> 8);
            dd2_star_div[k][si] = (uint8_t)(((si << 8) / di) & 0xFF);
        }
}

/* ----------------------------------------------------------------- INTRO.DD2 */
/* 0x67F6: 0x77FC("INTRO.DD2", [0x9096]) then 0x6246([0x9096], 0x20, 0x40,
   [0x9088]): the file expands (RLE, the port's dd2_rle_expand, 0x7BF4) to
   8192 bytes = 4 planes x 2048 = 32 bytes/row x 64 rows = 256x64 pixels.
   The image is the "Gamer's Edge PRESENTS" logo (re/audit/intro-logo.png). */
static uint8_t logo[STAR_LOGO_PLANES * STAR_LOGO_BPR * STAR_LOGO_H];

const uint8_t *Star_LogoPlane(int p)
{
    if (p < 0 || p >= STAR_LOGO_PLANES) return NULL;
    return logo + (size_t)p * STAR_LOGO_BPR * STAR_LOGO_H;
}

unsigned Star_LogoPixel(int x, int y)
{
    int p;
    unsigned c = 0;
    if (x < 0 || x >= STAR_LOGO_W || y < 0 || y >= STAR_LOGO_H) return 0;
    for (p = 0; p < STAR_LOGO_PLANES; p++) {
        const uint8_t *pl = Star_LogoPlane(p);
        uint8_t b = pl[y * STAR_LOGO_BPR + (x >> 3)];
        c |= (unsigned)((b >> (7 - (x & 7))) & 1) << p;
    }
    return c;
}

int Star_LoadLogo(const char *datadir)
{
    char path[1024];
    uint8_t *raw;
    size_t len = 0;
    const size_t need = sizeof logo;

    if (!datadir) return -1;
    snprintf(path, sizeof path, "%s/INTRO.DD2", datadir);
    raw = dd2_read_file(path, &len);
    if (!raw) return -1;
    {
        size_t out = 0;
        uint8_t *e = dd2_rle_expand(raw, len, &out);
        free(raw);
        if (!e) return -2;
        if (out < need) { free(e); return -3; }       /* 3525 bytes -> 8192 */
        memcpy(logo, e, need);
        free(e);
    }
    return 0;
}

/* ------------------------------------------------------------------ 0x6246 */
StarCol dd2_star_spans[STAR_LOGO_W];

void Star_EncodeSpans(void)
{
    int row, bit, di;

    memset(dd2_star_spans, 0, sizeof dd2_star_spans);
    for (row = 0; row < STAR_LOGO_BPR; row++) {         /* 0x62C0: w = 32 bytes */
        for (bit = 0; bit < 8; bit++) {                 /* 0x62C8 */
            int x = row * 8 + bit;                      /* the pixel column */
            StarCol *sc = &dd2_star_spans[x];
            int n = -1, len = 0;

            for (di = 0; di < STAR_LOGO_H; di++) {      /* 0x631A */
                unsigned c = Star_LogoPixel(x, di);
                /* 0x6372: a pixel of colour 1 (the blue background) ENDS the
                   column only if a run is already open (0x639B -> 0x63A6 ->
                   the next bit); with no open run it is simply skipped
                   (0x639D, the next pixel).  The accumulator of
                   0x6346-0x636F is the colour in the LOW nibble: four
                   `rcr al,cl` + `rcl [bp-0x20]` pairs. */
                if (c == 1) {
                    if (len && n >= 0) { sc->run[n].len = len; len = 0; }
                    if (n >= 0) goto col_done;          /* the run list ends here */
                    continue;                            /* leading background */
                }
                if (len == 0) {                         /* 0x6378: a new run */
                    if (n + 1 >= STAR_MAXRUNS) break;
                    n++;
                    sc->run[n].y = di;
                    sc->run[n].len = 0;
                }
                if (n >= 0 && len < STAR_LOGO_H)        /* 0x6386/0x6389 */
                    sc->run[n].col[len] = (unsigned char)c;
                len++;
            }
            if (len && n >= 0) sc->run[n].len = len;    /* 0x63A6: the final run */
col_done:
            if (n >= 0) { sc->nruns = n + 1; if (sc->nruns < STAR_MAXRUNS) sc->nruns++; }
        }
    }
}

/* Decode the spans back into pixels -- used by tests/startest.c to prove the
   encoder is lossless for the shape it encodes (0x6246 -> 0x649C input). */
unsigned Star_SpansPixel(int x, int y)
{
    const StarCol *sc;
    int i;
    if (x < 0 || x >= STAR_LOGO_W || y < 0 || y >= STAR_LOGO_H) return 0;
    sc = &dd2_star_spans[x];
    for (i = 0; i < sc->nruns && i < STAR_MAXRUNS; i++) {
        const StarRun *r = &sc->run[i];
        if (r->len <= 0) break;                        /* the terminator */
        if (y >= r->y && y < r->y + r->len) return r->col[y - r->y];
    }
    return 0;
}

/* Draw the encoded spans 1:1 into an 8-bit buffer.  In 0x649C the same walk is
   done straight into EGA planes: for every pixel column it sets GC8 from
   DS:0x91DC[col] (the bit mask) and addresses the byte with DS:0x9098[col]
   (col >> 3) -- the two tables of 0x63D6 (see re/audit/emu87.md).  Here the
   columns are pixels of the destination buffer instead. */
void Star_DrawSpans(unsigned char *dst, int dw, int dh)
{
    int x, y, i;
    if (!dst || dw <= 0 || dh <= 0) return;
    for (y = 0; y < dh; y++)
        for (x = 0; x < dw; x++)
            dst[y * dw + x] = 0;
    for (x = 0; x < STAR_LOGO_W && x < dw; x++) {
        const StarCol *sc = &dd2_star_spans[x];
        for (i = 0; i < sc->nruns && i < STAR_MAXRUNS; i++) {
            const StarRun *r = &sc->run[i];
            int k;
            if (r->len <= 0) break;
            for (k = 0; k < r->len; k++) {
                int yy = r->y + k;
                if (yy < 0 || yy >= dh) continue;
                dst[yy * dw + x] = r->col[k];
            }
        }
    }
}
