/*
 * video.c -- reconstruction of the Dangerous Dave 2 video layer (DOS, EGA).
 *
 * Every function is tagged with its original address in DD2.EXE (image
 * re/ref/orig/DAVE2_IMAGE.bin, CS=0, DGROUP=0x27040) and has been rewritten
 * from its disassembly. Nothing here is merely "inspired by" the original.
 *
 * Screen model (verified frame by frame against DOSBox-X, see
 * PORT_PLAN.md section 14):
 *   - the frame lives in a 640x200 buffer, row = [0x3004] = 0x50 = 80 bytes;
 *   - 4 planes, each a 64K window (like A000:0000-FFFF on the EGA);
 *   - pages inside a plane: 0x0000 (STARPIC/TITLE1), 0x4000 (copy),
 *     0x8000 (copy); TITLE2 is drawn at an offset of 0x28 bytes, i.e. into
 *     the right half of the row (0x14A0);
 *   - the visible 320 px window is set by [0x9060]: 0x90A8(start=s>>3, pan=s&7),
 *     so displayed pixel x is pixel (s+x) of the buffer.
 */
#include "video.h"
#include "platform.h"
#include "video.h"
#include "dd2.h"
#include "dd2_uitiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dd2_font.h"

/* ---------------------------------------------------------------- frame */
uint8_t dd2_vram[DD2_VRAM_SIZE];
int dd2_base;                 /* [0x2FFE] */
int dd2_stride = DD2_STRIDE;  /* [0x3004] */
int dd2_scroll;               /* [0x9060] */
int dd2_ega_plane;            /* EGA state similar to [0x4230] */
int g_fast = 1;               /* 1 = no delays (headless/tests) */

int dd2_text_x, dd2_text_y, dd2_text_color;   /* [0x4228],[0x422A],[0x4226] */
int dd2_text_left, dd2_text_right = 0x140;    /* [0x9798],[0x979A] */

/* ------------------------------------------------------------- helpers */
static void plane_write(int off, const uint8_t *src, int n)
{
    int p;
    if (off < 0 || off + n > DD2_PLANE) return;
    for (p = 0; p < 4; p++) memcpy(dd2_vram + p * DD2_PLANE + off, src, (size_t)n);
}

void Video_Clear(void) { memset(dd2_vram, 0, sizeof dd2_vram); }

/* 0x7FB2(+0x7E79): set the row width. The original sets CRTC offset = w/2
   words and [0x3004] = w bytes, then rebuilds the row address table
   ([0x3006][y] = y*w, 0x7D12). The table is not needed here: the row is
   computed as y*dd2_stride. */
void Video_SetWidth(int w)
{
    if (w <= 0 || w > 0x400) return;
    dd2_stride = w;
}

/* 0x90A8: CRT start address + horizontal pel panning.
   Original arguments: [bp+4] = start (bytes), [bp+6] = pan (pixels).
   Observed behaviour: the window shift in pixels = [0x9060],
   i.e. start = [0x9060] >> 3, pan = [0x9060] & 7 (verified by comparison). */
void Video_SetScroll(int px)
{
    dd2_scroll = px;
}

/* 0x9076: select the EGA plane (here the planes are separate anyway). */
void Video_SetPlane(int plane) { dd2_ega_plane = plane & 3; }

/* 0x8C70: one timer tick (original: pushf; lcall [0x2C99]). */
void Video_TickDelay(void) { Plat_Delay(7); }   /* 0x8EC3: retrace wait */

/* 0x7938: before showing a picture the original waits 0x64 timer ticks
   (~714 ms at 140 Hz); in headless mode the pause is skipped (g_fast). */
void Video_Prep(void)
{
    int si;
    if (g_fast) return;
    for (si = 0; si < 0x64; si++) Video_TickDelay();
}

/* 0x92BC: fill with a byte.
   Arguments: (x, row, nbytes, rows, value).
     di = [0x2FFE] + [0x3006][row] + x
     es = [0x2FFA] (video segment), GC5 = 0, SEQ map mask = 0x0F (all planes)
     rep stosb nbytes ; di += [0x3004] - nbytes ; rows times                */
