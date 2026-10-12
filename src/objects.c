/*
 * objects.c -- DD2 object system (core).
 *
 * Original:
 *   0x1799  Obj_Alloc()   -- find a free slot in the 164x90 array (DS:0x5264),
 *                            counter [0x8BCA], cursor [0x8BBE], end [0x8BCC]
 *   0x17F4  Obj_Spawn(type,x,y,dir,info)
 *                            obj->sprite = info[dir > 0 ? 2 : 0]   (sprite L/R)
 *                            obj->react  = info[0x10]
 *                            obj->info   = info
 *   0x1F0D  copy of the object into scratch DS:0x8BD4 (90 bytes)
 *   0x2047  call word ptr [0x8C2C] -- call the react function ("method")
 *   0x1E96  copy of scratch back into the object
 * Reactions are offsets (in the original -- addresses) of functions that work
 * on the object through the scratch copy.  The same mechanism is used here.
 */
#include "objects.h"
#include "dd2.h"
#include "dd2_actorinfo.h"
#include <string.h>

dd2_obj dd2_objs[DD2_MAXOBJ];      /* DS:0x5264 */
dd2_obj dd2_cur;                   /* DS:0x8BD4 */
/* --- declarations needed before the definitions ---------------------- */
extern int dd2_rand_byte(void);          /* 0x8EAF, implemented in level.c */
extern void (*dd2_sfx)(int id);          /* 0x8CB2 -- sound, defined in dave.c */
void (*dd2_extra_sprite)(int spr, int wx, int wy);  /* 0x2B0C from the handler */
int dd2_lives;                           /* [0x8BC8] -- number of lives */
extern int dd2_door_flag;                /* [0x8C38] -- single exit/hit flag (level.c) */
extern int dd2_god_mode;                 /* [0x4A8C] -- F12 menu toggle (dave.c) */
void Obj_SetState(dd2_obj *o, int rec);
void Obj_Box(dd2_obj *o);
int  Obj_PushOutX(dd2_obj *o, const uint16_t *tiles, const uint8_t *east,
                  const uint8_t *west, int w, int h);

int     dd2_obj_count;             /* [0x8BCA] */
int     dd2_update_calls, dd2_visible_calls;   /* debug */
int     dd2_obj_cursor;            /* [0x8BBE] */
/* The object slot whose reaction is running.  The original's sprite draws
   (0x2B0C) are queued from the REACTIONS, and 0x200E-0x2062 walks them BACK TO
   FRONT, so the queue -- and therefore the stacking -- is reverse slot order:
   Dave (slot 0) is queued last and ends up on top.  The renderer must draw in
   that same order. */
int     dd2_react_slot = -1;

/* 0x1799: find a free slot.  The original starts at slot 1 (DS:0x5264); slot 0
   belongs to the player (written directly by 0x3741), and the object count is
   the high-water mark, not the number of live objects. */
int Obj_Alloc(void)
{
    int i;
    for (i = 1; i < DD2_MAXOBJ; i++)
        if (dd2_objs[i].type == 0) {
            memset(&dd2_objs[i], 0, sizeof dd2_objs[i]);
            if (i >= dd2_obj_count) dd2_obj_count = i + 1;
            return i;
        }
    return -1;
}

/* 0x17F4: create an object. info -- offset of the 20-byte actor-info record. */
int Obj_Spawn(int type, int x, int y, int dir, int info)
{
    int slot = Obj_Alloc();
    dd2_obj *o;
    const unsigned char *ai;
    if (slot < 0) return -1;
    o = &dd2_objs[slot];
    o->type   = (int16_t)type;
    o->x      = (int16_t)x;
    o->y      = (int16_t)y;
    o->dir    = (int16_t)dir;
    o->info   = (int16_t)info;
    if (info >= DD2_ACTORINFO_BASE &&
        info + 20 <= DD2_ACTORINFO_BASE + DD2_ACTORINFO_LEN) {
        ai = dd2_actorinfo_blob + (info - DD2_ACTORINFO_BASE);
        o->sprite = (int16_t)(ai[dir > 0 ? 2 : 0] | (ai[dir > 0 ? 3 : 1] << 8));
        o->react  = (int16_t)(ai[0x10] | (ai[0x11] << 8));
    }
    Obj_Box(o);            /* 0x192E: every object has a bbox at once */
    o->f22 = o->tx_l; o->f24 = o->tx_t;   /* edges before the move, otherwise the */
    o->f26 = o->tx_r; o->f28 = o->tx_b;   /* first frame would scan from zero */
    return slot;
}

/* ---- frame globals, as in the original (DS addresses in comments) ---- */
int dd2_frame_delta;        /* [0x8C6A] -- frame delta (in units of 2 ticks) */
/* [0x8C14] and [0x8C16] are fields of the SCRATCH OBJECT (+0x40 and +0x42),
   not globals: each object has its own delay accumulator.  While they were
   static, with dozens of objects in a frame the shared counter went negative
   (each object subtracted its own delay) and nobody was updated at all --
   that is exactly why the enemies stood still. */
#define S_ACC(o)       ((o)->f40)   /* [0x8C14] */
#define S_CALLDELAY(o) ((o)->f42)   /* [0x8C16] */

/* [0x8C0E]/[0x8C10] -- direction signs for the xspeed/yspeed of the record.
   They are set by the per-frame functions themselves (for Dave --
   0x2E07/0x3B6F/0x3DE0); 0x1D76 multiplies by them before the function is
   called, so the value from the PREVIOUS frame takes effect -- as in the original. */
int dd2_dir_x = 1;
int dd2_dir_y = 1;

/* Equivalent of `call word ptr [bx+0xC]`: the per-frame function of the current record. */
void (*dd2_obj_perframe)(dd2_obj *o);

/* 0x1DBC: copy of 16 bytes of the object's "previous frame" (0x8BDE <- 0x8BEE) --
   the movement delta is computed from it later (0x2585). */
int16_t dd2_prev_frame[8];
int16_t dd2_cur_frame[8];

/* 0x1E4F: the record has no next -- the object "played out" its animation. */
int dd2_chain_end;
int16_t dd2_xmove, dd2_ymove;   /* [0x8BFE], [0x8C00] */

/* 0x1870 (helper): saves the CURRENT left>>8/right>>8 into +0x22/+0x26 --
   they are later compared by 0x1AC9 in order to scan only the new columns.
   Called before the movement is applied (right after 0x192E in 0x1D76). */
static void SaveOldEdges(dd2_obj *o)
{
    o->f22 = o->tx_l;      /* +0x22 <- left>>8   (for 0x1AC9) */
    o->f24 = o->tx_t;      /* +0x24 <- top>>8    (for 0x19EE) */
    o->f26 = o->tx_r;      /* +0x26 <- right>>8  (for 0x1AC9) */
    o->f28 = o->tx_b;      /* +0x28 <- bottom>>8 (for 0x19EE) */
}

/* 0x1AC9 is hooked up from outside: it needs the tile tables from the assets. */
int (*dd2_pushout)(dd2_obj *o);

/* -------------------------------------------------------------------------
 * 0x4D13 -- skull (SKULLFIREL): a HOMING projectile.  Each tick of the delta
 * the acceleration accumulators shift toward Dave and are clamped to +-4, and
 * the accumulated value is added to the movement:
 *
 *   di = (dave.x > x) ? 1 : -1                 ; along X toward Dave
 *   ax = y - 0xC0
 *   dx = (ax < dave.y) ? 1 : -1                ; along Y toward Dave
 *   for each tick:
 *       [+0x2E] += di ; clamp [-4,4] ; xmove += [+0x2E]
 *       [+0x30] += dx ; clamp [-4,4] ; ymove += [+0x30]
 *   dir_x = ([+0x2E] > 0) ? 1 : -1
 *
 * CAUTION about the fields: in the original +0x2E/+0x30 are the accumulators,
 * while xmove/ymove live in +0x2A/+0x2C.  In the port xmove/ymove occupy
 * +0x2E/+0x30, so the free f1c and f1e were taken for the accumulators
 * (verified: 0 uses).
 * ---------------------------------------------------------------------- */
void Obj_SkullAI(dd2_obj *o, dd2_obj *dave)
{
    int di, dx, si;
    if (!o || !dave) return;

    di = (dave->x > o->x) ? 1 : -1;                 /* 0x4D15 */
    dx = ((o->y - 0xC0) < dave->y) ? 1 : -1;        /* 0x4D28 */

    for (si = 0; si < (int)dd2_frame_delta; si++) { /* 0x4D40 */
        o->f1c = (int16_t)(o->f1c + di);            /* 0x4D42 */
        if (o->f1c < -4) o->f1c = -4;
        else if (o->f1c > 4) o->f1c = 4;
        dd2_xmove = (int16_t)(dd2_xmove + o->f1c);  /* 0x4D62 */

        o->f1e = (int16_t)(o->f1e + dx);            /* 0x4D6B */
        if (o->f1e < -4) o->f1e = -4;
        else if (o->f1e > 4) o->f1e = 4;
        dd2_ymove = (int16_t)(dd2_ymove + o->f1e);  /* 0x4D8B */
    }
    dd2_dir_x = (o->f1c > 0) ? 1 : -1;              /* 0x4D99 */
}

/* -------------------------------------------------------------------------
 * 0x19C0 -- GRAVITY (called, for example, by 0x4C2C = wolf jump):
 *     for each tick of the delta: accumulator [+0x30] += 3 (at most 0x50),
 *     then [0x8C00] (ymove) += accumulator.
 * The accumulator is an object field, not a global: each one has its own fall
 * speed.  In the port the unused f2c (+0x2C) is reserved for it.
 * ---------------------------------------------------------------------- */
void Obj_Gravity(dd2_obj *o)
{
    int si;
    if (!o) return;
    for (si = 0; si < (int)dd2_frame_delta; si++) {      /* 0x19C3 */
        o->f2c = (int16_t)(o->f2c + 3);                  /* 0x19C5 */
        if (o->f2c > 0x50) o->f2c = 0x50;                /* 0x19CA */
        dd2_ymove = (int16_t)(dd2_ymove + o->f2c);       /* 0x19D7 */
    }
}

/* -------------------------------------------------------------------------
 * 0x4C2C -- wolf jump (WOLFJUMPL): just gravity.
 * ---------------------------------------------------------------------- */
void Obj_WolfJump(dd2_obj *o)      { Obj_Gravity(o); }

/* -------------------------------------------------------------------------
 * 0x4C30 -- wolf at walls and on the floor (second half of the WOLFJUMPL record).
 *
 *   call 0x19E7                              ; push out X and Y
 *   if hit on the left/right:  [0x8C16] = 0 ; [0x8C26] = 1
 *   if NO hit below -> to the timer (0x4C9B)
 *   [0x8C16] = 7
 *   if [0x8C26] != 0:  dir_x = -dir_x ; [0x8C16] = 0 ; [0x8C26] = 0
 *   SetState(0x0F64)
 *   frame copy (memcpy 0x8BDE <- 0x8BEE, 0x10)
 *   SetState(0x0F64)
 *   Obj_Box()
 *   call 0x19E7                              ; push out once more
 *   0x4C9B: if [+0x46] != 0: -= delta, not below 0
 *
 * [0x8C16] -- field +0x42 (f42 in the port), [0x8C26] -- +0x52 (f48[5]).
 * ---------------------------------------------------------------------- */
void Obj_WolfBounce(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);        /* 0x4C30: call 0x19E7 */
    Obj_PushOutY(o, tiles, north, south, w, h);

    if (o->hit_left || o->hit_right) {               /* 0x4C33 */
        o->f42 = 0;
        o->f48[5] = 1;
    }
    if (o->hit_below) {                              /* 0x4C4D */
        o->f42 = 7;                                  /* 0x4C54 */
        if (o->f48[5]) {                             /* 0x4C5A */
            dd2_dir_x = -dd2_dir_x;                  /* 0x4C61 */
            o->f42 = 0;
            o->f48[5] = 0;
        }
        Obj_SetState(o, 0x0F64);                     /* 0x4C71 */
        memcpy(dd2_prev_frame, dd2_cur_frame, 16);   /* 0x4C7A */
        Obj_SetState(o, 0x0F64);                     /* 0x4C8C */
        Obj_Box(o);                                  /* 0x4C95 */
        Obj_PushOutX(o, tiles, east, west, w, h);    /* 0x4C98 */
        Obj_PushOutY(o, tiles, north, south, w, h);
    }
    if (o->timer != 0) {                             /* 0x4C9B: [+0x46] */
        o->timer = (int16_t)(o->timer - (int16_t)dd2_frame_delta);
        if (o->timer < 0) o->timer = 0;
    }
    Obj_OffscreenCheck(o);                           /* 0x4CDC: call 0x30C0 */
}

/* -------------------------------------------------------------------------
 * 0x2E55 -- reaction of Dave's record 31 (doors, crosshair, shot, score):
 *     draw(0x2B0C) ; 0x30C0 (remove if it went off screen)
 * The central renderer does the drawing, so only 0x30C0 is left here.
 * ---------------------------------------------------------------------- */
void Obj_DrawCullReact(dd2_obj *o)      { Obj_OffscreenCheck(o); }

/* 0x2E6B -- MASTER/MASTCLAW reaction: timer [+0x46] and off-screen removal.
   The central renderer does the drawing. */
void Obj_TimerCullReact(dd2_obj *o)
{
    if (!o) return;
    if (o->timer != 0) {                                  /* 0x2E6B */
        o->timer = (int16_t)(o->timer - (int16_t)dd2_frame_delta);
        if (o->timer < 0) o->timer = 0;
    }
    Obj_OffscreenCheck(o);                                /* 0x2EAC: call 0x30C0 */
}

/* 0x2FF6 -- SLIME/SPIDER reaction: push-out, then look at the tile row just
   below the object's bottom edge (row tx_b - 1, column at the leading edge,
   SOUTH table) and turn around when it is empty or a wall is ahead. */
