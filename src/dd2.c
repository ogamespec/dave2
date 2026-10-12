/* Data-file decoding for Dangerous Dave 2 -- see dd2.h */
#include "dd2.h"
#include "dd2_actorinfo.h"
#include "dd2_tileattr.h"
#include "dd2_sprtable.h"
#include "dd2_spritedata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ io */

/* 0x7637 / 0x7773: raw resource loader (fopen/fread wrapper in the original).
   The DOS file-name mangling and the far-heap allocation are replaced by libc. */
void *dd2_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = malloc((size_t)n + 16);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    if (len) *len = (size_t)n;
    return buf;
}

/* 0x7948: HUFF expander.
 * "HUFF" + uint32 expanded size + 1020-byte tree (255 nodes x 2 words) +
 * LSB-first bit stream.  The decoder must reload the next control word
 * *before* reading the literal byte it refers to (that is how LZEXE-era
 * DD2 files were written) -- see RE notes. */
uint8_t *dd2_huff_expand(const uint8_t *src, size_t len, size_t *outlen)
{
    uint32_t size;
    uint16_t lut[510];
    uint8_t *out;
    size_t sp, dst = 0;
    unsigned node = 254;
    unsigned w;
    int cnt = 16;

    if (len < 8 + 1020 || memcmp(src, "HUFF", 4) != 0) return NULL;
    memcpy(&size, src + 4, 4);
    if (size == 0 || size > (32u << 20)) return NULL;
    memcpy(lut, src + 8, sizeof lut);

    sp = 8 + 1020;
    w = src[sp] | (src[sp + 1] << 8);
    sp += 2;
    out = malloc(size + 16);
    if (!out) return NULL;

    while (dst < size) {
        unsigned b = w & 1;
        unsigned dx;
        if (--cnt == 0) { w = src[sp] | (src[sp + 1] << 8); sp += 2; cnt = 16; }
        else w >>= 1;
        dx = lut[node * 2 + (b ? 1 : 0)];
        if (dx >= 0x100) node = dx - 0x100;
        else { out[dst++] = (uint8_t)dx; node = 254; }
    }
    if (outlen) *outlen = dst;
    return out;
}

/* 0x7B5B: word-RLE expander. */
uint8_t *dd2_rlew_expand(const uint8_t *src, size_t len, size_t *outlen)
{
    uint32_t size;
    uint8_t *out;
    size_t p = 4, dst = 0;

    if (len < 4) return NULL;
    memcpy(&size, src, 4);
    if (size == 0 || size > (16u << 20)) return NULL;
    out = malloc(size + 16);
    if (!out) return NULL;
    memcpy(out, src, 4);
    dst = 4;
    while (dst < size && p + 3 < len) {
        uint16_t w = (uint16_t)(src[p] | (src[p + 1] << 8));
        p += 2;
        if (w == 0xFEFE) {
            uint16_t cnt, val;
            if (p + 3 >= len) break;
            cnt = (uint16_t)(src[p] | (src[p + 1] << 8)); p += 2;
            val = (uint16_t)(src[p] | (src[p + 1] << 8)); p += 2;
            /* no clamping: an RLE run may overshoot the nominal size, and the
               original decoder relies on that (the level header's own size
               field excludes the 4-byte prefix). */
            while (cnt--) {
                if (dst + 1 >= size + 8) break;
                out[dst++] = (uint8_t)(val & 0xFF);
                out[dst++] = (uint8_t)(val >> 8);
            }
        } else {
            out[dst++] = (uint8_t)(w & 0xFF);
            out[dst++] = (uint8_t)(w >> 8);
        }
    }
    if (outlen) *outlen = dst;
    return out;
}

/* 0x7BF4: byte-wise RLE of the "second branch" of the picture loader
   (0x77FC, called when [0x97A6] == 0). Format:
       uint32 unpacked size
       then the stream:  0xFE <count> <value>  -> count bytes of value
                         otherwise             -> a literal byte
   This is exactly how TITLE2.DD2 of the November version is stored:
       08 7d 00 00 "PIC\0" 28 00 c8 00 FE 28 55 FE 28 AA ... */