void Video_Fill(int x, int row, int nbytes, int rows, int value)
{
    int off = dd2_base + row * dd2_stride + x;
    int r;
    uint8_t buf[0x400];
    if (nbytes <= 0 || nbytes > (int)sizeof buf) return;
    memset(buf, value & 0xFF, (size_t)nbytes);
    for (r = 0; r < rows; r++) {
        plane_write(off, buf, nbytes);
        off += dd2_stride;
    }
}

/* 0x9313: row-by-row copy (GC5 = 1 write-mode-1, map mask 0x0F -> all
   planes at once).
     ah = nbytes ; bx = rows ; si = src ; di = dst ; dx = [0x3004] - nbytes
     rep movsb nbytes ; si += dx ; di += dx ; rows times
   The base [0x2FFE] is NOT added: src/dst are absolute offsets. */
void Video_RowCopy(int nbytes, int rows, int src, int dst)
{
    int r, p;
    if (nbytes <= 0 || rows <= 0) return;
    for (r = 0; r < rows; r++) {
        long s = (long)src + (long)r * dd2_stride;
        long d = (long)dst + (long)r * dd2_stride;
        if (s < 0 || d < 0 || s + nbytes > DD2_PLANE || d + nbytes > DD2_PLANE) continue;
        for (p = 0; p < 4; p++)
            memmove(dd2_vram + p * DD2_PLANE + d,
                    dd2_vram + p * DD2_PLANE + s, (size_t)nbytes);
    }
}

/* 0x7C77: load a PIC and blit it into video memory.
   PIC header: "PIC\0" + word rowbytes(=0x28) + word height(=0xC8),
   followed by 4 planes of rowbytes*height bytes each.
   Original arguments: (name, offset_in_row). Inside:
     di = es:[4] (= rowbytes) ; ax = di * row -> row offset in the SOURCE
     dx = [bp+6] + [0x3004] * row        -> address in the DESTINATION
   That is, the second argument is a byte offset within the row (0 for the
   left half of the 640-px row, 0x28 for the right), not a y shift. That is how
   TITLE1 and TITLE2 are placed side by side in one 640x200 buffer.                  */
void Video_ShowPicAt(const char *name, int row_off)
{
    char path[512];
    size_t len = 0;
    uint8_t *pic;
    int rowbytes, height, plane_size, plane, y, bx;

    snprintf(path, sizeof path, "%s/%s", Video_DataDir(), name);
    pic = dd2_load_pic(path, &len);
    if (!pic) return;
    if (len < 8 || memcmp(pic, "PIC\0", 4) != 0) { free(pic); return; }
    rowbytes  = pic[4] | (pic[5] << 8);
    height    = pic[6] | (pic[7] << 8);
    plane_size = rowbytes * height;
    if (rowbytes <= 0 || height <= 0 || 8 + 4 * plane_size > (int)len) { free(pic); return; }

    /* 0x7938: timer pause before the blit */
    Video_Prep();
    for (plane = 0; plane < 4; plane++)
        for (y = 0; y < height; y++) {
            const uint8_t *s = pic + 8 + (size_t)plane * plane_size + (size_t)y * rowbytes;
            int dst = plane * DD2_PLANE + row_off + y * dd2_stride;
            if (dst < 0 || dst + rowbytes > (plane + 1) * DD2_PLANE) continue;
            for (bx = 0; bx < rowbytes; bx++) dd2_vram[dst + bx] = s[bx];
        }
    free(pic);
}

/* Compatibility with the former name: the second argument used to be
   treated as a y offset, but per the disassembly it is a byte offset in the row. */
void Video_ShowPic(const char *name, int yoff) { Video_ShowPicAt(name, yoff); }

/* 0x14A0: title screen.
     0x7C77("TITLE1.DD2", 0)      ; left half of the wide row
     [0x97A6] = 0
     0x7C77("TITLE2.DD2", 0x28)   ; right half
     [0x97A6] = 1
     GC5 = 1 ; SEQ2 = 0x0F
     0x9313(0x50, 0xC8, 0, 0x4000)  ; copy the whole frame to page 0x4000  */