void Obj_SlimeSpiderReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *south,
                          const uint8_t *north, const uint8_t *east,
                          const uint8_t *west, int w, int h)
{
    int row, col, tile, type;
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);             /* 0x2FF7: call 0x19E7 */
    Obj_PushOutY(o, tiles, north, south, w, h);

    row = o->tx_b - 1;                                    /* 0x3001 / 0x300F: [0x8BEC]-1 */
    col = (dd2_dir_x > 0) ? o->tx_r : o->tx_l;
    tile = 0;
    if (row >= 0 && row < h && col >= 0 && col < w)
        tile = tiles[row * w + col];
    type = (tile <= 857) ? south[tile] : 0;               /* 0x4A94 */

    if (type == 0 ||                                       /* 0x3031-0x3033: empty ahead */
        (o->hit_left && dd2_dir_x == -1) ||               /* 0x3035 / 0x3043 */
        (o->hit_right && dd2_dir_x == 1)) {               /* 0x3043 / 0x304A */
        dd2_dir_x = -dd2_dir_x;                           /* 0x3051 */
        memcpy(dd2_prev_frame, dd2_cur_frame, sizeof dd2_prev_frame);
        Obj_SetState(o, o->info);                         /* 0x306B */
        Obj_Box(o);                                       /* 0x3074 */
        Obj_PushOutX(o, tiles, east, west, w, h);         /* 0x3077 */
    }
    if (o->timer != 0) {                                  /* 0x307A */
        o->timer = (int16_t)(o->timer - (int16_t)dd2_frame_delta);
        if (o->timer < 0) o->timer = 0;
    }
}

/* -------------------------------------------------------------------------
 * 0x2EC9 -- reload reaction (0x9B0/0x9C4/0x9D8).  NOT just drawing:
 *     call 0x19E7                       ; full X and Y collision
 *     if [+0x46] != 0: -= delta, not below 0 ; draw
 * ---------------------------------------------------------------------- */
void Obj_ReloadReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                     const uint8_t *south, const uint8_t *east,
                     const uint8_t *west, int w, int h)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);      /* 0x2EC9: call 0x19E7 */
    Obj_PushOutY(o, tiles, north, south, w, h);
    if (o->timer != 0) {                           /* 0x2ECC */
        o->timer = (int16_t)(o->timer - (int16_t)dd2_frame_delta);
        if (o->timer < 0) o->timer = 0;
    }
    Obj_OffscreenCheck(o);                         /* 0x2F0A: call 0x30C0 */
}

/* =========================================================================
 *  ENEMY BATCH (0x366B..0x4A0F)
 * ====================================================================== */

/* 0x366B -- CHUNKNW/CHUNKSW (fn): acceleration along X, gravity and transition
   to fall state 0x0C44 once it starts going down.
     [0x8BFE] = [0x8C02] * delta ; 0x19C0 ; if [0x8C04] > 0 AND [0x8C10] <= 0
     -> SetState(0x0C44), [0x8C10] = 1
   [+0x2E] in the original is the X acceleration; in the port the free f1a is
   taken for it; [+0x30] (gravity) -- f2c, as in Obj_Gravity. */
void Obj_ChunkFly(dd2_obj *o, int fall_state)
{
    if (!o) return;
    dd2_xmove = (int16_t)(o->f1a * dd2_frame_delta);      /* 0x366B */
    Obj_Gravity(o);                                       /* 0x3675 */
    if (o->f2c > 0 && dd2_dir_y <= 0) {                   /* 0x3678 */
        Obj_SetState(o, fall_state);
        dd2_dir_y = 1;                                    /* 0x368F */
    }
}

/* 0x3696 -- SPARKCHUNK (fn): the same, but without the transition to a fall. */
void Obj_SparkChunkFly(dd2_obj *o)
{
    if (!o) return;
    dd2_xmove = (int16_t)(o->f1a * dd2_frame_delta);      /* 0x3696 */
    Obj_Gravity(o);                                       /* 0x36A0 */
}

/* 0x36A4 -- CHUNK reaction (landing and bouncing off a wall):
     call 0x19E7
     if hit below: y += 0x80 ; SetState(0x0C6C)      ; landed
     else if hit on the left/right: dir_x = -dir_x ;
          [0x8C02] = -([0x8C02] / 2) ; SetState(the same record)
     if hit above: [0x8C04] = 0                     ; cut the speed
     draw */
void Obj_ChunkLand(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                   const uint8_t *south, const uint8_t *east,
                   const uint8_t *west, int w, int h, int land_state)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);             /* 0x36A4 */
    Obj_PushOutY(o, tiles, north, south, w, h);
    if (o->hit_below) {                                   /* 0x36A7 */
        o->y = (int16_t)(o->y + 0x80);                    /* 0x36AE */
        Obj_SetState(o, land_state);                      /* 0x36B4 */
    } else if (o->hit_left || o->hit_right) {             /* 0x36BA */
        dd2_dir_x = -dd2_dir_x;                           /* 0x36C8 */
        o->f1a = (int16_t)(-(o->f1a / 2));                /* 0x36D0 */
        Obj_SetState(o, o->info);                         /* 0x36DE */
    }
    if (o->hit_above) o->f2c = 0;                         /* 0x36E7 */
}

/* 0x3707 -- SPARKCHUNK reaction: touched anything -- it goes out. */
void Obj_SparkReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);             /* 0x3707 */
    Obj_PushOutY(o, tiles, north, south, w, h);
    if (o->hit_below || o->hit_left || o->hit_right || o->hit_above) {
        o->type = 0;                                      /* 0x3726: [0x8BD4] = 0 */
        o->f02  = 0;                                      /* [0x8BD6] = 0 */
    }
}

/* 0x4623 -- ZOMBDOWN reaction: if there is floor underfoot OR we are below
   Dave -- stand up (0x0C94), otherwise stay lying down.  Then timer [+0x46]. */
void Obj_ZombieDownReact(dd2_obj *o, dd2_obj *dave, const uint16_t *tiles,
                         const uint8_t *south, const uint8_t *north,
                         int w, int h)
{
    int tile = 0, solid = 0;
    if (!o) return;
    if (tiles && south && o->tx_b >= 0 && o->tx_b < h &&
        o->tx_l >= 0 && o->tx_l < w) {
        tile = tiles[o->tx_b * w + o->tx_l];
        if (tile <= 857) solid = south[tile];
    }
    if (!(solid == 0 && dave && o->tx_b <= dave->tx_b)) {
        Obj_PushOutY(o, tiles, north, south, w, h);       /* 0x464C: call 0x19EE */
        Obj_SetState(o, 0x0C94);                          /* 0x464F: stand up */
    }
    if (o->timer != 0) {                                  /* 0x4658 */
        o->timer = (int16_t)(o->timer - (int16_t)dd2_frame_delta);
        if (o->timer < 0) o->timer = 0;
    }
}

/* 0x476B -- HNATTACKW3 (fn): the hunchback throws a knife (type 0x10, record
   0x0DD4) in the facing direction; when dir_x > 0 the throw point is shifted by +0x200. */
void Obj_HunchThrow(dd2_obj *o)
{
    int slot;
    if (!o) return;
    if (dd2_dir_x > 0)                                    /* 0x476B */
        slot = Obj_Spawn(0x10, o->x + 0x200, o->y, 1, 0x0DD4);
    else
        slot = Obj_Spawn(0x10, o->x, o->y, -1, 0x0DD4);
    if (slot >= 0) {
        dd2_objs[slot].f48[0] = 0;                        /* 0x47A5: [bx+0x48] = 0 */
        dd2_objs[slot].f02 = 1;                           /* 0x47AA: [bx+2] = 1 */
    }
    /* 0x47AF: [0x8C16] = 4 -- 0x1D76 defers the record fn for 4 cycles, so the
       hunchback is silent for ~32 ticks after a throw. */
    o->f42 = 4;
}

/* 0x47B6 -- KNIFEW reaction: push out along X ONLY; if it hit something it got
   stuck (SetState 0x0AA0). */
void Obj_KnifeReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *east,
                    const uint8_t *west, int w, int h)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);             /* 0x47B6: call 0x1AC9 */
    if (o->hit_left || o->hit_right)                      /* 0x47B9 */
        Obj_SetState(o, 0x0AA0);                          /* 0x47C7 */
    Obj_OffscreenCheck(o);                                /* 0x47E2: call 0x30C0 */
}

/* 0x49D1 -- GHOSTSOLID (fn): the ghost became solid. */
void Obj_GhostSolid(dd2_obj *o)
{
    if (!o) return;
    o->f48[1] = 1;                                        /* [0x8C1E] = 1 */
    o->f48[0] = 1;                                        /* [0x8C1C] = 1 */
    if (dd2_sfx) dd2_sfx(0x0F);                           /* GHOSTREAL */
}

/* 0x496F -- STARTDAVE (fn): "the ghost noticed Dave" -- if he is close
   vertically (|dy| < 0x200) and at a horizontal distance of 0x200..0x500 in the
   facing direction, the ghost appears: SFX(0x0E) GHOSTFADE, [0x8C14] = 0,
   SetState(0x0EC4). */
void Obj_GhostStartDave(dd2_obj *o, dd2_obj *dave)
{
    int dy, dx;
    if (!o || !dave) return;
    dy = dave->y - o->y;
    if (dy < 0) dy = -dy;
    if ((unsigned)dy >= 0x200u) return;                   /* 0x498D */
    if (dave->dir > 0) dx = dave->x - o->x;               /* 0x5244 = Dave's dir */
    else               dx = o->x - dave->x;
    if ((unsigned)dx < 0x200u || (unsigned)dx > 0x500u) return;
    if (dd2_sfx) dd2_sfx(0x0E);                           /* 0x49B7: GHOSTFADE */
    o->f40 = 0;                                           /* [0x8C14] = 0 */
    Obj_SetState(o, 0x0EC4);                              /* 0x49C6 */
}

/* 0x4DB1 -- SKULLFIREL reaction: full collision, the renderer draws after that. */
void Obj_SkullReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);             /* 0x4DB1: call 0x19E7 */
    Obj_PushOutY(o, tiles, north, south, w, h);
    Obj_OffscreenCheck(o);                                /* 0x4DC6: call 0x30C0 */
}

/* 0x503B -- FRANKSPARK reaction: the fireball BOUNCES off walls and the floor,
   counting the bounces in [+0x4C]; after 0xC bounces it goes out. */
void Obj_FrankSparkReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                         const uint8_t *south, const uint8_t *east,
                         const uint8_t *west, int w, int h)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);             /* 0x503B */
    Obj_PushOutY(o, tiles, north, south, w, h);
    if (o->hit_above) {                                   /* 0x503E */
        dd2_dir_y = 1;
        o->f48[2]++;                                      /* 0x505A */
    } else if (o->hit_below) {                            /* 0x504D */
        dd2_dir_y = -1;
        o->f48[2]++;
    }
    if (o->hit_left) {                                    /* 0x505E */
        dd2_dir_x = 1;
        o->f48[2]++;
    } else if (o->hit_right) {                            /* 0x506D */
        dd2_dir_x = -1;
        o->f48[2]++;
    }
    if (o->f48[2] > 0x0C) {                               /* 0x507E */
        o->type = 0;
        o->f02  = 0;
    }
}

/* 0x4B4B -- WOLFRUNL reaction: push out along X, turn at a wall and JUMP
   if there is no floor ahead. */
void Obj_WolfReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                   const uint8_t *east, const uint8_t *west, int w, int h)
{
    int row, col, tile, type;
    if (!o || !tiles || !north) return;
    Obj_PushOutX(o, tiles, east, west, w, h);             /* 0x4B4C: call 0x1AC9 */

    row = o->tx_b + 1;
    col = (dd2_dir_x > 0) ? o->tx_r : o->tx_l;            /* 0x4B56 / 0x4B64 */
    tile = 0;
    if (row >= 0 && row < h && col >= 0 && col < w)
        tile = tiles[row * w + col];
    type = (tile <= 857) ? north[tile] : 0;

    if ((o->hit_left && dd2_dir_x == -1) ||               /* 0x4B86 / 0x4B94 */
        (o->hit_right && dd2_dir_x == 1)) {
        dd2_dir_x = -dd2_dir_x;                           /* 0x4BA2 */
        memcpy(dd2_prev_frame, dd2_cur_frame, sizeof dd2_prev_frame);
        Obj_SetState(o, o->info);                         /* 0x4BBC */
        Obj_Box(o);                                       /* 0x4BC5 */
        Obj_PushOutX(o, tiles, east, west, w, h);         /* 0x4BC8 */
    } else if (type == 0) {                               /* 0x4BCD: no floor ahead */
        Obj_SetState(o, 0x0FB4);                          /* 0x4BD1: jump */
        o->f2c = -65;                                     /* 0x4BDA: [0x8C04] = 0xFFBF */
    }
    o->f42 = 0;                                           /* 0x4BE0: [0x8C16] = 0 */
    if (o->timer != 0) {                                  /* 0x4BE6 */
        o->timer = (int16_t)(o->timer - (int16_t)dd2_frame_delta);
        if (o->timer < 0) o->timer = 0;
    }
}

/* 0x52D2 -- MASTCLAW: the boss's claw, HOMING on both axes with a speed
   limit from [+0x4C]:
     di = (dave.x > x) ? 1 : -1 ; dx = (dave.y > y) ? 1 : -1
     for each tick of the delta:
         [0x8C02] += di ; clamp +-[0x8C20] ; [0x8BFE] += [0x8C02]
         [0x8C04] += dx ; clamp +-[0x8C20] ; [0x8C00] += [0x8C04]
     dir_x = ([0x8C02] > 0) ? 1 : -1
   [x+0x2E]/[x+0x30] in the original are the accelerators; in the port f1a and f2c. */