uint8_t *dd2_rle_expand(const uint8_t *src, size_t len, size_t *outlen)
{
    uint32_t size;
    uint8_t *out;
    size_t p = 4, dst = 0;

    if (len < 5) return NULL;
    memcpy(&size, src, 4);
    if (size == 0 || size > (32u << 20)) return NULL;
    out = malloc((size_t)size + 16);
    if (!out) return NULL;
    while (dst < size && p < len) {
        uint8_t c = src[p++];
        if (c != 0xFE) { out[dst++] = c; continue; }
        if (p + 1 >= len) break;
        {
            unsigned cnt = src[p++];
            uint8_t val = src[p++];
            while (cnt-- && dst < size) out[dst++] = val;
        }
    }
    if (outlen) *outlen = dst;
    return out;
}

/* Pictures ("PIC\0") are loaded through three different paths, depending on the
   game version and the loader branch ([0x97A6] in the original):
     HUFF -> "PIC\0"                 (TITLE1/STARPIC/PROGPIC)
     0x7BF4: uint32 + RLE 0xFE       (TITLE2 of the November version, 0x77FC)
     RLEW  -> 4-byte size + PIC      (just in case)
   We try them in turn and accept only what actually starts with the PIC
   signature. */
uint8_t *dd2_load_pic(const char *path, size_t *outlen)
{
    size_t len = 0, l2 = 0;
    uint8_t *raw = dd2_read_file(path, &len), *e;

    if (!raw) return NULL;
    if (len >= 4 && memcmp(raw, "HUFF", 4) == 0) {
        e = dd2_huff_expand(raw, len, &l2);
        if (e && l2 >= 8 && memcmp(e, "PIC\0", 4) == 0) {
            free(raw); if (outlen) *outlen = l2; return e;
        }
        free(e);
    }
    e = dd2_rle_expand(raw, len, &l2);
    if (e && l2 >= 8 && memcmp(e, "PIC\0", 4) == 0) {
        free(raw); if (outlen) *outlen = l2; return e;
    }
    free(e);
    e = dd2_rlew_expand(raw, len, &l2);
    if (e && l2 >= 8 && memcmp(e, "PIC\0", 4) != 0 && l2 >= 12 && memcmp(e + 4, "PIC\0", 4) == 0) {
        memmove(e, e + 4, l2 - 4);
        l2 -= 4;
    }
    if (e && l2 >= 8 && memcmp(e, "PIC\0", 4) == 0) {
        free(raw); if (outlen) *outlen = l2; return e;
    }
    free(e);
    if (len >= 8 && memcmp(raw, "PIC\0", 4) == 0) {
        if (outlen) *outlen = len;
        return raw;
    }
    free(raw);
    return NULL;
}

/* 0x7637 (second half): read a file and expand it by its header (HUFF/RLEW/RLE). */
uint8_t *dd2_load_any(const char *path, size_t *outlen)
{
    size_t len;
    uint8_t *raw = dd2_read_file(path, &len);
    uint8_t *res;
    if (!raw) return NULL;
    if (len >= 4 && memcmp(raw, "HUFF", 4) == 0) {
        res = dd2_huff_expand(raw, len, outlen);
        free(raw);
        return res;
    }
    res = dd2_rlew_expand(raw, len, outlen);
    if (!res) {                     /* not compressed: hand back the raw file */
        res = malloc(len);
        if (res) memcpy(res, raw, len);
        if (outlen) *outlen = len;
    }
    free(raw);
    return res;
}

/* --------------------------------------------------------------- tiles */
static void decode_ega_plane(const uint8_t *data, int w, int h, int stride,
                             int off, uint8_t *px, int pxstride)
{
    int row = w >> 3, y, bx, k;
    for (y = 0; y < h; y++) {
        for (bx = 0; bx < row; bx++) {
            int i = off + y * row + bx;
            uint8_t b0 = data[i], b1 = data[i + stride], b2 = data[i + 2 * stride],
                    b3 = data[i + 3 * stride];
            for (k = 0; k < 8; k++) {
                int sh = 7 - k;
                px[y * pxstride + bx * 8 + k] =
                    (uint8_t)(((b3 >> sh) & 1) << 3 | ((b2 >> sh) & 1) << 2 |
                              ((b1 >> sh) & 1) << 1 | ((b0 >> sh) & 1));
            }
        }
    }
}

/* ------------------------------------------------------------- sprites */
/* Sprite dimensions live in a table inside DD2.EXE: 4 records of 32 bytes
 * per sprite, 10-char name at +8 of each record, width/8 at +24 and height
 * at +26 of the 4th record.  Sprite data itself is grouped by "END..."
 * marker sprites into the five S_*.DD2 chunks. */