void Video_TitlePics(void)
{
    Video_ShowPicAt("TITLE1.DD2", 0);
    Video_ShowPicAt("TITLE2.DD2", 0x28);
    Video_RowCopy(0x50, 0xC8, 0, 0x4000);
}

/* --------------------------------------------------- 8x8 tiles and panels */
/* 0x915A -- 8x8 TILE BLIT.  The original takes the tile from the level plane
   (8 bytes per tile) and writes it to video memory at [0x2FFE] + tile +
   [row*2 + 0x3006].  In the port EGATILES.DD2 tiles are decoded as 16x16,
   so tile N is quadrant N%4 of tile N/4 (TL, TR, BL, BR).          */
void Video_Tile8(const dd2_assets *A, int tile, int px_x, int px_y)
{
    /* 0x915A, verbatim from the disassembly:
         di = [0x2FFE] + arg1 + rowtable[arg2]      (destination byte offset)
         si = arg3 * 8                              (source offset in the UI bank)
         GC5 = 0 (write mode 0, copy), then 4 iterations of
         SEQ index 2 = map mask 8,4,2,1 -> plane 3,2,1,0,
         source segment ss:[bx-0x615A] = DS:0x9EAC/0x9EAA/0x9EA8/0x9EA6,
         i.e. the four plane segments of the UI tile bank at segment 0x1881
         (0x0885 `mov word ptr [0x9EA6],0x1881`, plane size 13232 = 0x33B0).
         Each iteration copies 8 consecutive source bytes to 8 consecutive
         rows (`movsb` + `add di,stride-1`), so the bank is 1 byte per row
         and bit 7 is the leftmost pixel.  An earlier port version took the
         quadrants of the decoded 16x16 LEVEL tiles instead, which made
         panel borders look like clouds. */
    int y, x;
    (void)A;
    if (tile < 0 || tile >= DD2_UITILES_N) return;
    for (y = 0; y < 8; y++) {
        int sy = px_y + y;
        const unsigned char *b = &dd2_uitiles[0][tile * 8 + y];
        int off;
        if (sy < 0 || sy >= DD2_H) continue;
        off = dd2_base + sy * dd2_stride;
        for (x = 0; x < 8; x++) {
            int sx = px_x + x, sh, c, a, p;
            if (sx < 0 || sx >= DD2_VIRTUAL_W) continue;
            sh = 7 - (sx & 7);
            c = ((dd2_uitiles[3][tile * 8 + y] >> sh) & 1) << 3 |
                ((dd2_uitiles[2][tile * 8 + y] >> sh) & 1) << 2 |
                ((dd2_uitiles[1][tile * 8 + y] >> sh) & 1) << 1 |
                ((b[0] >> sh) & 1);
            a = off + (sx >> 3);       /* dd2_stride is in bytes, as in [0x3004] */
            if (a < 0 || a >= DD2_PLANE) continue;
            for (p = 0; p < 4; p++) {
                int o2 = p * DD2_PLANE + a;
                if ((c >> p) & 1) dd2_vram[o2] |= (uint8_t)(1 << sh);
                else              dd2_vram[o2] &= (uint8_t)~(1 << sh);
            }
        }
    }
}

/* 0x7FC9 -- PANEL made of 8x8 tiles: (x, y, w, h) in CHARACTERS (8x8 cells).
   The original's parameters are the rectangle x..w, y..h INCLUSIVE: the top
   row is drawn at row y, the sides at rows y+1..h-1, the bottom row at row h;
   the horizontal border tiles sit at column x and column w.  (The previous
   version iterated i<w / j<h with the right/bottom border at w-1 / h-1, so
   every panel was one tile narrower, one tile shorter and shifted.)
   Tiles: 1 -- top-left, 2 -- top, 3 -- top-right,
          4 -- left, 0x1B -- fill, 5 -- right,
          6 -- bottom-left, 7 -- bottom, 8 -- bottom-right.                        */