static void home_chase(dd2_obj *o, dd2_obj *dave, int lim)
{
    int di = (dave->x > o->x) ? 1 : -1;                   /* 0x52D4 / 0x4A11 */
    int dx = (dave->y > o->y) ? 1 : -1;                   /* 0x52E7 / 0x4A24 */
    int si;
    for (si = 0; si < (int)dd2_frame_delta; si++) {
        o->f1a = (int16_t)(o->f1a + di);                  /* [0x8C02] += di */
        if (o->f1a < -lim) o->f1a = (int16_t)-lim;
        else if (o->f1a > lim) o->f1a = (int16_t)lim;
        dd2_xmove = (int16_t)(dd2_xmove + o->f1a);        /* [0x8BFE] += [0x8C02] */
        o->f2c = (int16_t)(o->f2c + dx);                  /* [0x8C04] += dx */
        if (o->f2c < -lim) o->f2c = (int16_t)-lim;
        else if (o->f2c > lim) o->f2c = (int16_t)lim;
        dd2_ymove = (int16_t)(dd2_ymove + o->f2c);        /* [0x8C00] += [0x8C04] */
    }
    dd2_dir_x = (o->f1a > 0) ? 1 : -1;                    /* 0x535F / 0x4A92 */
}

void Obj_MastClaw(dd2_obj *o, dd2_obj *dave)
{
    int lim;
    if (!o || !dave) return;
    lim = o->f48[2];                                      /* [0x8C20] */
    if (lim < 0) lim = -lim;
    home_chase(o, dave, lim);                             /* 0x52D2 */
}

/* 0x4A0F -- GHOSTW (fn): the ghost chases Dave along both axes with a
   FIXED limit of +-0x14 (20).  Otherwise the same as 0x52D2. */
void Obj_GhostChase(dd2_obj *o, dd2_obj *dave)
{
    if (!o || !dave) return;
    home_chase(o, dave, 0x14);                            /* 0x4A3F */
}


/* 0x5377 -- MASTFIRE (fn): the projectile flies at the speed from [+0x2E]/[+0x30]
   (f1a/f2c in the port), multiplied by the frame delta. */
void Obj_MastFire(dd2_obj *o)
{
    if (!o) return;
    dd2_xmove = (int16_t)(o->f1a * dd2_frame_delta);        /* [0x8C02] */
    dd2_ymove = (int16_t)(o->f2c * dd2_frame_delta);        /* [0x8C04] */
}

/* 0x4E73 -- STARTDAVE (reaction): "rope/vine".  Push out along X; if there is
   something solid (SOUTH) under the bottom edge -- go to state 0x1040; then it
   draws a series of sprites 0x9A from point [0x8C20] down to y + 0x80 with step
   0x80 and sprite 0x99 at the bottom.  The extra-sprite queue does the drawing. */
void Obj_RopeReact1(dd2_obj *o, const uint16_t *tiles, const uint8_t *south,
                    const uint8_t *east, const uint8_t *west,
                    int w, int h)
{
    int si, di;
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);             /* 0x4E75: call 0x1AC9 */
    for (si = o->tx_l; si <= o->tx_r; si++) {             /* 0x4E78 */
        int t;
        if (o->tx_b < 0 || o->tx_b >= h || si < 0 || si >= w) continue;
        t = tiles[o->tx_b * w + si];
        if (t <= 857 && south[t]) {
            dd2_dir_y = -1;                               /* 0x4E9C */
            Obj_SetState(o, 0x1040);
            break;
        }
    }
    if (o->y - o->f48[2] > 0x600) {                       /* 0x4EB4 */
        dd2_dir_y = -1;
        Obj_SetState(o, 0x1040);
    }
    for (di = o->f48[2]; o->y + 0x80 > di; di += 0x80)    /* 0x4EEB */
        if (dd2_extra_sprite) dd2_extra_sprite(0x9A, o->x + 0x40, di);
    if (dd2_extra_sprite) dd2_extra_sprite(0x99, o->x, o->y);
}

/* 0x4F0A -- STARTDAVE (reaction): the same, but it takes the anchor point from
   [+0x4C]: if the object is BELOW it -- it pulls it up to it and goes to state 0x1004. */
void Obj_RopeReact2(dd2_obj *o, const uint16_t *tiles,
                    const uint8_t *east, const uint8_t *west, int w, int h)
{
    int si;
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);             /* 0x4F0B: call 0x1AC9 */
    if (!(o->y > o->f48[2])) {                            /* 0x4F11 */
        o->y = o->f48[2];                                 /* 0x4F1A */
        Obj_SetState(o, 0x1004);
        return;
    }
    for (si = o->f48[2]; o->y + 0x80 > si; si += 0x80)    /* 0x4F44 */
        if (dd2_extra_sprite) dd2_extra_sprite(0x9A, o->x + 0x40, si);
    if (dd2_extra_sprite) dd2_extra_sprite(0x99, o->x, o->y);
}

/* 0x3125 -- FAN OF CHUNK PROJECTILES (called from Dave's hitscan when an
   explosive object's health reaches 0).
 *
 * The original always spawns TWO projectiles first (record 0x0C1C, or 0x0C80
 * when the source type is 0x11), then dispatches on the source type:
 *
 *   type 0x0A/0x0B (0x31CB): remove every live type 0x11/0x0F/0x0E object,
 *                            set [0x8C38] = 0x64, then spawn the 14-row block;
 *   type 2/7/0x12  (0x3483): spawn the 2-row block;
 *   anything else  (0x34DB): straight to the tail.
 *
 * The tail (0x34DB) awards Score_Award(src.x + 0x40, src.y + 0x40, kind) with
 * kind from the type table at DGROUP 0x1248, then clears the source object.
 * Table extracted from 0x3125..0x3504 (every call to 0x17F4). */
static void chunk_spawn(int x, int y, int dir, int rec, int vx, int vy, int f02)
{
    int slot = Obj_Spawn(0x12, (int16_t)x, (int16_t)y, dir, rec);
    if (slot < 0) return;
    dd2_objs[slot].f1a = (int16_t)vx;               /* [+0x2E] */
    dd2_objs[slot].f2c = (int16_t)vy;               /* [+0x30] */
    if (f02) dd2_objs[slot].f02 = 1;                /* [+0x02] = 1 */
}

void Obj_ChunkFan(dd2_obj *src)
{
    /* 14-row block of the 0x0A/0x0B branch; x2 = x is src.x*2 (0x33E6). */
    static const struct { int dx, dy, dir, rec, vx, vy, f02, x2; } b14[14] = {
        { 0x000, 0x140, -1, 0x0C44, -30,   0, 1, 0 },
        { 0x100, 0x100,  1, 0x0C44,  30,   0, 0, 0 },
        { 0x200, 0x000, -1, 0x0C1C, -30, -40, 0, 0 },
        { 0x300, 0x040,  1, 0x0C1C,  30, -40, 0, 0 },
        { 0x200, 0x140, -1, 0x0C44, -30,   0, 0, 0 },
        { 0x300, 0x100,  1, 0x0C44,  30,   0, 0, 0 },
        { 0x000, 0x200, -1, 0x0C1C, -30, -40, 0, 0 },
        { 0x100, 0x240,  1, 0x0C1C,  30, -40, 0, 0 },
        { 0x200, 0x340, -1, 0x0C44, -30,   0, 0, 0 },
        { 0x300, 0x300,  1, 0x0C44,  30,   0, 0, 0 },
        { 0x000, 0x200, -1, 0x0C1C, -30, -40, 0, 1 },   /* x = src.x * 2 */
        { 0x300, 0x240,  1, 0x0C1C,  30, -40, 0, 0 },
        { 0x200, 0x340, -1, 0x0C44, -30,   0, 0, 0 },
        { 0x300, 0x300,  1, 0x0C44,  30,   0, 0, 0 },
    };
    static const struct { int dx, dy, dir, rec, vx, vy, f02; } b2[2] = {
        { 0x000, 0x140, -1, 0x0C44, -30, 0, 1 },
        { 0x100, 0x100,  1, 0x0C44,  30, 0, 1 },
    };
    /* DGROUP 0x1248, indexed by the source type (words). */
    static const int type_kind[18] = {
        -1, -1, 1, 1, 2, 2, 2, 3, -1, -1, 5, 6,
        0x0AB4, 0x0ADC, 0x0B04, 0x0B2C, 0x0B54, 0x0B7C
    };
    int i, rec, x0, y0, kind;
    if (!src) return;
    rec = (src->type == 0x11) ? 0x0C80 : 0x0C1C;     /* [si+0] */
    x0  = src->x;                                    /* [si+6] */
    y0  = src->y;                                    /* [si+8] */

    chunk_spawn(x0,          y0,      -1, rec, -30, -40, 1);   /* 0x3149 */
    chunk_spawn(x0 + 0x100,  y0+0x40,  1, rec,  30, -40, 1);   /* 0x3186 */

    if (src->type == 0x0A || src->type == 0x0B) {
        for (i = 1; i < dd2_obj_count; i++) {                  /* 0x31D0 */
            dd2_obj *o = &dd2_objs[i];
            if (o->type == 0x11 || o->type == 0x0F || o->type == 0x0E) {
                o->f02 = 0;                                    /* [+0x02] = 0 */
                o->type = 0;                                   /* [+0x00] = 0 */
            }
        }
        dd2_door_flag = 0x64;                                  /* 0x3227 */
        for (i = 0; i < 14; i++) {
            chunk_spawn(b14[i].x2 ? (x0 << 1) : (x0 + b14[i].dx),
                        y0 + b14[i].dy, b14[i].dir, b14[i].rec,
                        b14[i].vx, b14[i].vy, b14[i].f02);
        }
    } else if (src->type == 2 || src->type == 7 || src->type == 0x12) {
        for (i = 0; i < 2; i++)
            chunk_spawn(x0 + b2[i].dx, y0 + b2[i].dy, b2[i].dir, b2[i].rec,
                        b2[i].vx, b2[i].vy, b2[i].f02);
    }

    /* 0x34DB tail */
    kind = (src->type >= 0 && src->type < 18) ? type_kind[src->type] : -1;
    Score_Award(src->x + 0x40, src->y + 0x40, kind);           /* 0x34F1 */
    src->type = 0; src->f02 = 0;                               /* 0x34F7 */
}

/* 0x3578 -- SCORE AWARD (called from 0x3641 and 0x42E6 on pickup):
     bx = bonus kind (0..5), di = x, [bp+6] = direction
     points: 100, 100, 200, 400, 800, 1600 -> [0x9320..0x9322] (32 bits)
     then Obj_Spawn(0x12, x, 0, dir, floating_score_record):
         0x0C08, 0x0BA4, 0x0BB8, 0x0BCC, 0x0BE0, 0x0BF4
     for the floating score [+0x02] = 1 and [+0x3C] = -1
     and the EXTRA LIFE check: [0x8C3A..0x8C3C] += 0x2710 (10000);
     if the accumulated value <= the score -- increment [0x8BC8] and sound 0x15.          */
unsigned int dd2_score_lo, dd2_score_hi;      /* [0x9320], [0x9322] */
static unsigned int dd2_extra_lo, dd2_extra_hi;   /* [0x8C3A], [0x8C3C] */

void score_extra_reset(void) { dd2_extra_lo = dd2_extra_hi = 0; }

/* 0x3578 -- Score_Award(x, y, kind).  Arg order verified at 0x35E4..0x35F0:
   Score_Award(type=0x12, x, y, dir=0, info=rec) -- the second argument is the
   Y coordinate of the floating score, not a direction.  (The previous version
   passed it as `dir` and hard-coded y = 0.) */
void Score_Award(int x, int y, int kind)
{
    /* 0x3592: kind 0 (the 1UP pickup) jumps straight to the spawn with NO
       score added; kinds 1..5 add 0x64/0xC8/0x190/0x320/0x640. */
    static const int pts[6]  = { 0, 0x64, 0xC8, 0x190, 0x320, 0x640 };
    static const int recs[6] = { 0x0C08, 0x0BA4, 0x0BB8, 0x0BCC, 0x0BE0, 0x0BF4 };
    int slot;
    unsigned int lo, hi;
    if (kind < 0 || kind > 5) return;                     /* 0x3583 */
    lo = dd2_score_lo + (unsigned)pts[kind];              /* 0x3597 and onward */
    hi = dd2_score_hi + (lo < dd2_score_lo ? 1u : 0u);
    dd2_score_lo = lo; dd2_score_hi = hi;

    slot = Obj_Spawn(0x12, x, y, 0, recs[kind]);          /* 0x35F0 */
    if (slot >= 0) {
        dd2_objs[slot].f02 = 1;                           /* [+0x02] = 1 */
        dd2_objs[slot].f3c = -1;                          /* [+0x3C] = -1 */
    }
    /* extra life */
    lo = dd2_extra_lo + 0x2710u;                          /* 0x360B */
    hi = dd2_extra_hi + (lo < dd2_extra_lo ? 1u : 0u);
    if (hi > dd2_score_hi || (hi == dd2_score_hi && lo > dd2_score_lo)) return;
    dd2_extra_lo = lo; dd2_extra_hi = hi;                 /* 0x3620 */
    dd2_lives++;                                          /* [0x8BC8]++ */
    if (dd2_sfx) dd2_sfx(0x15);
    /* 0x3638: the "1UP" floating-score object: type 0x12, record 0x0C08,
       y = argument - 0x100, dir = 0, no [+0x02] flag, [+0x3C] = -1. */
    slot = Obj_Spawn(0x12, x, y - 0x100, 0, 0x0C08);
    if (slot >= 0)
        dd2_objs[slot].f3c = -1;                          /* 0x3655 */
}

/* -------------------------------------------------------------------------
 * 0x41E7 -- the object-vs-object reaction stored in rec[+0x0E] of every Dave
 * record (and of the ghost records, which use 0x49E4).  Called as
 * [self+0x56](self, other); the body reacts to the TYPE of `other`.
 *
 *   type 2   -- hunchback: hit only while it faces Dave (0x422F) -> -3
 *   type 4   -- -> -2      type 5 -> -4      type 6 -> -5 if [+0x48] != 0
 *   type 7   -- -> -6      type 8/0x0A/0x0E/0x0F/0x11 -> -7
 *   type 0x10-> -> -1
 *   type 9   -- bonus: SFX 0x10..0x15, +1 life for kind 5, Score_Award(x, y, kind),
 *               then the bonus object is removed from the world.
 * The negative value is the "Dave was hit" code consumed by the level loop
 * (0x2295), where it selects the death animation.  0x42F3/0x4307 (the tail)
 * only clears the code under the god-mode flag [0x4A8C], which the port omits.
 * ---------------------------------------------------------------------- */
