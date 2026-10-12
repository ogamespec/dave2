/*
 * Dangerous Dave in the Haunted Mansion (id Software / Softdisk, 1991)
 * SDL3 source port -- loads the ORIGINAL .DD2 data files.
 *
 * Reverse-engineered from DD2.EXE (Borland C++ 3.0, large model):
 *   - HUFF / RLEW containers              (see dd2.h comments)
 *   - EGATILES.DD2 : 858 tiles, 128 bytes each, 4 EGA planes
 *   - S_*.DD2      : uint16 plane size + 5 planes (4 colour + mask),
 *                    sprite sizes come from a table inside DD2.EXE
 *   - LEVELxx.DD2  : RLEW, header + tile plane + object-tag plane
 *   - object system: 164 objects, react-function dispatch (0x1799/0x17F4)
 */
#ifndef DD2_PORT_H
#define DD2_PORT_H

#include <stdint.h>
#include <stddef.h>

#define DD2_W 320
#define DD2_H 200

/* ---------------------------------------------------------------- data */

typedef struct {
    uint8_t  px[16 * 16];       /* palette index per pixel */
} dd2_tile;

typedef struct {
    char     name[11];
    int      w, h;
    int      off;               /* byte offset inside the plane */
    uint8_t *px;                /* w*h indexed */
    uint8_t *mask;              /* w*h, 0xFF = opaque */
    /* bounding box of the opaque pixels; the original keeps this in the
       actor-info record and uses it to place the sprite, which is why all
       animation frames of an actor must be aligned by it. */
    int      bx0, by0, bx1, by1;
    /* rec[+0x1C]/[+0x1E] -- sprite origin (1/16 px, signed).  The original
       (0x2B0C) adds it to the object position before drawing. */
    int      ox, oy;
} dd2_sprite;

/* 32-byte sprite record (phase 0) from dd2_spriteinfo -- what is read by
   0x192E: +8/+0x0A/+0x0C/+0x0E -- bbox, +0x1C/+0x1E -- origin. */
const unsigned char *dd2_sprite_record(int sprite);

/* startup log (implementation in main.c) */
void LOGF(const char *fmt, ...);

typedef struct {
    int      w, h;
    uint16_t *tiles;            /* w*h tile indices    (plane 0) */
    uint16_t *objs;             /* w*h object tags     (plane 1) */
    uint16_t  plane_size;
    char      name[32];
} dd2_level;

/* actor info record (20 bytes in DGROUP), see dd2_actorinfo.h */
typedef struct {
    uint16_t spr_l, spr_r;
    uint16_t flags;
    uint16_t p6, p8, p10;
    uint16_t p12, p14;
    uint16_t react;
    uint16_t next;
} dd2_actorinfo;

typedef struct {
    dd2_tile    tiles[1024];
    int         ntiles;
    /* per-tile behaviour, straight from the EXE tables (see dd2_tileattr.h) */
    uint8_t     north[1024];    /* solid when moving down  (floor)   */
    uint8_t     south[1024];    /* solid when moving up    (ceiling) */
    uint8_t     east[1024];     /* solid when moving right           */
    uint8_t     west[1024];     /* solid when moving left            */
    uint8_t     intile[1024];   /* tile type: 1/2 doors, 13 deadly, 99 exit */

    dd2_sprite *sprites;
    int         nsprites;

    dd2_actorinfo *info;        /* actor info records */
    int            ninfo;

    dd2_level   level;
    int         have_level;
} dd2_assets;

/* ------------------------------------------------------------- loading */
int  dd2_load_assets(dd2_assets *a, const char *datadir, const char *exe);
int  dd2_load_level(dd2_assets *a, const char *datadir, int levelno);

/* ------------------------------------------------------------- utils  */
void *dd2_read_file(const char *path, size_t *len);
uint8_t *dd2_huff_expand(const uint8_t *src, size_t len, size_t *outlen);
uint8_t *dd2_rlew_expand(const uint8_t *src, size_t len, size_t *outlen);
uint8_t *dd2_load_any(const char *path, size_t *outlen);
uint8_t *dd2_rle_expand(const uint8_t *src, size_t len, size_t *outlen);  /* 0x7BF4 */
uint8_t *dd2_load_pic(const char *path, size_t *outlen);

#endif /* DD2_PORT_H */