void Video_Panel(const dd2_assets *A, int x, int y, int w, int h)
{
    int i, j;
    if (!A || w <= x || h <= y) return;
    Video_Tile8(A, 1, x * 8, y * 8);                       /* 0x8001 */
    for (i = x + 1; i < w; i++)                            /* 0x8020..0x8039 */
        Video_Tile8(A, 2, i * 8, y * 8);
    Video_Tile8(A, 3, w * 8, y * 8);                       /* 0x803B */
    for (j = y + 1; j < h; j++) {                          /* 0x8058..0x80AC */
        Video_Tile8(A, 4, x * 8, j * 8);
        for (i = x + 1; i < w; i++)
            Video_Tile8(A, 0x1B, i * 8, j * 8);
        Video_Tile8(A, 5, w * 8, j * 8);
    }
    Video_Tile8(A, 6, x * 8, h * 8);                       /* 0x80AE */
    for (i = x + 1; i < w; i++)                            /* 0x80CD..0x80E6 */
        Video_Tile8(A, 7, i * 8, h * 8);
    Video_Tile8(A, 8, w * 8, h * 8);                       /* 0x80E8 */
    /* 0x80FF..0x812B: the text window/cursor for the panel interior. */
    dd2_text_left  = x * 8 + 8;                            /* [0x9798] */
    dd2_text_right = w * 8;                                /* [0x979A] */
    dd2_text_x     = (x + 1) * 8;                          /* [0x4228] */
    dd2_text_y     = (y + 1) * 8;                          /* [0x422A] */
}

/* 0x812F -- centred rectangle (w, h) in characters.  0x812F computes
   x = 0x13 - w/2, y = 0x0B - h/2 and calls 0x7FC9(x, y, x+w+1, y+h+1): the
   panel arguments are INCLUSIVE end coordinates, so the size must be converted
   here. */
void Video_CenterPanel(const dd2_assets *A, int w, int h)
{
    int x = 20 - w / 2 - 1;                /* [0x28BC] = 0x13 */
    int y = 12 - h / 2 - 1;                /* [0x28BE] = 0x0B */
    Video_Panel(A, x, y, x + w + 1, y + h + 1);   /* 0x8170: 0x7FC9 */
}


/* ------------------------------------------------------------- font */
/* The font segment is set at startup (0x16B4): [0x9324] = 0x105E.
   Format (0x9347):
     height   = word  es:[0]
     width    = byte  es:[0x202 + ch]
     glyph    = word  es:[2 + ch*2] + (width+7)/8 * height
     bitmap   -- row by row, (width+7)/8 bytes per row, MSB = leftmost pixel.
   Verified frame by frame: the text "Copyright (c) 1991 Softdisk inc." matches
   the DOSBox-X frame re/dosbox/ref/boot_03.png. */
static int font_h(void)  { return dd2_fontseg[0] | (dd2_fontseg[1] << 8); }
static int font_w(int ch) { return dd2_fontseg[(0x202 + ch) & 0xFFF]; }
static int font_wb(int ch) { return (font_w(ch) + 7) >> 3; }

/* pixel in the plane: we write the whole colour (0 bits clear the pixel) --
   this is write-mode-0 behaviour with map mask = colour (0x9347, SEQ2=[0x4226]). */
static void screen_pixel(int x, int y, int color)
{
    int off, sh, p;
    if (x < 0 || x >= DD2_VIRTUAL_W || y < 0 || y >= DD2_H) return;
    off = dd2_base + y * dd2_stride + (x >> 3);
    if (off < 0 || off >= DD2_PLANE) return;
    sh = 7 - (x & 7);
    for (p = 0; p < 4; p++) {
        int a = p * DD2_PLANE + off;
        if ((color >> p) & 1) dd2_vram[a] |= (uint8_t)(1 << sh);
        else                  dd2_vram[a] &= (uint8_t)~(1 << sh);
    }
}