/* Returns 1 when the caller must run the 0x41E7 god-mode tail (0x42F3) and 0
   when the original leaves through 0x4307 (plain ret) -- the two type-2 facing
   exits at 0x424A/0x425E and the separate ghost function 0x49E4, which has no
   tail at all. */
static int pair_react_core(dd2_obj *self, dd2_obj *other)
{
    int t, kind;
    if (!self || !other) return 1;

    /* 0x49E4: the ghost (records 0xF28/0xF3C) becomes solid on touching Dave
       (type 1): switch itself to record 0xF00. */
    if (self->react_obj == 0x49E4) {
        if (other->type == 1) {
            const unsigned char *ai = dd2_actorinfo_blob + (0x0F00 - DD2_ACTORINFO_BASE);
            self->info      = 0x0F00;                          /* [si+0x44] */
            self->sprite    = (int16_t)(ai[2] | (ai[3] << 8));  /* [0xF02] */
            self->react     = (int16_t)(ai[0x10] | (ai[0x11] << 8)); /* [0xF10] */
            self->react_obj = (int16_t)(ai[0x0E] | (ai[0x0F] << 8)); /* [0xF0E] */
        }
        return 0;                                 /* 0x49E4: no tail */
    }
    if (self->react_obj != 0x41E7) return 1;

    t = other->type - 2;                          /* 0x41EF */
    if (t < 0 || t > 0xF) return 1;               /* 0x41F8 -> 0x42F3 */

    switch (t) {
    case 0:                                       /* 0x422F: type 2 */
        if (other->info != 0x0D0C) return 1;
        if (other->dir == 1) {
            if (other->x > self->x) return 0;     /* 0x424A -> 0x4307 */
        } else if (other->dir == -1) {
            if (other->x < self->x) return 0;     /* 0x425E -> 0x4307 */
        }
        dd2_door_flag = -3;                       /* 0x4261: 0xFFFD */
        return 1;
    case 2:  dd2_door_flag = -2; return 1;          /* type 4:  0xFFFE */
    case 3:  dd2_door_flag = -4; return 1;          /* type 5:  0xFFFC */
    case 4:                                       /* type 6:  0xFFFB */
        if (other->f48[0] == 0) return 1;
        dd2_door_flag = -5; return 1;
    case 5:  dd2_door_flag = -6; return 1;          /* type 7:  0xFFFA */
    case 6:  dd2_door_flag = -7; return 1;          /* type 8:  0xFFF9 */
    case 8:  dd2_door_flag = -7; return 1;          /* type 0x0A */
    case 12: case 13: dd2_door_flag = -7; return 1; /* type 0x0E, 0x0F */
    case 14: dd2_door_flag = -1; return 1;          /* type 0x10: 0xFFFF */
    case 15: dd2_door_flag = -7; return 1;          /* type 0x11 */
    case 7:                                       /* 0x427B: type 9 bonus */
        kind = other->f48[2];                     /* [+0x4C] */
        if (kind == 5) {                          /* 1UP: 0x42D0 */
            if (dd2_sfx) dd2_sfx(0x15);
            dd2_lives++;                          /* [0x8BC8]++ */
            kind = 0;
        } else if (kind >= 0 && kind <= 4) {      /* 0x428A..0x42C2 */
            if (dd2_sfx) dd2_sfx(0x10 + kind);
            kind++;
        } else {
            return 1;                               /* >5 is unreachable in data */
        }
        Score_Award(other->x, other->y, kind);    /* 0x42DF */
        other->type = 0; other->f02 = 0;          /* 0x42EC: remove the bonus */
        return 1;
    default:
        return 1;                                   /* 0x42F3 tail */
    }
}

/* 0x42F3..0x4301: with God mode ([0x4A8C] != 0) a negative hit code is
   cleared again, so Dave cannot die on contact. */
void Obj_PairReact(dd2_obj *self, dd2_obj *other)
{
    /* 0x42F3: the god-mode cheat clears a negative flag, but only on the paths
       that reach the tail (not the two 0x424A/0x425E facing exits, not 0x49E4). */
    if (pair_react_core(self, other) && dd2_god_mode && dd2_door_flag < 0)
        dd2_door_flag = 0;
}

/* 0x50D8 -- MASTER1 (fn): the boss walks between 0x200 and 0xF00 and
   periodically "sets up" two turret nests (type 0x0E, record 0x10B8), remembering
   them in [0x8C20]/[0x8C22]; each lives 0x12C ticks, and once every 0x14A ticks
   the boss changes state to 0x1090 with sound 0x17.  Slots are stored as index+1. */
int dd2_master_slot1, dd2_master_slot2;      /* [0x8C20], [0x8C22] */

void Obj_MasterWalk(dd2_obj *o)
{
    int slot;
    if (!o) return;
    if (dd2_dir_x > 0) {                                  /* 0x50D8 */
        if (o->x > 0x0F00) dd2_dir_x = -1;
    } else {
        if (o->x < 0x0200) dd2_dir_x = 1;
    }
    o->x = (int16_t)(o->x + dd2_frame_delta * dd2_dir_x * 0x0C);  /* 0x50FD */

    if (!(dd2_master_slot1 > 0 && dd2_objs[dd2_master_slot1 - 1].type == 0x0E)) {
        o->f48[6] = (int16_t)(o->f48[6] + (int16_t)dd2_frame_delta);   /* [0x8C28] */
        if (o->f48[6] > 0x12C) {                          /* 0x5120 */
            o->f48[6] = 0;
            slot = Obj_Spawn(0x0E, o->x, o->y + 0x300, -1, 0x10B8);
            if (slot >= 0) {
                dd2_objs[slot].f48[1] = 1;                /* [+0x4A] */
                dd2_objs[slot].f48[0] = 1;                /* [+0x48] */
                dd2_objs[slot].health = 2;                /* [+0x3E] */
                dd2_objs[slot].f48[2] = 8;                /* [+0x4C] */
                dd2_master_slot1 = slot + 1;
            }
            Obj_SetState(o, 0x107C);                      /* 0x5165 */
        }
    }
    if (!(dd2_master_slot2 > 0 && dd2_objs[dd2_master_slot2 - 1].type == 0x0E)) {
        o->f48[5] = (int16_t)(o->f48[5] + (int16_t)dd2_frame_delta);   /* [0x8C26] */
        if (o->f48[5] > 0x12C) {                          /* 0x5181 */
            o->f48[6] = 0;
            slot = Obj_Spawn(0x0E, o->x + 0x400, o->y + 0x300, -1, 0x10B8);
            if (slot >= 0) {
                dd2_objs[slot].f48[1] = 1;
                dd2_objs[slot].f48[0] = 1;
                dd2_objs[slot].health = 2;
                dd2_objs[slot].f48[2] = 6;                /* [+0x4C] = 6 */
                dd2_master_slot2 = slot + 1;
            }
            Obj_SetState(o, 0x107C);                      /* 0x51C9 */
        }
    }
    o->f48[3] = (int16_t)(o->f48[3] + (int16_t)dd2_frame_delta);       /* [0x8C24] */
    if (o->f48[3] > 0x14A) {                              /* 0x51DC */
        o->f48[3] = 0;
        o->f48[5] = 0;
        Obj_SetState(o, 0x1090);                          /* 0x51EC */
        if (dd2_sfx) dd2_sfx(0x17);
    }
}

/* 0x51FF -- MASTER (fn): the boss's attack.  Counts [+0x52]; when it exceeds
   0x3C -- fires a FAN of four projectiles (type 0x0F, record 0x10F4) with
   the given offsets and speeds, then SetState(0x1054):
     1) x,     y+0x300  vx=-30  vy=30
     2) x+100, y+400    vx=-16  vy=40
     3) x+300, y+400    vx= 16  vy=40
     4) x+400, y+300    vx= 30  vy=30                        */
void Obj_MasterAttack(dd2_obj *o)
{
    static const struct { int dx, dy, vx, vy; } shot[4] = {
        { 0x000, 0x300, -30, 30 },
        { 0x100, 0x400, -16, 40 },
        { 0x300, 0x400,  16, 40 },
        { 0x400, 0x300,  30, 30 },
    };
    int i, slot;
    if (!o) return;
    o->f48[5] = (int16_t)(o->f48[5] + (int16_t)dd2_frame_delta);   /* 0x51FF */
    if (o->f48[5] <= 0x3C) return;                                 /* 0x5209 */
    for (i = 0; i < 4; i++) {
        slot = Obj_Spawn(0x0F, o->x + shot[i].dx, o->y + shot[i].dy, -1, 0x10F4);
        if (slot >= 0) {
            dd2_objs[slot].f1a = (int16_t)shot[i].vx;              /* [+0x2E] */
            dd2_objs[slot].f2c = (int16_t)shot[i].vy;              /* [+0x30] */
        }
    }
    Obj_SetState(o, 0x1054);                                       /* 0x52C8 */
}

/* 0x53D2 -- DELBERT reaction: VERTICAL push-out (0x19EE); touched the
   floor -- state 0x1158.  The renderer does the drawing. */
void Obj_DelbertReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                      const uint8_t *south, int w, int h)
{
    if (!o) return;
    Obj_PushOutY(o, tiles, north, south, w, h);          /* 0x53D2: call 0x19EE */
    if (o->hit_below)                                    /* 0x53D5 */
        Obj_SetState(o, 0x1158);                         /* 0x53DC */
}

/* 0x53F8 -- DELBERT4 (fn): [0x8C38] = 1.  This is the game-completion flag:
   DELBERT is the last boss. */
void Obj_DelbertWin(dd2_obj *o)   { (void)o; dd2_door_flag = 1; }

/* 0x310D -- ZATTACK* (fn): simply turn to face Dave. */
void Obj_FaceDave(dd2_obj *o, dd2_obj *dave)
{
    if (!o || !dave) return;
    dd2_dir_x = (o->bbox_l > dave->bbox_l) ? -1 : 1;     /* 0x310D */
}

/* -------------------------------------------------------------------------
 * 0x4F9A -- Frankenstein, timer-based animation (FRANKGOL1..4):
 *     [+0x4C]++ ; if > 0xA: [+0x4C] -= 0xA ; SetState(0x11D0)
 * ---------------------------------------------------------------------- */
void Obj_FrankAnim(dd2_obj *o)
{
    if (!o) return;
    o->f48[2]++;                                   /* 0x4F9A */
    if (o->f48[2] > 0x0A) {                        /* 0x4F9E */
        o->f48[2] = (int16_t)(o->f48[2] - 0x0A);
        Obj_SetState(o, 0x11D0);                   /* 0x4FAA */
    }
}

/* -------------------------------------------------------------------------
 * 0x4FB4 -- Frankenstein throws a fireball in the facing direction:
 *     dir_x > 0 -> Obj_Spawn(0x11, bbox_r,        y + 0x100,  1, 0x11F8)
 *     otherwise -> Obj_Spawn(0x11, bbox_l - 0x100, y + 0x100, -1, 0x11F8)
 * ---------------------------------------------------------------------- */
void Obj_FrankThrow(dd2_obj *o)
{
    int slot;
    if (!o) return;
    if (dd2_dir_x > 0)                             /* 0x4FB4 */
        slot = Obj_Spawn(0x11, o->bbox_r, o->y + 0x100, 1, 0x11F8);
    else
        slot = Obj_Spawn(0x11, o->bbox_l - 0x100, o->y + 0x100, -1, 0x11F8);
    if (slot < 0) return;
    /* 0x4FF0-0x5005: the freshly spawned fireball is configured through
       [0x8BBE] -- dir_y = 1 (so 0x5006 gives it a vertical speed), health = 1,
       [+0x4A] = 1 (explosive, tested at 0x3C82) and [+0x48] = 1 (vulnerable,
       tested at 0x3C62).  Without this the spark flew horizontally and could
       not be shot. */
    dd2_objs[slot].f3c = 1;                        /* [bx+0x3C] */
    dd2_objs[slot].health = 1;                     /* [bx+0x3E] */
    dd2_objs[slot].f48[1] = 1;                     /* [bx+0x4A] */
    dd2_objs[slot].f48[0] = 1;                     /* [bx+0x48] */
}

/* -------------------------------------------------------------------------
 * 0x5006 -- fireball flight (FRANKSPARK): speed 0x18 per frame delta
 *     [0x8BFE] = dir_x * delta * 0x18
 *     [0x8C00] = dir_y * delta * 0x18
 * ---------------------------------------------------------------------- */
void Obj_SparkFly(dd2_obj *o)
{
    (void)o;
    dd2_xmove = (int16_t)(dd2_dir_x * dd2_frame_delta * 0x18);   /* 0x5006 */
    dd2_ymove = (int16_t)(dd2_dir_y * dd2_frame_delta * 0x18);   /* 0x5015 */
}

/* -------------------------------------------------------------------------
 * 0x5025 -- spark timer: [+0x4E]++ ; if >= 0x18 -> SetState(0x1220)
 * ---------------------------------------------------------------------- */
void Obj_SparkTimer(dd2_obj *o)
{
    if (!o) return;
    o->f48[3]++;                                   /* 0x5025 */
    if (o->f48[3] >= 0x18)                         /* 0x5029 */
        Obj_SetState(o, 0x1220);
}

/* -------------------------------------------------------------------------
 * 0x4E07 -- spider (SPIDERL1/2).  Attacks downward when Dave is right below
 * it, otherwise simply turns toward him.
 *
 *   dx = dave.x - x ; dy = dave.y - y
 *   if dy < 0x600 (96 px, unsigned -- so Dave is BELOW):
 *       if |dx| < 0xA0 (10 px):
 *           dir_y = 1 ; SetState(0x102C)      ; attack downward
 *           exit
 *   if dave.x > x and dir_x == -1 -> dir_x = 1,  SetState(o->info)
 *   if dave.x < x and dir_x ==  1 -> dir_x = -1, SetState(o->info)
 * ---------------------------------------------------------------------- */
