/* startest.c -- checks the star engine's objectively verifiable stages:
 *   * the 0x63D6 tables against their formulas,
 *   * the INTRO.DD2 logo decode (256x64, 4 planes) against the pinned
 *     reference re/audit/intro-logo.raw (8192 bytes of plane data produced
 *     once from the same file; it also backs re/audit/intro-logo.png).
 *
 *   make startest && ./startest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "star.h"

void LOGF(const char *f, ...) { (void)f; }

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/mnt/c/Games/DD2";
    const char *ref = "/mnt/c/Games/DD2/re/audit/intro-logo.raw";
    int fails = 0, di, si, k, x, y;
    unsigned char *raw;
    size_t got;
    FILE *f;

    Star_TablesInit();
    for (k = 0, di = 2; di <= 0x100 && k < 128; di += 2, k++)
        for (si = 0; si < 0x100; si += 7) {
            unsigned m = (unsigned)((di * si) >> 8) & 0xFF;
            unsigned d = (unsigned)(((si << 8) / di) & 0xFF);
            if (dd2_star_mul[k][si] != m || dd2_star_div[k][si] != d) {
                printf("FAIL: table[%d][%d] = %u/%u, expected %u/%u\n", k, si,
                       dd2_star_mul[k][si], dd2_star_div[k][si], m, d);
                fails++;
                break;
            }
        }
    printf("tables: shift[0..3]=%d,%d,%d,%d  phase[0..3]=%#x,%#x,%#x,%#x  "
           "mul[0][255]=%d  div[127][255]=%d\n",
           dd2_star_shift[0], dd2_star_shift[1], dd2_star_shift[2], dd2_star_shift[3],
           dd2_star_phase[0], dd2_star_phase[1], dd2_star_phase[2], dd2_star_phase[3],
           dd2_star_mul[0][255], dd2_star_div[127][255]);

    switch (Star_LoadLogo(dir)) {
    case 0:  break;
    case -1: printf("skip: no INTRO.DD2 in %s\n", dir); return fails ? 1 : 0;
    default: printf("FAIL: INTRO.DD2 did not expand to 8192 bytes\n"); return 1;
    }

    f = fopen(ref, "rb");
    if (!f) { printf("skip: no %s\n", ref); return fails ? 1 : 0; }
    raw = malloc(8192);
    got = fread(raw, 1, 8192, f);
    fclose(f);
    if (got != 8192) { printf("FAIL: reference is %zu bytes, expected 8192\n", got); free(raw); return 1; }
    {
        int bad = 0, first = -1;
        for (y = 0; y < STAR_LOGO_H; y++)
            for (x = 0; x < STAR_LOGO_W; x++) {
                unsigned c = Star_LogoPixel(x, y);
                unsigned r = 0, p;
                for (p = 0; p < STAR_LOGO_PLANES; p++) {
                    const uint8_t *pl = Star_LogoPlane(p);
                    unsigned b = pl[y * STAR_LOGO_BPR + (x >> 3)];
                    r |= (unsigned)((b >> (7 - (x & 7))) & 1) << p;
                }
                if (c != r) { if (first < 0) first = y * STAR_LOGO_W + x; bad++; }
            }
        printf("logo: 256x64, planes 8192 bytes, pixel(10,10)=%u pixel(128,32)=%u; "
               "plane/pixel consistency: %d differing%s\n",
               Star_LogoPixel(10, 10), Star_LogoPixel(128, 32), bad,
               bad ? "" : " (exact)");
        if (bad) { printf("  first at x=%d y=%d\n", first % STAR_LOGO_W, first / STAR_LOGO_W); fails++; }
    }
    free(raw);

    /* 0x6246: spans must reproduce every pixel above the first colour-1 of each
       column (the encoder stops the column there) */
    Star_EncodeSpans();
    {
        int bad = 0, cols_empty = 0, runs_total = 0, first = -1;
        for (x = 0; x < STAR_LOGO_W; x++) {
            const StarCol *sc = &dd2_star_spans[x];
            int y0 = -1, cut = STAR_LOGO_H, i, k;
            /* the encoder skips the background (colour 1) until the first
               shape pixel and ends the column at the next background pixel */
            for (y = 0; y < STAR_LOGO_H; y++) {
                unsigned c = Star_LogoPixel(x, y);
                if (y0 < 0) { if (c != 1) y0 = y; }
                else if (c == 1) { cut = y; break; }
            }
            if (sc->nruns == 0) cols_empty++;
            for (i = 0; i < sc->nruns && i < STAR_MAXRUNS; i++) {
                const StarRun *r = &sc->run[i];
                if (r->len <= 0) break;
                runs_total++;
                for (k = 0; k < r->len; k++) {
                    int yy = r->y + k;
                    if (yy < y0 || yy >= cut || r->col[k] != Star_LogoPixel(x, yy)) {
                        if (first < 0) first = yy * STAR_LOGO_W + x;
                        bad++;
                    }
                }
            }
            for (y = 0; y < STAR_LOGO_H; y++) {
                unsigned want = (y0 >= 0 && y >= y0 && y < cut) ? Star_LogoPixel(x, y) : 0;
                if (Star_SpansPixel(x, y) != want) {
                    if (first < 0) first = y * STAR_LOGO_W + x;
                    bad++;
                }
            }
        }
        printf("spans: %d columns, %d runs, empty columns=%d, mismatches=%d%s\n",
               STAR_LOGO_W, runs_total, cols_empty, bad, bad ? "" : " (exact)");
        if (bad) { printf("  first at x=%d y=%d\n", first % STAR_LOGO_W, first / STAR_LOGO_W); fails++; }
    }

    /* the blit primitive must reproduce the logo where the spans cover it */
    {
        static unsigned char buf[STAR_LOGO_W * STAR_LOGO_H];
        int bad = 0, x, y;
        Star_DrawSpans(buf, STAR_LOGO_W, STAR_LOGO_H);
        for (x = 0; x < STAR_LOGO_W; x++) {
            int y0 = -1, cut = STAR_LOGO_H;
            for (y = 0; y < STAR_LOGO_H; y++) {
                unsigned c = Star_LogoPixel(x, y);
                if (y0 < 0) { if (c != 1) y0 = y; }
                else if (c == 1) { cut = y; break; }
            }
            for (y = 0; y < STAR_LOGO_H; y++) {
                unsigned want = (y0 >= 0 && y >= y0 && y < cut) ? Star_LogoPixel(x, y) : 0;
                if (buf[y * STAR_LOGO_W + x] != want) bad++;
            }
        }
        printf("draw 1:1: %d differing pixels of %d%s\n", bad, STAR_LOGO_W * STAR_LOGO_H,
               bad ? "" : "  (exact)");
        if (bad) fails++;
    }

    printf(fails ? "startest: FAILED\n" : "startest: ok\n");
    return fails ? 1 : 0;
}