static void font_draw(int ch, int x, int y)
{
    int wb = font_wb(ch), h = font_h(), r, c, k, base;
    if (h <= 0 || h > 32 || wb <= 0 || wb > 4) return;
    base = (dd2_fontseg[2 + ch * 2] | (dd2_fontseg[3 + ch * 2] << 8)) + wb * h;
    if (base < 0 || base + wb * h > (int)sizeof dd2_fontseg) return;
    for (r = 0; r < h; r++)
        for (c = 0; c < wb; c++) {
            unsigned b = dd2_fontseg[base + r * wb + c];
            for (k = 0; k < 8; k++)
                /* Transparent: only the glyph pixels are written, the panel
                   behind them is preserved.  Verified against the DOSBox
                   reference `start_06`: the "LEVEL 1" strokes are colour 0 on
                   the white panel (a 15/0 opaque box, as an earlier port
                   version drew, does not appear there), and the score digits
                   are colour 8 on the grey panel. */
                if ((b >> (7 - k)) & 1)
                    screen_pixel(x + c * 8 + k, y + r, dd2_text_color);
        }
}

/* 0x9347: output a character: draw the glyph and advance the cursor by its width. */
void Video_PrintChar(int ch)
{
    int w;
    if (ch < 0 || ch > 255) return;
    w = font_w(ch);
    font_draw(ch, dd2_text_x, dd2_text_y);
    dd2_text_x += w;
}

/* 0x876C: output a string.
     0x0A -> [0x422A] += 10 (next line), [0x4228] = [0x9798]
     0x7F -> [0x4226] = next byte - 0x41  (colour change)
     otherwise -> 0x9347(character), which also advances [0x4228] by the width. */
void Video_PrintStr(const char *s)
{
    int ch;
    if (!s) return;
    while ((ch = (unsigned char)*s++) != 0) {
        if (ch == 0x0A) {
            dd2_text_y += 10;
            dd2_text_x = dd2_text_left;
        } else if (ch == 0x7F) {
            dd2_text_color = (unsigned char)*s++ - 0x41;
        } else {
            Video_PrintChar(ch);
        }
    }
}

/* 0x8819: string width in pixels (0x7F swallows the next byte). */
int Video_StrWidth(const char *s)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    if (!p) return 0;
    for (; *p; p++) {
        if (*p == 0x7F) { if (p[1]) p++; continue; }
        n += font_w(*p);
    }
    return n;
}

/* 0x8851: output a line centred between [0x9798] and [0x979A]. */
void Video_PrintStrC(const char *s)
{
    int w = Video_StrWidth(s);
    dd2_text_x = dd2_text_left + (dd2_text_right - dd2_text_left - w) / 2;
    Video_PrintStr(s);
}

/* ----------------------------------------------------------- attract */
/* 0x6720: STARPIC into three pages + two lines at the bottom (pages 0 and 0x4000).
   Then the title loop 0x14F8. The order is confirmed by an AVI recording in
   DOSBox-X: STARPIC (boot_03) -> TITLE1/TITLE2 with scrolling. */
void Video_AttractBegin(void)
{
    int page;
    dd2_base = 0;       Video_Fill(0, 0, 40, 200, 0);
    dd2_base = 0x4000;  Video_Fill(0, 0, 40, 200, 0);
    Video_SetWidth(DD2_STRIDE);
    Video_ShowPicAt("STARPIC.DD2", 0);
    Video_RowCopy(40, 200, 0, 0x4000);
    Video_RowCopy(40, 200, 0, 0x8000);

    dd2_text_left = 0;
    dd2_text_right = 0x140;
    for (page = 0; page <= 0x4000; page += 0x4000) {
        dd2_base = page;
        dd2_text_y = 0xb4;
        dd2_text_color = 15;
        Video_PrintStrC("Copyright (c) 1991 Softdisk inc.\n");
        Video_PrintStrC("'I' for information");
    }
    dd2_scroll = 0;
    dd2_base = 0;
}

/* 0x14F8, prologue: width, scroll to 0, base 0, 0x7938 and 0x14A0. */
void Video_TitleBegin(void)
{
    Video_SetWidth(DD2_STRIDE);
    Video_SetScroll(0);
    dd2_base = 0;
    Video_Prep();
    Video_TitlePics();
    dd2_scroll = 0;
}