void Obj_SpiderAI(dd2_obj *o, dd2_obj *dave)
{
    int dx, dy;
    if (!o || !dave) return;

    dx = dave->x - o->x;                            /* 0x4E08 */
    dy = dave->y - o->y;                            /* 0x4E11 */

    if ((unsigned)dy < 0x600u) {                    /* 0x4E18: Dave below and close */
        int adx = dx < 0 ? -dx : dx;
        if (adx < 0xA0) {                           /* 0x4E29 */
            dd2_dir_y = 1;                          /* 0x4E2E */
            Obj_SetState(o, 0x102C);                /* 0x4E34: attack downward */
            return;
        }
    }
    if (dx > 0) {                                   /* 0x4E3A: Dave to the right */
        if (dd2_dir_x == -1) {
            dd2_dir_x = 1;
            Obj_SetState(o, o->info);               /* 0x4E68 */
        }
    } else if (dx < 0) {                            /* 0x4E52: Dave to the left */
        if (dd2_dir_x == 1) {
            dd2_dir_x = -1;
            Obj_SetState(o, o->info);
        }
    }
}

/* -------------------------------------------------------------------------
 * 0x30C0 -- REMOVAL of an object that went far off screen (unlike the culling
 * in 0x1EAB, which only skips the update, here the object is removed for good).
 * Returns 1 if the object was removed.
 *
 *   remove if (vp_l - 8 > tx_r) OR (vp_t - 8 > tx_b)
 *              OR (vp_l + 0x1C < tx_l) OR (vp_t + 0x12 < tx_t)
 *
 * The removal itself: if type >= 0x0D -> type = 0; always f02 = 0.
 * ([0x8C44]/[0x8C68] are dd2_vp_l/dd2_vp_t.)
 * ---------------------------------------------------------------------- */