/* The sprite size table only exists inside the game executable, so try the
   usual names first and fall back to the copy embedded in dd2_sprtable.h --
   that way the port still runs when the EXE has been renamed or replaced. */
/* Everything the port needs from the original DOS executable is compiled in
   (see dd2_sprtable.h / dd2_actorinfo.h / dd2_tileattr.h), so the game data
   directory only has to contain the .DD2 files. */
static void use_embedded_sprtable(dd2_sprite *sp)
{
    int i, cum = 0;
    /* Chunk boundaries (DGROUP 0xD2 / 0xE6): 0..84, 85..120, 121..173,
       174..186, 187..198.  A sprite offset is counted FROM THE START OF ITS
       OWN CHUNK: each S_*.DD2 is a separate file with its own five planes,
       and an absolute prefix sum over all 199 sprites would read data
       far beyond the buffer (that is what happened for 114 of the 199 sprites). */
    static const int chunk_start[5] = { 0, 85, 121, 174, 187 };
    int ci;
    /* The sizes and offsets come NOT from the size table but from the 32-byte
       sprite records -- exactly what the original loader 0xA80 does:

           size = rec[s*4+0][+0x1A] = rec[+0]*rec[+2]      (bytes per plane)
           [0x9EB6] -- the record table, base 0x12490

       A sprite's offset in the plane is the prefix sum of these sizes over the
       sprites.  The size table (dd2_sprtable.h) gives wrong values for the
       marker sprites: for STARTDAVE the record says 8x8, while the size table
       says 24x32; because of that the offsets drifted (AMMO0 ended up at
       1808 instead of 1736, and the sprite decoded as garbage). */
    for (i = 0; i < 199; i++) {
        const unsigned char *r = dd2_spriteinfo + (size_t)i * 4 * DD2_SPRITEINFO_REC;
        for (ci = 0; ci < 5; ci++) if (i == chunk_start[ci]) cum = 0;
        int row = r[0] | (r[1] << 8);      /* +0x00: bytes per row of phase 0 */
        int hgt = r[2] | (r[3] << 8);      /* +0x02: height in pixels */
        snprintf(sp[i].name, sizeof sp[i].name, "%s", dd2_sprtable[i].name);
        if (row > 0 && hgt > 0) {
            sp[i].w = row * 8;
            sp[i].h = hgt;
        } else {
            sp[i].w = dd2_sprtable[i].w ? dd2_sprtable[i].w : 8;
            sp[i].h = dd2_sprtable[i].h ? dd2_sprtable[i].h : 8;
        }
        sp[i].ox = (int16_t)(r[0x1C] | (r[0x1D] << 8));
        sp[i].oy = (int16_t)(r[0x1E] | (r[0x1F] << 8));
        sp[i].off = cum;
        cum += sp[i].w / 8 * sp[i].h;
    }
}

const unsigned char *dd2_sprite_record(int sprite)
{
    if (sprite < 0 || sprite >= 199) return 0;
    return dd2_spriteinfo + (size_t)sprite * 4 * DD2_SPRITEINFO_REC;   /* phase 0 */
}

/* 0x7A3D: fix up the sprite table pointers (the original patches far pointers;
   the port uses the table embedded from the EXE instead). */
static int load_sprite_table(const char *exe, dd2_sprite **out, int *count)
{
    dd2_sprite *sp = calloc(199, sizeof *sp);
    (void)exe;
    if (!sp) return 0;
    use_embedded_sprtable(sp);
    *out = sp;
    *count = 199;
    return 1;
}


/* 0x09E3: sprite chunk loader -- HUFF payload = uint16 plane_size + 5 planes
   (4 colour + mask), sprites packed back to back in each plane. */