/* Body of the 0x14F8 loop (one iteration = one frame):
     if (si > 0xC8 && si <= 0x118) [0x9060] += 4
     if (si > 0x190 && si <= 0x1E0) [0x9060] -= 4
     if (si > 0x1E0) si = 0
     0x90A8([0x9060]&7, [0x9060]>>3)
     si++
     [0x2FFE] = [0x9060] >> 3
   In total the cycle is exactly 481 frames: 201 frames s=0, 80 frames of
   acceleration to 320, 120 frames s=320, 80 frames of return. Verified by AVI:
   period 481 frames, static stretches of 202 and 121 frames. */
void Video_AttractFrame(void)
{
    static int si;
    if (si > 0xC8 && si <= 0x118) dd2_scroll += 4;
    if (si > 0x190 && si <= 0x1E0) dd2_scroll -= 4;
    if (si > 0x1E0) { si = 0; dd2_scroll = 0; }
    Video_SetScroll(dd2_scroll);
    si++;
    dd2_base = dd2_scroll >> 3;
}

/* ------------------------------------------- EGA -> indexed frame */
/* Displayed pixel x is pixel (dd2_scroll + x) of the wide buffer
   (start address = s>>3 bytes, pel pan = s&7). */
/* The inverse of Video_ToIndexed: load an indexed frame back into the EGA
   planes.  The in-game frame is built in `fb` (the port's own renderer), while
   the panels/text of the F-menu screens are written into the planes -- the
   original draws them straight over the video RAM that already holds the
   level, so the level stays visible around the panel.  Without this the panel
   conversion wiped the frame and the background turned black. */
void Video_FromIndexed(const uint8_t *fb, int w, int h)
{
    int x, y, p;
    if (dd2_stride <= 0) return;
    for (y = 0; y < h; y++) {
        int row = y * dd2_stride;
        for (x = 0; x < w; x++) {
            int vx = dd2_scroll + x;
            int off = row + (vx >> 3);
            int sh = 7 - (vx & 7);
            int c = fb[y * w + x];
            if (off < 0 || off >= DD2_PLANE) continue;
            for (p = 0; p < 4; p++) {
                if ((c >> p) & 1) dd2_vram[p * DD2_PLANE + off] |= (uint8_t)(1 << sh);
                else              dd2_vram[p * DD2_PLANE + off] &= (uint8_t)~(1 << sh);
            }
        }
    }
}

void Video_ToIndexed(uint8_t *fb, int w, int h)
{
    int x, y, p;
    if (dd2_stride <= 0) return;
    for (y = 0; y < h; y++) {
        int row = y * dd2_stride;
        for (x = 0; x < w; x++) {
            int vx = dd2_scroll + x;
            int off = row + (vx >> 3);
            int sh = 7 - (vx & 7);
            int c = 0;
            if (off < 0 || off >= DD2_PLANE) { fb[y * w + x] = 0; continue; }
            for (p = 0; p < 4; p++)
                c |= ((dd2_vram[p * DD2_PLANE + off] >> sh) & 1) << p;
            fb[y * w + x] = (uint8_t)c;
        }
    }
}

void Video_TitleToIndexed(uint8_t *fb, int w, int h, int wipe)
{
    (void)wipe;
    Video_Clear();
    Video_AttractBegin();
    Video_ToIndexed(fb, w, h);
}

const char *g_datadir_v = ".";
void Video_SetDataDir(const char *d) { g_datadir_v = d; }
const char *Video_DataDir(void) { return g_datadir_v; }

uint32_t Video_EGAPalette(int idx)
{
    static const uint32_t pal[16] = {
        0x000000,0x0000AA,0x00AA00,0x00AAAA,0xAA0000,0xAA00AA,0xAA5500,0xAAAAAA,
        0x555555,0x5555FF,0x55FF55,0x55FFFF,0xFF5555,0xFF55FF,0xFFFF55,0xFFFFFF };
    return pal[idx & 15];
}