int Obj_OffscreenCheck(dd2_obj *o)
{
    if (!o) return 0;
    if (dd2_vp_l - 8 > o->tx_r || dd2_vp_t - 8 > o->tx_b ||
        dd2_vp_l + 0x1C < o->tx_l || dd2_vp_t + 0x12 < o->tx_t) {
        if (o->type >= 0x0D) o->type = 0;          /* 0x30F0 */
        o->f02 = 0;                                /* 0x30FF */
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * 0x4878 -- slime "wanders": random turns on both axes.
 *
 *   if rand() < 0x20 -> dir_x = -dir_x
 *   if rand() < 0x10 -> dir_y = -dir_y, SetState(dir_y > 0 ? 0x0E38 : 0x0E60)
 * ---------------------------------------------------------------------- */
void Obj_SlimeWander(dd2_obj *o)
{
    if (!o) return;
    if (dd2_rand_byte() < 0x20)                    /* 0x4878 */
        dd2_dir_x = -dd2_dir_x;
    if (dd2_rand_byte() < 0x10) {                  /* 0x4888 */
        if (dd2_dir_y > 0) {
            dd2_dir_y = -1;
            Obj_SetState(o, 0x0E60);
        } else {
            dd2_dir_y = 1;
            Obj_SetState(o, 0x0E38);
        }
    }
}

/* -------------------------------------------------------------------------
 * 0x48B2 -- the slime crawls along a surface: full push-out (0x19E7), and
 * if it hit the ceiling (moving up) or the floor (moving down) --
 * it changes state.
 * ---------------------------------------------------------------------- */
void Obj_SlimeCrawl(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);      /* 0x48B2: call 0x19E7 */
    Obj_PushOutY(o, tiles, north, south, w, h);
    if (dd2_dir_y == -1 && o->hit_above) {         /* 0x48B5 */
        Obj_SetState(o, 0x0E88);
        if (dd2_sfx) dd2_sfx(2);
    } else if (dd2_dir_y == 1 && o->hit_below) {   /* 0x48C8 */
        Obj_SetState(o, 0x0E9C);
        if (dd2_sfx) dd2_sfx(2);
    }
    Obj_OffscreenCheck(o);                         /* 0x48FA: call 0x30C0 */
}

/* -------------------------------------------------------------------------
 * 0x4AE1 -- wolf-runner (WOLFRUNL*, 4 records).  Always charges at Dave, and
 * jumps when close:
 *
 *   dy = |bbox_b - dave.bbox_t|
 *   x > dave.x  ->  dx = bbox_l - dave.bbox_r, dir = -1
 *   otherwise   ->  dx = dave.bbox_l - bbox_r, dir = +1
 *
 *   if dy < 0x300 (48 px):
 *       dir_x = dir                        ; run toward Dave
 *       if dx < 0x600 (96 px):
 *           SetState(0x0FB4)               ; jump
 *           [0x8C04] = 0xFFBF              ; ymove = -65
 * ---------------------------------------------------------------------- */
void Obj_WolfAI(dd2_obj *o, dd2_obj *dave)
{
    int dy, dx, dir;
    if (!o || !dave) return;

    dy = o->bbox_b - dave->bbox_b;   /* 0x4532/0x46DB/0x4AE8: [0x8BE4] - [0x521A] */
    if (dy < 0) dy = -dy;

    if (o->x > dave->x) {
        dx = o->bbox_l - dave->bbox_r;
        dir = -1;
    } else {
        dx = dave->bbox_l - o->bbox_r;
        dir = 1;
    }

    if (dy < 0x300) {                               /* 0x4B24 */
        dd2_dir_x = dir;                            /* 0x4B2A */
        if (dx < 0x600) {                           /* 0x4B30 */
            Obj_SetState(o, 0x0FB4);                /* 0x4B36: jump */
            o->f2c = -65;                           /* 0x4B3F: [0x8C04] = 0xFFBF (gravity seed) */
        }
    }
}

/* -------------------------------------------------------------------------
 * 0x46D4 -- walking hunchback (HNCHWALK*, 4+ records).  Behavior depends on the
 * distance to Dave:
 *
 *   dy = |bbox_b - dave.bbox_t|
 *   x > dave.x  ->  dx = bbox_l - dave.bbox_r, dir = -1
 *   otherwise   ->  dx = dave.bbox_l - bbox_r, dir = +1
 *
 *   if dy <= 0x100:                       ; close vertically
 *       if dx > 0x700 (112 px) -> dir_x = dir (run toward Dave) and with
 *           probability 50/256 -> SFX(8) + SetState(0x0D98) (attack)
 *       else if dx < 0x300 (48 px) -> dir_x = -dir (back away)
 *   otherwise                             ; not close vertically
 *       with probability 2/256 -> dir_x = -dir_x (random turn)
 * ---------------------------------------------------------------------- */
void Obj_HunchbackAI(dd2_obj *o, dd2_obj *dave)
{
    int dy, dx, dir;
    if (!o || !dave) return;

    dy = o->bbox_b - dave->bbox_b;   /* 0x4532/0x46DB/0x4AE8: [0x8BE4] - [0x521A] */
    if (dy < 0) dy = -dy;

    if (o->x > dave->x) {
        dx = o->bbox_l - dave->bbox_r;
        dir = -1;
    } else {
        dx = dave->bbox_l - o->bbox_r;
        dir = 1;
    }

    if (dy <= 0x100) {                              /* 0x4717 */
        if (dx > 0x700) {                           /* 0x471D: far -> run */
            dd2_dir_x = dir;                        /* 0x4723 */
            if (dd2_rand_byte() < 0x32) {           /* 0x4729 */
                if (dd2_sfx) dd2_sfx(8);
                Obj_SetState(o, 0x0D98);            /* 0x473A: attack */
            }
            return;
        }
        if (dx < 0x300)                             /* 0x4745: close -> back away */
            dd2_dir_x = -dir;
        return;
    }
    if (dd2_rand_byte() < 2)                        /* 0x4755 */
        dd2_dir_x = -dd2_dir_x;
}

/* -------------------------------------------------------------------------
 * 0x3504 (BONUS100/200/400/800) and 0x351E (BONUS1UP) -- the bonus DISAPPEARS
 * on a timer if it was not picked up:
 *
 *   0x3504:  [+0x4C] += delta ; if > 0x3C  -> remove the object
 *   0x351E:  [+0x4C] += delta ; if > 0xB4  -> remove the object
 *
 * "Remove" means [0x8BD4] = 0 and [0x8BD6] = 0, i.e. +0x00 (type) and
 * +0x02 (active) of the scratch object; in the port the object is the same, so
 * it is written directly (just as at the tail of the 0x1D76 chain).
 * ---------------------------------------------------------------------- */
void Obj_BonusTimer(dd2_obj *o, int ttl_limit)
{
    int16_t t;
    if (!o) return;
    t = (int16_t)(o->f48[2] + (int16_t)dd2_frame_delta);   /* [+0x4C] += delta */
    o->f48[2] = t;
    if (t > ttl_limit) {
        o->type = 0;                                       /* [0x8BD4] = 0 */
        o->f02  = 0;                                       /* [0x8BD6] = 0 */
    }
}

/* -------------------------------------------------------------------------
 * 0x1B9D -- "is there something solid underfoot".  Works only when the object's
 * bottom edge is odd (check `test [0x8BE4], 1`), otherwise it does nothing.
 * It looks at row tx_b + 1 over columns tx_l..tx_r with the NORTH table and sets
 * [+0x36] hit_below = 1 if floor is found.  This function is called by behaviors
 * 0x4338/0x4380 to find out whether it is time to fall.
 * ---------------------------------------------------------------------- */
int Obj_GroundBelow(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    int w, int h)
{
    int si, row;
    if (!o || !tiles || !north) return 0;
    if (!(o->bbox_b & 1)) return 0;                  /* 0x1B9E */
    row = o->tx_b + 1;
    for (si = o->tx_l; si <= o->tx_r; si++) {        /* 0x1BA6 */
        int tile, idx = row * w + si;                /* 0x1BAC: flat, as the original */
        if (idx < 0 || idx >= w * h) continue;
        tile = tiles[idx];
        if (tile > 857 || north[tile] == 0) continue;
        o->hit_below = 1;                            /* 0x1BCB */
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * 0x19EE -- VERTICAL push-out.  It was missing: without it Dave fell through
 * the floor after a jump.  It is always called together with the horizontal one
 * -- 0x19E7 is `call 0x1AC9; call 0x19EE`.
 *
 *   floor:  di = f28+1 .. tx_b, si = tx_l .. tx_r, NORTH table ([0x4A92])
 *           dy = (di << 8) + 1 - bottom ; y -= dy ; top -= dy ; bottom -= dy
 *           tx_b-- ; [+0x36] hit_below = 1
 *   ceiling: di = f24-1 .. >= tx_t, si = tx_l .. tx_r, SOUTH table ([0x4A94])
 *           dy = ((di + 1) << 8) - top ; y += dy ; top += dy ; bottom += dy
 *           tx_t++ ; [+0x32] hit_above = 1
 * ---------------------------------------------------------------------- */
int Obj_PushOutY(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                 const uint8_t *south, int w, int h)
{
    int di, si, hit = 0;
    if (!o || !tiles || !north || !south) return 0;

    for (di = o->f28 + 1; di <= o->tx_b; di++) {          /* 0x19F5 */
        for (si = o->tx_l; si <= o->tx_r; si++) {         /* 0x19FE */
            int tile, dy, idx;
            idx = di * w + si;                            /* 0x1A04: flat, as the original */
            if (idx < 0 || idx >= w * h) continue;
            tile = tiles[idx];
            if (tile > 857 || north[tile] == 0) continue;
            /* 0x1A22: ax = (row << 8) ; dx = bbox_b + 1 ; dx -= ax ; di = dx
               0x1A32: y -= di   -- that is UP, toward the top edge of the tile.
               Previously the sign was inverted here (y -= ((row<<8)+1 - bbox_b)),
               the object was pushed DOWN, and harder with every frame --
               hence "falls through the floor" and the teleports. */
            dy = o->bbox_b + 1 - (di << 8);
            o->y -= (int16_t)dy;                          /* 0x1A32 */
            o->bbox_t -= (int16_t)dy;
            o->bbox_b -= (int16_t)dy;
            o->tx_b--;                                    /* 0x1A3E */
            o->hit_below = 1;                             /* 0x1A42 */
            hit = 1;
            goto ceiling;
        }
    }
ceiling:
    for (di = o->f24 - 1; di >= o->tx_t; di--) {          /* 0x1A5D */
        for (si = o->tx_l; si <= o->tx_r; si++) {         /* 0x1A66 */
            int tile, dy, idx;
            idx = di * w + si;                            /* 0x1A6C: flat */
            if (idx < 0 || idx >= w * h) continue;
            tile = tiles[idx];
            if (tile > 857 || south[tile] == 0) continue;
            dy = ((di + 1) << 8) - o->bbox_t;             /* 0x1A8A */
            o->y += (int16_t)dy;                          /* 0x1A98 */
            o->bbox_t += (int16_t)dy;
            o->bbox_b += (int16_t)dy;
            o->tx_t++;                                    /* 0x1AA4 */
            o->hit_above = 1;                             /* 0x1AA8 */
            hit = 1;
            goto done;
        }
    }
done:
    return hit;
}

/* -------------------------------------------------------------------------
 * 0x452A -- zombie behavior (fn of records 0x0C94/0x0CA8/0x0CBC/0x0CD0).
 * PARTIALLY PORTED: chasing along X and starting the attack.
 *
 *   dy = |[0x8BE4] - [0x521A]|              ; |zombie bottom - Dave top|
 *   if zombie x > Dave x: dx = left - [0x5218]; dir = -1
 *   otherwise:            dx = [0x5214] - right; dir = +1
 *   if dy < 0x200:                          ; close vertically
 *       [0x8C0E] = dir                      ; go toward Dave
 *       if dx <= 0x80: SetState(0x0CE4)     ; attack
 *
 * TODO: 0x4599..0x469A -- vertical chasing and the "is there ground ahead"
 * check (SOUTH/NORTH tables over the columns under the bottom edge).
 * ---------------------------------------------------------------------- */
void Obj_ZombieAI(dd2_obj *o, dd2_obj *dave, const uint16_t *tiles,
                  const uint8_t *south, const uint8_t *north,
                  int w, int h)
{
    int dy, dx, dir;

    if (!o || !dave) return;
    dy = o->bbox_b - dave->bbox_b;   /* 0x4532/0x46DB/0x4AE8: [0x8BE4] - [0x521A] */
    if (dy < 0) dy = -dy;

    if (o->x > dave->x) {                        /* 0x454C */
        dx = o->bbox_l - dave->bbox_r;
        dir = -1;
    } else {
        dx = dave->bbox_l - o->bbox_r;
        dir = 1;
    }
    if (dy < 0x200) {                            /* 0x4573 */
        dd2_dir_x = dir;                         /* 0x457D: go toward Dave */
        if (dx <= 0x80)
            Obj_SetState(o, 0x0CE4);             /* 0x4586: attack */
    }

    /* 0x458F..0x461C: VERTICAL PART.  If Dave is slightly BELOW (0 < dy2 < 0xA)
       and horizontally close (|dx2| < 6), check whether there is ground under
       the zombie across its whole width: row bbox_b+1 (SOUTH) and bbox_b+2
       (NORTH).  If there is no ground in any column -- step down: ymove = 0x80,
       state 0x0D20. */
    {
        /* 0x458F: `ax = [0x5222] - [0x8BEC]` -- Dave.tx_b - o.tx_b, in TILE
           units (both are the +0x18 field).  With bbox_b in 1/16-px the test
           (0 < dy2 < 0xA) never fired, so the step-down was dead code. */
        int dy2 = dave->tx_b - o->tx_b;
        if (dy2 > 0 && dy2 < 0xA) {
            int dx2 = o->tx_l - dave->tx_l;      /* [0x8BE6] - [0x521C] */
            int si, row;
            if (dx2 < 0) dx2 = -dx2;
            if (dx2 < 6) {
                for (si = o->tx_l; si <= o->tx_r; si++) {        /* 0x45BF */
                    row = o->tx_b + 1;                           /* 0x45C5 */
                    if (row >= 0 && row < h && si >= 0 && si < w &&
                        south[tiles[row * w + si] & 1023] != 0)
                        break;                                   /* 0x45E2: floor present */
                    row = o->tx_b + 2;                           /* 0x45E4 */
                    if (row >= 0 && row < h && si >= 0 && si < w &&
                        north[tiles[row * w + si] & 1023] == 0)
                        break;                                   /* 0x4605: no floor */
                }
                if (si > o->tx_r) {                              /* 0x4608 */
                    dd2_ymove = 0x80;                            /* 0x460E */
                    Obj_SetState(o, 0x0D20);                     /* 0x4614 */
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * 0x4338 (9 records) and 0x4380 (11 records) -- "walker": push out, and then
 * either a stand at the wall or a fall if there is nothing underfoot.
 *
 *   0x4338: call 0x1AC9 ; call 0x1B9D
 *           if hit on the left or right AND record != 0x794 -> SetState(0x780)
 *           if no hit below                              -> SetState(0x8AC)
 *   0x4380: call 0x1AC9 ; call 0x1B9D
 *           if no hit below                              -> SetState(0x8AC)
 * ---------------------------------------------------------------------- */
void Obj_WalkerReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                     const uint8_t *east, const uint8_t *west, int w, int h,
                     int stop_at_wall)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);        /* 0x4338/0x4380 */
    Obj_GroundBelow(o, tiles, north, w, h);          /* 0x1B9D */
    if (stop_at_wall &&
        (o->hit_left != 0 || o->hit_right != 0) &&
        o->info != 0x0794) {                         /* 0x433E */
        Obj_SetState(o, 0x0780);                     /* stand */
    }
    if (o->hit_below == 0)                           /* 0x435D / 0x4386 */
        Obj_SetState(o, 0x08AC);                     /* fall */
}

/* -------------------------------------------------------------------------
 * 0x2F11 -- patrol AI (22 actor-info records: zombies and kin).
 *   call 0x1AC9                       ; push out along X
 *   tile UNDER THE BOTTOM EDGE ahead: (tx_b + 1) * width + (dir_x > 0 ? tx_r : tx_l)
 *   type = NORTH[tile]                 ; [0x4A92] -- north table (floor)
 *   turn if: type == 0, OR (hit left AND dir_x == -1),
 *                   OR (hit right AND dir_x == 1)
 *   turn:  [0x8C16] = 0 or "random"/0x20; dir_x = -dir_x;
 *              frame copy; SetState(same record); 0x192E; 0x1AC9
 *   then counter [0x8C1A] (field +0x46) is decreased by the delta.
 * Drawing (0x2B0C/0x2C2D) is not needed in the port -- the central renderer draws.
 * ---------------------------------------------------------------------- */
void Obj_WalkAI(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                const uint8_t *east, const uint8_t *west, int w, int h)
{
    int row, col, tile, type, turn;

    if (!o || !tiles || !north) return;
    Obj_PushOutX(o, tiles, east, west, w, h);            /* 0x2F12 */

    row = o->tx_b + 1;                                   /* [0x8BEC] + 1 */
    col = (dd2_dir_x > 0) ? o->tx_r : o->tx_l;           /* 0x2F1C / 0x2F2A */
    tile = 0;
    if (row >= 0 && row < h && col >= 0 && col < w)
        tile = tiles[row * w + col];
    type = (tile <= 857) ? north[tile] : 0;              /* 0x2F46 */

    turn = (type == 0) ||
           (o->hit_left  && dd2_dir_x == -1) ||
           (o->hit_right && dd2_dir_x ==  1);
    if (turn) {                                          /* 0x2F6C */
        if (o->f42 != 0) o->f42 = 0;                     /* 0x2F73 */
        else o->f42 = (int16_t)((dd2_rand_byte() / 0x20) & 0x7F);  /* 0x2F7B */
        dd2_dir_x = -dd2_dir_x;                          /* 0x2F87 */
        memcpy(dd2_prev_frame, dd2_cur_frame, sizeof dd2_prev_frame);  /* 0x2F8F */
        Obj_SetState(o, o->info);                        /* 0x2FA5: the same record again */
        Obj_Box(o);                                      /* 0x2FAA */
        Obj_PushOutX(o, tiles, east, west, w, h);        /* 0x2FAD */
    }
    if (o->timer != 0) {                                 /* [0x8C1A] */
        o->timer -= (int16_t)dd2_frame_delta;
        if (o->timer < 0) o->timer = 0;
    }
}

/* -------------------------------------------------------------------------
 * 0x192E -- recompute the object's bbox from the sprite.
 *
 *   idx = sprite*4 + ((x >> 6) & 1)
 *   rec = DS:0x97B0 + idx*32                 ; spriteInfo table (runtime)
 *   left   = x + rec[+8]   + rec[+0x1C]
 *   right  = x + rec[+0x0C] + rec[+0x1C]
 *   top    = y + rec[+0x0A] + rec[+0x1E]
 *   bottom = y + rec[+0x0E] + rec[+0x1E]
 *   [0x8BE6] = left   >> 8   [0x8BEA] = right  >> 8
 *   [0x8BE8] = top    >> 8   [0x8BEC] = bottom >> 8
 *
 * The DS:0x97B0 table is zero in the image and is built at startup; its fields
 * +8/+0x0A/+0x0C/+0x0E are the sprite's bbox from the mask, which the port
 * already computes in dd2.c (bx0/by0/bx1/by1).  So the values arrive through
 * dd2_sprite_bbox.
 * ---------------------------------------------------------------------- */
void (*dd2_sprite_bbox)(int sprite, int *l, int *t, int *r, int *b);

void Obj_Box(dd2_obj *o)
{
    int l = 0, t = 0, r = 0, b = 0;
    if (!o) return;
    if (dd2_sprite_bbox) dd2_sprite_bbox(o->sprite, &l, &t, &r, &b);
    /* The hook already adds rec[+0x1C]/[+0x1E] (0x192E does it at 0x1969/0x1993). */
    o->bbox_l = (int16_t)(o->x + l);
    o->bbox_r = (int16_t)(o->x + r);
    o->bbox_t = (int16_t)(o->y + t);
    o->bbox_b = (int16_t)(o->y + b);
    /* 0x199F/0x19A7/0x19AF/0x19B7: `shr ax,cl` -- LOGICAL >>8.  With a
       negative bbox edge the original therefore gets 0xFF (255), not -1;
       arithmetic shifts changed the Obj_PushOutX/Y scan windows. */
    o->tx_l = (int16_t)((uint16_t)o->bbox_l >> 8);
    o->tx_r = (int16_t)((uint16_t)o->bbox_r >> 8);
    o->tx_t = (int16_t)((uint16_t)o->bbox_t >> 8);
    o->tx_b = (int16_t)((uint16_t)o->bbox_b >> 8);
}

/* 0x2585 -- frame delta:
 *   [0x905C] = [0x8C66]*0x1F4 + 0x8C70 ; *[0x905C] = 0
 *   ticks = [0x2C8F]:[0x2C8D] & 0xFFFE
 *   [0x8C6A] = (ticks - [0x8C40]:[0x8C3E]) >> 1
 *   [0x8C40]:[0x8C3E] = ticks
 * That is, the delta is the number of 2-tick timer intervals. */
uint32_t dd2_ticks;                  /* [0x2C8D]/[0x2C8F] */
static uint32_t s_ticks_prev;        /* [0x8C3E]/[0x8C40] */

void Obj_Delta(void)
{
    uint32_t now = dd2_ticks & 0xFFFFFFFEu;
    dd2_frame_delta = (int)((now - s_ticks_prev) >> 1);   /* [0x8C6A] */
    s_ticks_prev = now;
}

/* 0x2214-0x2226 (level start): [0x8C6A] = 0, [0x8C3E]/[0x8C40] = 0,
   [0x2C8D]/[0x2C8F] = 0.  Without it the startup frame 0x226E on levels
   2..8 inherited the leftover delta / loading time and moved the objects
   one step too far. */
void Obj_DeltaReset(void)
{
    dd2_frame_delta = 0;              /* [0x8C6A] */
    dd2_ticks = 0;                    /* [0x2C8D]/[0x2C8F] */
    s_ticks_prev = 0;                 /* [0x8C3E]/[0x8C40] */
}

/* 0x1D76: universal object update (animation + speeds + call of the per-frame
   function).  Ported from the disassembly, see PORT_PLAN.md 12/12.1. */
void Obj_SetState(dd2_obj *o, int rec);

/* -------------------------------------------------------------------------
 * 0x1AC9 -- HORIZONTAL push-out of the object from solid tiles.
 * Both halves adjust x (flags [0x8C08]/[0x8C0C] -- hit right/left).
 * Fields +0x22 (f22) and +0x26 (f26) are left>>8/right>>8 BEFORE the move;
 * tx_l/tx_r are after it (0x192E recomputes them).  Exactly those columns that
 * the object "acquired" during this frame are scanned.
 *   right: di = f26+1 .. tx_r, si = tx_t .. tx_b, east table ([0x4DDC])
 *           dx = (di << 8) + 1 - right ;  x -= dx ; left -= dx ; right -= dx
 *           tx_r = di - 1 ; [0x8C08] = 1
 *   left:  di = f22-1 .. tx_l, si = tx_t .. tx_b, west table ([0x4DD8])
 *           dx = ((di + 1) << 8) - left ; x += dx ; left += dx ; right += dx
 *           tx_l = di + 1 ; [0x8C0C] = 1
 * ---------------------------------------------------------------------- */
int Obj_PushOutX(dd2_obj *o, const uint16_t *tiles, const uint8_t *east,
                 const uint8_t *west, int w, int h)
{
    int di, si, hit = 0;
    if (!o || !tiles || !east || !west) return 0;

    for (di = o->f26 + 1; di <= o->tx_r; di++) {          /* 0x1AD0 */
        for (si = o->tx_t; si <= o->tx_b; si++) {         /* 0x1AD8 */
            int tile, dx, idx;
            /* 0x1ADE: ax = si * w ; ax += di -- the original indexes the tile
               plane FLAT, with no per-coordinate bounds check.  di == w wraps
               into the next row (that is what stops an object at the right
               edge of the map); only a flat index outside the plane is
               treated as empty.  The old `di >= w` skip let objects walk off
               the right edge (enemies did, see `make sweep`). */
            idx = si * w + di;
            if (idx < 0 || idx >= w * h) continue;
            tile = tiles[idx];
            if (tile > 857 || east[tile] == 0) continue;
            /* 0x1AFB: ax = di << 8 ; dx = bbox_r + 1 ; dx -= ax ; x -= dx.
               Pushing the object LEFT until bbox_r sits at di*256 - 1.  The
               operands used to be swapped here ((di<<8)+1 - bbox_r), so the
               correction had the wrong sign and drove the object INTO the
               wall -- on the right edge of the map Dave walked out of the
               level and fell forever. */
            dx = o->bbox_r + 1 - (di << 8);
            o->x -= (int16_t)dx;                          /* 0x1B0E */
            o->bbox_l -= (int16_t)dx;
            o->bbox_r -= (int16_t)dx;
            o->tx_r = (int16_t)(di - 1);                  /* 0x1B1A */
            o->hit_right = 1;                             /* 0x1B20 */
            hit = 1;
            goto left_side;
        }
    }
left_side:
    for (di = o->f22 - 1; di >= o->tx_l; di--) {          /* 0x1B36 */
        for (si = o->tx_t; si <= o->tx_b; si++) {         /* 0x1B3E */
            int tile, dx, idx;
            idx = si * w + di;                            /* 0x1B44: flat */
            if (idx < 0 || idx >= w * h) continue;
            tile = tiles[idx];
            if (tile > 857 || west[tile] == 0) continue;
            dx = ((di + 1) << 8) - o->bbox_l;             /* 0x1B61 */
            o->x += (int16_t)dx;                          /* 0x1B6F */
            o->bbox_l += (int16_t)dx;
            o->bbox_r += (int16_t)dx;
            o->tx_l = (int16_t)(di + 1);                  /* 0x1B7B */
            o->hit_left = 1;                              /* 0x1B81 */
            hit = 1;
            goto done;
        }
    }
done:
    return hit;
}

/* 0x1D76: universal object update (animation + speeds + per-frame handler +
   application of the movement).  See the detailed block comment above. */
void Obj_Update(dd2_obj *o, const unsigned char *ai_ignored, void (*perframe)(dd2_obj *))
{
    dd2_update_calls++;
    const unsigned char *ai;
    int delay, si = 0, prev;
    (void)ai_ignored;
    if (!o || o->info < DD2_ACTORINFO_BASE) return;
    if (o->info + 20 > DD2_ACTORINFO_BASE + DD2_ACTORINFO_LEN) return;

    /* [0x8C0E]/[0x8C10] are also fields of the SCRATCH OBJECT (+0x3A dir, +0x3C),
       not globals: each object has its own direction.  While they were shared,
       an object moved in the direction of the PREVIOUS one processed
       (a zombie with dir=-1 moved to the right). */
    dd2_dir_x = o->dir;
    dd2_dir_y = o->f3c;
    /* 01D80..01D89: all four hit flags are cleared at the START of the update.
       Without this they stuck after the very first collision, and the AI
       conditions (0x2F11: "hit left AND dir_x == -1") triggered always. */
    o->hit_left = o->hit_right = 0;
    o->hit_above = o->hit_below = 0;
    o->f40 = (int16_t)(o->f40 + dd2_frame_delta);   /* 01d8c: [0x8C14] += [0x8C6A] */
    ai = dd2_actorinfo_blob + (o->info - DD2_ACTORINFO_BASE);   /* [0x8C18] */
    delay = ai[6] | (ai[7] << 8);                   /* 01d9a */
    if (delay > o->f40) {                           /* 01d9d: not yet time -> 0x1E5B */
        /* IMPORTANT: the delay gates ONLY the computation of xmove/ymove from
           the record and the transition along next.  The handler (fn) and the
           application of the movement (0x2E07) are called EVERY frame -- this is
           clear from the 0x1E5B code:
               if info[+4] == 0 -> straight to the object copy
               Obj_Box ; frame copy ; xmove = 0 ; ymove = 0
               if info[+0xC] != 0 -> call word ptr [bx+0xC]   ; handler
           0x1E93: call 0x2E07
           Previously the port exited entirely here, so for states with a large
           delay (fall 0x898, delay 60) neither physics nor touch handling
           worked -- hence falling through the floor. */
        if (ai[4] | ai[5]) {                        /* 01e5f: info[+4] != 0 */
            Obj_Box(o);                             /* 01e65 */
            SaveOldEdges(o);                        /* 01e74: the 16-byte copy +0x0A.. -> +0x1A.. */
            memcpy(dd2_prev_frame, dd2_cur_frame, sizeof dd2_prev_frame);
            dd2_xmove = 0;                          /* 01e7a */
            dd2_ymove = 0;
            if (ai[0x0C] | (ai[0x0D] << 8)) {       /* 01e8a: info[+0xC] != 0 */
                o->dir = (int16_t)dd2_dir_x;
                o->f3c = (int16_t)dd2_dir_y;
                if (perframe) perframe(o);          /* 01e90 */
            }
            /* 01e93: apply the movement (0x2E07) */
            if (dd2_xmove >  0x100) dd2_xmove =  0x100;
            else if (dd2_xmove < -0x100) dd2_xmove = -0x100;
            if (dd2_ymove >  0x100) dd2_ymove =  0x100;
            else if (dd2_ymove < -0x100) dd2_ymove = -0x100;
            o->x = (int16_t)(o->x + dd2_xmove);
            o->y = (int16_t)(o->y + dd2_ymove);
            o->dir = (int16_t)dd2_dir_x;
            o->f3c = (int16_t)dd2_dir_y;
            Obj_Box(o);
        }
        /* 01e63: if info[+4] == 0 -- the original jumps STRAIGHT to the object
           copy (0x1E96), bypassing both the handler and the application of the
           movement.  Previously the port applied the movement in this case too,
           so the recoil on shooting (record 0x8D4: xs = -64, delay = 12)
           accumulated every frame -- Dave was "knocked back" too hard. */
        return;                                     /* -> 0x1E96 */
    }
    o->f40 = (int16_t)(o->f40 - delay);             /* 01da9 */
    Obj_Box(o);                                     /* 01dad */
    SaveOldEdges(o);                                /* 0x1870: +0x22/+0x26 */
    /* 01dbc: 16 bytes from 0x8BEE to 0x8BDE -- previous x/y/b box for the delta */
    memcpy(dd2_prev_frame, dd2_cur_frame, sizeof dd2_prev_frame);

    /* 01dc6/01dd4: movement = record speed * direction sign */
    dd2_xmove = (int16_t)((int16_t)(ai[8] | (ai[9] << 8)) * dd2_dir_x);
    dd2_ymove = (int16_t)((int16_t)(ai[0x0A] | (ai[0x0B] << 8)) * dd2_dir_y);
    if (delay == 0) {                               /* 01dde: delay 0 -- an "instant" record */
        o->f40 = 0;                                 /* 01de8 */
        dd2_xmove = (int16_t)(dd2_xmove * dd2_frame_delta);   /* 01dee */
        dd2_ymove = (int16_t)(dd2_ymove * dd2_frame_delta);   /* 01df8 */
    }

    if (ai[0x0C] || ai[0x0D]) {                     /* 01e02: there is a per-frame function */
        if (o->f42 != 0) {                          /* 01e0c: the call is deferred */
            o->f42--;                               /* 01e13 */
            si = 1;                                 /* 01e34 */
        } else {
            prev = o->info;                         /* 01e19: save the record */
            if (perframe) perframe(o);              /* 01e23: call [bx+0xC] */
            if (prev == o->info) si = 1;            /* 01e29: the state did not change */
        }
    } else si = 1;                                  /* 01e0a -> 01e34 */

    if (si) {                                       /* 01e37 */
        ai = dd2_actorinfo_blob + (o->info - DD2_ACTORINFO_BASE);
        {
            int next = ai[0x12] | (ai[0x13] << 8);  /* 01e3f: info[+0x12] */
            if (next) Obj_SetState(o, next);        /* 01e48: transition along the chain */
            else {                                  /* 01e4f: the chain ended */
                /* [0x8BD4] = 0 and [0x8BD6] = 0 -- these are +0x00 (type) and
                   +0x02 (active) of the scratch object, i.e. the object is removed. */
                o->type = 0;
                o->f02 = 0;
                dd2_chain_end = 1;
            }
        }
    }

    /* 01E93: call 0x2E07 -- APPLYING THE MOVEMENT.  This is done by 0x1D76
       itself, not by the handler: the only call to 0x2E07 in the whole game is
       from here.  Without this step objects did not move (the enemies stood
       still), because the handlers only set [0x8BFE]/[0x8C00] and the direction. */
    if (dd2_xmove >  0x100) dd2_xmove =  0x100;
    else if (dd2_xmove < -0x100) dd2_xmove = -0x100;
    if (dd2_ymove >  0x100) dd2_ymove =  0x100;
    else if (dd2_ymove < -0x100) dd2_ymove = -0x100;
    o->x = (int16_t)(o->x + dd2_xmove);
    o->y = (int16_t)(o->y + dd2_ymove);
    o->dir = (int16_t)dd2_dir_x;                    /* back into the object */
    o->f3c = (int16_t)dd2_dir_y;
    Obj_Box(o);                                     /* 0x2E51: call 0x192E */
}

void Obj_SetState(dd2_obj *o, int rec)
{
    const unsigned char *ai;
    int delay;
    if (!o || rec < DD2_ACTORINFO_BASE) return;
    if (rec + 20 > DD2_ACTORINFO_BASE + DD2_ACTORINFO_LEN) return;
    ai = dd2_actorinfo_blob + (rec - DD2_ACTORINFO_BASE);
    o->info = (int16_t)rec;                                   /* [0x8C18] */
    /* 0x1D33: the sprite comes from the CURRENT object's direction [0x8C0E]
       (scratch +0x3A).  The port keeps that value in dd2_dir_x while an object
       is being processed (loaded at the start of Obj_Update / the frame
       passes), so that is what is read here. */
    o->sprite = (int16_t)((dd2_dir_x > 0) ? (ai[2] | (ai[3] << 8))
                                          : (ai[0] | (ai[1] << 8)));  /* [0x8BD8] */
    delay = ai[6] | (ai[7] << 8);
    if (delay < o->f40) o->f40 = (int16_t)(delay - 1);        /* [0x8C14] */
    o->react     = (int16_t)(ai[0x10] | (ai[0x11] << 8));     /* [0x8C2C] */
    /* 0x1D6A: the pair reaction (+0x56) comes from record[+0x0E]. */
    o->react_obj = (int16_t)(ai[0x0E] | (ai[0x0F] << 8));     /* [0x8C2A] */
    o->f22 = o->tx_l; o->f24 = o->tx_t;                   /* see 0x1870 */
    o->f26 = o->tx_r; o->f28 = o->tx_b;
}


int dd2_vp_l;                 /* [0x8C44] */
int dd2_vp_t;                 /* [0x8C68] */

void Obj_ClearAll(void)
{
    memset(dd2_objs, 0, sizeof dd2_objs);
    memset(&dd2_cur, 0, sizeof dd2_cur);
    dd2_obj_count = dd2_obj_cursor = 0;
}

/* -------------------------------------------------------------------------
 * 0x1EAB -- the frame.  Fully analysed (see PORT_PLAN.md 17.1).
 *
 *   [0x8BB8] = 8 ; [0x8BC6] = 0
 *   0x2585()                       -- time delta -> [0x8C6A] (called by the caller)
 *   0x754B(DS:0x8BB2, 1, &in)      -- input                (called by the caller)
 *   end = [0x8BCA]*90 + 0x520A
 *
 *   Pass 0: culling by the camera box + 0x1D76 (animation/per-frame)
 *   Pass 1: pairwise 0x18DC(a,b); on overlap -- call [a+0x56](a,b) and
 *             call [b+0x56](b,a)
 *   Pass 2: for each object with [+0x58] -- copy into scratch 0x8BD4,
 *             call [0x8C2C] (= scratch+0x58), copy back
 *
 * The array starts at DS:0x520A (NOT 0x5264 -- correction to 1), record size
 * 90 bytes, type 0 = slot free, [+2] = "active".
 *
 * The reactions themselves (the values of [+0x56] and [+0x58]) are far addresses
 * of functions that are ported in item 3.  Until they are ported, the dispatcher
 * calls weak stubs so that the frame structure is exactly the original one.
 * ---------------------------------------------------------------------- */
void (*dd2_pair_react)(dd2_obj *self, dd2_obj *other);   /* call far [obj+0x56] */
void (*dd2_frame_react)(dd2_obj *self);                  /* call far [0x8C2C]   */

/* 0x18DC -- intersection of the bboxes of two objects, verbatim:
     if (a[+0x0A] == 0 || b[+0x0A] == 0) return 0;
     if (a[+0x0E] <  b[+0x0A]) return 0;   ; a.right < b.left
     if (a[+0x10] <  b[+0x0C]) return 0;   ; a.bottom < b.top
     if (a[+0x0A] >  b[+0x0E]) return 0;   ; a.left > b.right
     if (a[+0x0C] >  b[+0x10]) return 0;   ; a.top > b.bottom
     return 1; */
int Obj_Overlap(const dd2_obj *a, const dd2_obj *b)
{
    /* 0x18DC: `jae`/`jbe` are UNSIGNED 16-bit compares. */
    if (a->bbox_l == 0 || b->bbox_l == 0) return 0;
    if ((uint16_t)a->bbox_r < (uint16_t)b->bbox_l) return 0;
    if ((uint16_t)a->bbox_b < (uint16_t)b->bbox_t) return 0;
    if ((uint16_t)a->bbox_l > (uint16_t)b->bbox_r) return 0;
    if ((uint16_t)a->bbox_t > (uint16_t)b->bbox_b) return 0;
    return 1;
}

/* 0x1EAB */
void Obj_Frame(void)
{
    int i, j, n = dd2_obj_count;

    /* --- pass 0: culling by the camera + 0x1D76 ----------------------- */
    for (i = 0; i < n; i++) {
        dd2_obj *o = &dd2_objs[i];
        const unsigned char *ai;
        if (o->type == 0) continue;
        /* 0x1F1D: the window test runs ONLY while the object is still inactive.
           As soon as it became visible it is marked active (+0x02 = 1, 0x1F54)
           and from then on 0x1D76 runs every frame, even when it leaves the
           screen again.  (Previously f02 was never set here, so pass 2's
           reactions and pass 1's pair reactions were dead for every spawned
           enemy, and active objects froze when they scrolled out.) */
        if (o->f02 == 0) {
            if (dd2_vp_l - 2 < o->tx_r && dd2_vp_t - 2 < o->tx_b &&
                dd2_vp_l + 0x17 > o->tx_l && dd2_vp_t + 0x0C > o->tx_t)
                o->f02 = 1;                                /* [0x8BD6] = 1 */
        }
        if (o->f02 == 0) continue;
        dd2_visible_calls++;
        if (o->info < DD2_ACTORINFO_BASE) continue;
        ai = dd2_actorinfo_blob + (o->info - DD2_ACTORINFO_BASE);
        Obj_Update(o, ai, dd2_obj_perframe);               /* 0x1D76 */
    }

    /* --- pass 1: object versus object --------------------------------- */
    for (i = 0; i < n; i++) {
        dd2_obj *a = &dd2_objs[i];
        if (a->f02 == 0) continue;
        for (j = i + 1; j < n; j++) {
            dd2_obj *b = &dd2_objs[j];
            if (b->f02 == 0) continue;
            if (!Obj_Overlap(a, b)) continue;
            if (a->react_obj && dd2_pair_react) {
                memcpy(&dd2_cur, a, sizeof dd2_cur);
                dd2_dir_x = dd2_cur.dir; dd2_dir_y = dd2_cur.f3c;
                dd2_pair_react(&dd2_cur, b);
                dd2_cur.dir = (int16_t)dd2_dir_x; dd2_cur.f3c = (int16_t)dd2_dir_y;
                memcpy(a, &dd2_cur, sizeof dd2_cur);
            }
            if (b->react_obj && dd2_pair_react) {
                memcpy(&dd2_cur, b, sizeof dd2_cur);
                dd2_dir_x = dd2_cur.dir; dd2_dir_y = dd2_cur.f3c;
                dd2_pair_react(&dd2_cur, a);
                dd2_cur.dir = (int16_t)dd2_dir_x; dd2_cur.f3c = (int16_t)dd2_dir_y;
                memcpy(b, &dd2_cur, sizeof dd2_cur);
            }
        }
    }

    /* --- pass 2: per-frame reaction through scratch -------------------- */
    for (i = n - 1; i >= 0; i--) {
        dd2_obj *o = &dd2_objs[i];
        if (o->f02 == 0 || o->react == 0) continue;
        dd2_react_slot = i;
        memcpy(&dd2_cur, o, sizeof dd2_cur);               /* 0x1F0D */
        /* dir/dir_y are fields of the scratch object (+0x3A/+0x3C); the port
           keeps them in the dd2_dir_x/y globals, so they must be loaded before
           and stored back after each reaction -- otherwise the reactions'
           turn decisions are lost. */
        dd2_dir_x = dd2_cur.dir; dd2_dir_y = dd2_cur.f3c;
        /* 0x307A: the original decides flash-vs-normal BEFORE it subtracts
           the delta from [+0x46], so the frame the timer reaches 0 still
           flashes (20 frames, not 19). */
        dd2_cur.flash_draw = (dd2_cur.timer != 0) ? 1 : 0;
        if (dd2_frame_react) dd2_frame_react(&dd2_cur);    /* call [0x8C2C] */
        dd2_cur.dir = (int16_t)dd2_dir_x; dd2_cur.f3c = (int16_t)dd2_dir_y;
        memcpy(o, &dd2_cur, sizeof dd2_cur);               /* 0x1E96 */
    }

    dd2_obj_cursor = 0;                                    /* [0x8BCE] = 0x520A */
    dd2_react_slot = -1;
}

/* ------------------------------------------------------------- self-test */
#ifdef DD2_OBJ_TEST
#include <stdio.h>
int main(void)
{
    int z, d;
    Obj_ClearAll();
    z = Obj_Spawn(2, 100, 200, 1, 0x0C94);      /* ZOMBIE from RE_levels.md */
    d = Obj_Spawn(9, 50, 60, -1, 0x0AB4);       /* TREASURE1 */
    printf("zombie: slot=%d type=%d sprite=%d react=0x%04x info=0x%04x\n",
           z, dd2_objs[z].type, dd2_objs[z].sprite,
           (unsigned short)dd2_objs[z].react, (unsigned short)dd2_objs[z].info);
    printf("treasure: slot=%d sprite=%d (dir=-1)\n", d, dd2_objs[d].sprite);
    printf("count=%d sizeof=%zu\n", dd2_obj_count, sizeof(dd2_obj));
    {
        /* animation timing: for the zombie delay = info[+6]; check that with a
           delay of 3 the update happens once every 3 frames (0x1D76) */
        const unsigned char *ai = dd2_actorinfo_blob + (0x0C94 - DD2_ACTORINFO_BASE);
        int delay = ai[6] | (ai[7] << 8), i, n = 0;
        dd2_frame_delta = 1; dd2_objs[z].f40 = 0;
        for (i = 0; i < 12; i++) {
            int before = dd2_objs[z].f40;
            Obj_Update(&dd2_objs[z], ai, NULL);
            if (dd2_objs[z].f40 != before + 1) n++;   /* an update happened */
        }
        {
            /* 0x3504: the bonus disappears when [+0x4C] > 0x3C */
            dd2_obj b; int i, gone = -1;
            memset(&b, 0, sizeof b);
            b.type = 3; b.f02 = 1;
            dd2_frame_delta = 1;
            for (i = 1; i <= 70; i++) {
                Obj_BonusTimer(&b, 0x3C);
                if (b.type == 0 && gone < 0) gone = i;
            }
            printf("BONUS: vanished on frame %d (expected 61)\n", gone);
        }
        {
            /* 0x46D4: hunchback.  Dave far on X -> go to him; close -> back away */
            dd2_obj h, dd;
            int dirs[2];
            memset(&h, 0, sizeof h); memset(&dd, 0, sizeof dd);
            h.type = 2; h.f02 = 1; h.x = 0;
            h.bbox_b = 100;
            dd.x = 10000; dd.bbox_t = 100; dd.bbox_l = 10000; dd.bbox_r = 10080;
            h.bbox_l = 0; h.bbox_r = 80;
            dd2_dir_x = 1; Obj_HunchbackAI(&h, &dd); dirs[0] = dd2_dir_x;
            dd.x = 100; dd.bbox_l = 100; dd.bbox_r = 180;   /* close */
            h.bbox_l = 0; h.bbox_r = 80;
            dd2_dir_x = 1; Obj_HunchbackAI(&h, &dd); dirs[1] = dd2_dir_x;
            printf("HUNCH: far dir_x=%d (expected 1), near dir_x=%d (expected -1)\n",
                   dirs[0], dirs[1]);
        }
        {
            /* 0x4AE1: wolf.  Dave far -> runs; Dave close -> jump */
            dd2_obj w, dd;
            int d1, d2, st;
            memset(&w, 0, sizeof w); memset(&dd, 0, sizeof dd);
            w.type = 2; w.f02 = 1; w.x = 0; w.bbox_l = 0; w.bbox_r = 80; w.bbox_b = 100;
            dd.bbox_t = 100;
            dd.x = 8000; dd.bbox_l = 8000; dd.bbox_r = 8080;   /* far */
            dd2_dir_x = 1; dd2_ymove = 0; Obj_WolfAI(&w, &dd);
            d1 = dd2_dir_x; st = w.info;
            w.info = 0;
            dd.x = 400; dd.bbox_l = 400; dd.bbox_r = 480;      /* close */
            dd2_dir_x = 1; dd2_ymove = 0; Obj_WolfAI(&w, &dd);
            d2 = dd2_dir_x;
            printf("WOLF: far dir_x=%d info=%#06x (expected 1, 0); near info=%#06x ymove=%d (expected 0xfb4, -65)\n",
                   d1, st, w.info, dd2_ymove);
            (void)d2;
        }
        {
            /* 0x30C0: an object that went off screen is removed, not merely
               skipped; 0x48B2: the slime at the ceiling changes state. */
            dd2_obj q;
            static uint16_t empty_tiles[64];
            static uint8_t zero[1024];
            int far_removed, near_kept, st;
            dd2_vp_l = 10; dd2_vp_t = 10;
            memset(&q, 0, sizeof q); q.type = 2; q.f02 = 1;
            q.tx_l = 200; q.tx_r = 205; q.tx_t = 10; q.tx_b = 12;   /* far to the right */
            far_removed = Obj_OffscreenCheck(&q);
            memset(&q, 0, sizeof q); q.type = 2; q.f02 = 1;
            q.tx_l = 12; q.tx_r = 15; q.tx_t = 12; q.tx_b = 14;     /* on screen */
            near_kept = Obj_OffscreenCheck(&q);
            printf("CULL: far removed=%d (expected 1), on screen removed=%d (expected 0)\n",
                   far_removed, near_kept);

            memset(&q, 0, sizeof q); q.type = 6; q.f02 = 1;
            q.hit_above = 1; dd2_dir_y = -1;
            Obj_SlimeCrawl(&q, empty_tiles, zero, zero, zero, zero, 8, 8);
            st = q.info;
            printf("SLIME at ceiling: info=%#06x (expected 0xe88)\n", st);
        }
        {
            /* 0x4E07: spider.  Dave right below it -> attack downward (0x102C);
               Dave to the right while the spider faces left -> turn. */
            dd2_obj sp, dd;
            int st2, dx2;
            memset(&sp, 0, sizeof sp); memset(&dd, 0, sizeof dd);
            sp.type = 6; sp.f02 = 1; sp.x = 1000; sp.y = 1000;
            dd.x = 1005; dd.y = 1100;                 /* below it, 5 px to the side */
            dd2_dir_y = 0; Obj_SpiderAI(&sp, &dd);
            st2 = sp.info;
            memset(&sp, 0, sizeof sp); sp.type = 6; sp.f02 = 1;
            sp.x = 1000; sp.y = 1000; sp.info = 0x0EE8;
            dd.x = 5000; dd.y = 1000;                 /* Dave to the right */
            dd2_dir_x = -1; Obj_SpiderAI(&sp, &dd);
            dx2 = dd2_dir_x;
            printf("SPIDER: below info=%#06x (expected 0x102c); right dir_x=%d (expected 1)\n",
                   st2, dx2);
        }
        {
            /* 0x4F9A: Frankenstein animation -- on the 11th tick state 0x11D0,
               the timer decreases by 0xA.  0x5006: fireball flight. */
            dd2_obj f;
            int i, st3 = 0;
            memset(&f, 0, sizeof f); f.type = 7; f.f02 = 1;
            for (i = 1; i <= 11; i++) { Obj_FrankAnim(&f); if (f.info) st3 = f.info; }
            printf("FRANK anim: at tick 11 info=%#06x timer=%d (expected 0x11d0, 1)\n",
                   st3, f.f48[2]);
            dd2_dir_x = 1; dd2_dir_y = 0; dd2_frame_delta = 2;
            Obj_SparkFly(0);
            printf("SPARK: xmove=%d ymove=%d (expected 48, 0)\n", dd2_xmove, dd2_ymove);
        }
        {
            /* 0x19C0: gravity.  Over 3 ticks the accumulator is 3+6+9=18, ymove=18.
               0x4C2C is simply a call to gravity. */
            dd2_obj g;
            memset(&g, 0, sizeof g); g.type = 6; g.f02 = 1;
            dd2_ymove = 0; dd2_frame_delta = 3;
            Obj_WolfJump(&g);
            printf("GRAVITY 3 ticks: ymove=%d acc=%d (expected 18, 9)\n",
                   dd2_ymove, g.f2c);
        }
        {
            /* 0x4C30: wolf.  Hit below + a bounce already happened -> turn and
               clear the flag; without a bounce there is no turn. */
            dd2_obj w;
            static uint16_t et[64]; static uint8_t zz[1024];
            int d1, d2, f1;
            memset(&w, 0, sizeof w); w.type = 6; w.f02 = 1;
            w.hit_below = 1; w.f48[5] = 1;
            dd2_dir_x = 1; Obj_WolfBounce(&w, et, zz, zz, zz, zz, 8, 8);
            d1 = dd2_dir_x; f1 = w.f48[5];
            memset(&w, 0, sizeof w); w.type = 6; w.f02 = 1;
            w.hit_below = 1; w.f48[5] = 0;
            dd2_dir_x = 1; Obj_WolfBounce(&w, et, zz, zz, zz, zz, 8, 8);
            d2 = dd2_dir_x;
            printf("WOLF bounce: with bounce dir_x=%d flag=%d (expected -1, 0); without dir_x=%d (expected 1)\n",
                   d1, f1, d2);
        }
        {
            /* 0x4D13: skull.  Dave to the right and below -> acceleration
               right-down; over 3 ticks the accumulators are 3, xmove/ymove are
               6 each, dir_x = 1. */
            dd2_obj k, dd;
            memset(&k, 0, sizeof k); memset(&dd, 0, sizeof dd);
            k.type = 11; k.f02 = 1; k.x = 0; k.y = 1000;
            dd.x = 5000; dd.y = 2000;
            dd2_xmove = 0; dd2_ymove = 0; dd2_dir_x = 0; dd2_frame_delta = 3;
            Obj_SkullAI(&k, &dd);
            printf("SKULL: xmove=%d ymove=%d dir_x=%d (expected 6, 6, 1) acc=%d,%d\n",
                   dd2_xmove, dd2_ymove, dd2_dir_x, k.f1c, k.f1e);
        }
        printf("zombie delay=%d, updates in 12 frames: %d (expected %d)\n",
               delay ? delay : 1, n, delay ? 12 / delay : 12);
    }
    {
        /* 0x1D2A: state change -- sprite and reaction are taken from the record
           and the direction of the object being processed ([0x8C0E]). */
        dd2_dir_x = 1;
        Obj_SetState(&dd2_objs[z], 0x0C94);     /* ZOMBIE, standing */
        printf("SetState(0x0C94): sprite=%d react=0x%04x\n",
               dd2_objs[z].sprite, (unsigned short)dd2_objs[z].react);
        dd2_dir_x = -1;
        Obj_SetState(&dd2_objs[z], 0x0CA8);     /* ZOMBIE, another phase */
        printf("SetState(0x0CA8) dir=left: sprite=%d react=0x%04x\n",
               dd2_objs[z].sprite, (unsigned short)dd2_objs[z].react);
    }
    {
        /* 0x18DC: AABB intersection (order left/top/right/bottom) */
        dd2_obj a, b;
        memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        a.bbox_l = 100; a.bbox_t = 100; a.bbox_r = 120; a.bbox_b = 140;
        b.bbox_l = 110; b.bbox_t = 130; b.bbox_r = 130; b.bbox_b = 160;
        printf("Overlap(intersect) = %d (expected 1)\n", Obj_Overlap(&a, &b));
        b.bbox_l = 121; b.bbox_t = 130; b.bbox_r = 140; b.bbox_b = 160;
        printf("Overlap(apart)     = %d (expected 0)\n", Obj_Overlap(&a, &b));
        memset(&b, 0, sizeof b);
        printf("Overlap(none bbox)     = %d (expected 0)\n", Obj_Overlap(&a, &b));
    }
    {
        /* 0x2585: delta = (ticks - previous)/2 */
        dd2_ticks = 1000; Obj_Delta();
        printf("Delta: %d (expected 500)\n", dd2_frame_delta);
        dd2_ticks = 1004; Obj_Delta();
        printf("Delta: %d (expected 2)\n", dd2_frame_delta);
    }
    return 0;
}
#endif