static int decode_sprite_chunk(const char *path, dd2_sprite *sp, int first, int count)
{
    size_t len;
    uint8_t *e = dd2_load_any(path, &len), *body;
    int plane, i, off = 0;

    if (!e) return 0;
    if (len < 3) { free(e); return 0; }
    {
        int plane_size = e[0] | (e[1] << 8);
        body = e + 2;
        for (i = 0; i < count; i++) {
            dd2_sprite *s = &sp[first + i];
            int row = s->w >> 3, size = row * s->h, y, bx, k;
            off = s->off;               /* the offset has already been computed from the records */
            s->px = calloc((size_t)s->w * s->h, 1);
            s->mask = calloc((size_t)s->w * s->h, 1);
            for (y = 0; y < s->h; y++) {
                for (bx = 0; bx < row; bx++) {
                    int idx = off + y * row + bx;
                    uint8_t c[4], m;
                    for (plane = 0; plane < 4; plane++)
                        c[plane] = body[plane * plane_size + idx];
                    m = body[4 * plane_size + idx];
                    for (k = 0; k < 8; k++) {
                        int sh = 7 - k, o = y * s->w + bx * 8 + k;
                        s->px[o] = (uint8_t)(((c[3] >> sh) & 1) << 3 |
                                             ((c[2] >> sh) & 1) << 2 |
                                             ((c[1] >> sh) & 1) << 1 |
                                             ((c[0] >> sh) & 1));
                        /* mask bit 0 = opaque (the sprite stays visible there) */
                        s->mask[o] = ((m >> sh) & 1) ? 0 : 0xFF;
                    }
                }
            }
            {
                int x, y2, x0 = s->w, y0 = s->h, x1 = -1, y1 = -1;
                for (y2 = 0; y2 < s->h; y2++)
                    for (x = 0; x < s->w; x++)
                        if (s->mask[y2 * s->w + x]) {
                            if (x < x0) x0 = x;
                            if (x > x1) x1 = x;
                            if (y2 < y0) y0 = y2;
                            if (y2 > y1) y1 = y2;
                        }
                if (x1 < 0) { x0 = 0; y0 = 0; x1 = s->w - 1; y1 = s->h - 1; }
                s->bx0 = x0; s->by0 = y0; s->bx1 = x1; s->by1 = y1;
            }
            off += size;
        }
        (void)plane_size;
    }
    free(e);
    return 1;
}

/* ------------------------------------------------------------ collision */
/* Directional solidity tables + tile type table, copied verbatim from the
   EXE (DS:0x12D0 header -> six 858-byte arrays, see dd2_tileattr.h).
   The original resolves collisions by pushing out per axis (0x19EE / 0x1AC9)
   and tests the tile *type* byte for doors/death/exit (0x4422 / 0x39B1). */
static void rebuild_attributes(dd2_assets *a)
{
    int i;
    for (i = 0; i < 1024; i++) {
        a->north[i]  = (i < 858) ? dd2_tile_north[i]  : 0;
        a->south[i]  = (i < 858) ? dd2_tile_south[i]  : 0;
        a->east[i]   = (i < 858) ? dd2_tile_east[i]   : 0;
        a->west[i]   = (i < 858) ? dd2_tile_west[i]   : 0;
        a->intile[i] = (i < 858) ? dd2_tile_intile[i] : 0;
    }
}

/* -------------------------------------------------------------- levels */
/* 0x0DAD: level loader -- RLEW-expand LEVELnn.DD2, build the plane pointer
   table ([0xA6CA] + i*4) and the used-tile map (DGROUP 0x4DEE).  The port
   preloads all 858 tiles, so the used-tile map is not reproduced. */
int dd2_load_level(dd2_assets *a, const char *datadir, int levelno)
{
    char path[512];
    size_t len;
    uint8_t *d;
    int w, h, i;

    snprintf(path, sizeof path, "%s/LEVEL%02d.DD2", datadir, levelno);
    d = dd2_load_any(path, &len);
    if (!d) {
        snprintf(path, sizeof path, "%s/LEVEL%02d.dd2", datadir, levelno);
        d = dd2_load_any(path, &len);
    }
    if (!d) return 0;
    if (len < 0x24) { free(d); return 0; }
    w = d[4] | (d[5] << 8);
    h = d[6] | (d[7] << 8);
    /* Header +0x12 is the plane stride the original uses to find the tag plane
       (0x0E8C-0x0EB3).  It equals w*h*2 for seven levels, but LEVEL06 has
       0x37D0 vs 0x37C8, so hard-coding w*h*2 read the tags 4 cells early. */
    {
        size_t stride = (size_t)(d[0x12] | (d[0x13] << 8));
        if (stride < (size_t)w * h * 2) stride = (size_t)w * h * 2;
        if (w <= 0 || h <= 0 || 0x24 + stride + (size_t)w * h * 2 > len + 8) {
            free(d); return 0;
        }
        free(a->level.tiles); free(a->level.objs);
        a->level.w = w; a->level.h = h;
        a->level.tiles = malloc((size_t)w * h * 2);
        a->level.objs = malloc((size_t)w * h * 2);
        for (i = 0; i < w * h; i++) {
            a->level.tiles[i] = (uint16_t)(d[0x24 + i * 2] | (d[0x24 + i * 2 + 1] << 8));
            a->level.objs[i] = (uint16_t)(d[0x24 + stride + i * 2] |
                                          (d[0x24 + stride + i * 2 + 1] << 8));
        }
        a->level.plane_size = (uint16_t)stride;
    }
    snprintf(a->level.name, sizeof a->level.name, "LEVEL%02d", levelno);
    a->have_level = 1;
    free(d);
    rebuild_attributes(a);
    return 1;
}

/* --------------------------------------------------------------- assets */
int dd2_load_assets(dd2_assets *a, const char *datadir, const char *exe)
{
    char path[512];
    size_t len;
    uint8_t *t;
    int i;

    memset(a, 0, sizeof *a);

    /* tiles ------------------------------------------------------------ */
    snprintf(path, sizeof path, "%s/EGATILES.DD2", datadir);
    t = dd2_read_file(path, &len);
    if (!t) {
        snprintf(path, sizeof path, "%s/EGATILES.dd2", datadir);
        t = dd2_read_file(path, &len);
    }
    if (!t) { LOGF("  tiles: not found"); return 0; }
    LOGF("  tiles: %zu bytes", len);
    a->ntiles = (int)(len / 128);
    if (a->ntiles > 1024) a->ntiles = 1024;
    for (i = 0; i < a->ntiles; i++)
        decode_ega_plane(t + i * 128, 16, 16, 32, 0, a->tiles[i].px, 16);

    LOGF("  tiles decoded: %d", a->ntiles);
    /* sprite dimensions out of the original executable ------------------ */
    if (!load_sprite_table(exe, &a->sprites, &a->nsprites)) {
        fprintf(stderr, "dd2: sprite table not found in '%s'\n", exe);
        /* fall back: assume 8x8 for everything */
        a->sprites = calloc(199, sizeof *a->sprites);
        for (i = 0; i < 199; i++) { a->sprites[i].w = 8; a->sprites[i].h = 8; }
        a->nsprites = 199;
    }
    LOGF("  sprite table ok: %d", a->nsprites);
    /* chunk boundaries: cumulative (w/8)*h equals each chunk's plane size */
    {
        static const char *files[5] = { "S_DAVE.DD2", "S_CHUNK1.DD2", "S_CHUNK2.DD2",
                                        "S_FRANK.DD2", "S_MASTER.DD2" };
        int idx = 0, f;
        for (f = 0; f < 5; f++) {
            size_t total = 0;
            int start = idx;
            /* the boundary is known from the sprite table sums: recompute it */
            snprintf(path, sizeof path, "%s/%s", datadir, files[f]);
            LOGF("  chunk %d: %s", f, files[f]);
            {
                size_t l2;
                uint8_t *e = dd2_load_any(path, &l2);
                int plane_size = e ? (e[0] | (e[1] << 8)) : 0;
                free(e);
                while (idx < 199) {
                    int sz = (a->sprites[idx].w >> 3) * a->sprites[idx].h;
                    total += sz;
                    idx++;
                    if ((int)total >= plane_size) break;
                }
                LOGF("    plane=%d sprites %d..%d", plane_size, start, idx - 1);
                if (plane_size)
                    decode_sprite_chunk(path, a->sprites, start, idx - start);
            }
        }
    }

    /* actor info records (extracted from DGROUP 0x9d8..0x1270) ---------- */
    a->ninfo = DD2_ACTORINFO_LEN / 20;
    a->info = calloc(a->ninfo, sizeof *a->info);
    for (i = 0; i < a->ninfo; i++) {
        const unsigned char *r = dd2_actorinfo_blob + i * 20;
        dd2_actorinfo *ai = &a->info[i];
        ai->spr_l = (uint16_t)(r[0] | (r[1] << 8));
        ai->spr_r = (uint16_t)(r[2] | (r[3] << 8));
        ai->flags = (uint16_t)(r[4] | (r[5] << 8));
        ai->p6 = (uint16_t)(r[6] | (r[7] << 8));
        ai->p8 = (uint16_t)(r[8] | (r[9] << 8));
        ai->p10 = (uint16_t)(r[10] | (r[11] << 8));
        ai->p12 = (uint16_t)(r[12] | (r[13] << 8));
        ai->p14 = (uint16_t)(r[14] | (r[15] << 8));
        ai->react = (uint16_t)(r[16] | (r[17] << 8));
        ai->next = (uint16_t)(r[18] | (r[19] << 8));
    }
    free(t);
    return 1;
}
