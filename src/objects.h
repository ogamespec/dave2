/*
 * objects.h -- DD2 object system, object structure 1:1 with the original.
 *
 * Array: 164 objects of 90 bytes each, DS:0x520A (counter [0x8BCA], cursor
 * [0x8BCE], end [0x8BCC]) -- the base was corrected per 0x1EAB, see PORT_PLAN 17.1.
 * The structure is copied wholesale into the scratch area DS:0x8BD4 (90 bytes),
 * so the size must match to the byte.
 *
 *   +0x00 type      +0x04 sprite    +0x06 x         +0x08 y
 *   +0x0A bbox.l    +0x0C bbox.t    +0x0E bbox.r    +0x10 bbox.b
 *      (the order left/top/right/bottom is confirmed by 0x18DC: it compares
 *       a[+0x0E]>=b[+0x0A], a[+0x10]>=b[+0x0C], a[+0x0A]<=b[+0x0E],
 *       a[+0x0C]<=b[+0x10] -- this is exactly an AABB)
 *   +0x12..+0x18 tile coordinates
 *   +0x2E xmove     +0x30 ymove
 *   +0x32 hitAbove  +0x34 hitLeft   +0x36 hitBelow  +0x38 hitRight
 *   +0x3A dir       +0x3E health    +0x44 info (20-byte record)
 *   +0x46 timer     +0x56 react(obj-vs-obj)  +0x58 react(per-frame)
 */
#ifndef DD2_OBJECTS_H
#define DD2_OBJECTS_H
#include <stdint.h>

#define DD2_MAXOBJ   164
#define DD2_OBJSIZE  90
#define DD2_SCRATCH  0x8BD4     /* DS: scratch copy of the current object */

typedef struct {
    int16_t  type;              /* +0x00 */
    int16_t  f02;               /* +0x02 */
    int16_t  sprite;            /* +0x04 */
    int16_t  x, y;              /* +0x06, +0x08 */
    int16_t  bbox_l, bbox_t;    /* +0x0A, +0x0C */
    int16_t  bbox_r, bbox_b;    /* +0x0E, +0x10 */
    int16_t  tx_l, tx_t;        /* +0x12 left>>8,  +0x14 top>>8    (0x192E) */
    int16_t  tx_r, tx_b;        /* +0x16 right>>8, +0x18 bottom>>8 (0x192E) */
    int16_t  f1a, f1c, f1e, f20, f22, f24, f26, f28, f2a, f2c;  /* +0x1A..+0x2C */
    int16_t  xmove, ymove;      /* +0x2E, +0x30 */
    /* Offsets as in the original (scratch 0x8BD4): +0x32 hit_above,
       +0x34 hit_RIGHT, +0x36 hit_below, +0x38 hit_LEFT.  Previously left/right
       were swapped -- 0x1AC9 sets +0x34 when pushing out to the left, and the
       conditions of 0x2F11 compare them with the direction. */
    int16_t  hit_above, hit_right;  /* +0x32, +0x34 */
    int16_t  hit_below, hit_left;   /* +0x36, +0x38 */
    int16_t  dir;               /* +0x3A */
    int16_t  f3c;               /* +0x3C */
    int16_t  health;            /* +0x3E -- decreased on a hit */
    int16_t  f40, f42;          /* +0x40, +0x42 */
    int16_t  info;              /* +0x44 -- offset of the actor-info record */
    int16_t  timer;             /* +0x46 */
    int16_t  f48[7];            /* +0x48..+0x54 */
    int16_t  react_obj;         /* +0x56 -- object-vs-object reaction */
    int16_t  react;             /* +0x58 -- per-frame reaction (end: +0x5A = 90) */
} dd2_obj;

extern dd2_obj dd2_objs[DD2_MAXOBJ];   /* DS:0x520A */
extern dd2_obj dd2_cur;                /* DS:0x8BD4 (scratch) */
extern int     dd2_obj_count;          /* [0x8BCA] */
extern int     dd2_update_calls, dd2_visible_calls;   /* debug */
extern int     dd2_dir_x, dd2_dir_y;   /* [0x8C0E]/[0x8C10] -- direction signs */
extern int16_t dd2_xmove, dd2_ymove;   /* [0x8BFE]/[0x8C00] -- movement */
extern void  (*dd2_obj_perframe)(dd2_obj *o);   /* call word ptr [bx+0xC] */
extern int16_t dd2_prev_frame[8], dd2_cur_frame[8];  /* 0x8BDE/0x8BEE */
extern int     dd2_chain_end;          /* 0x1E4F: the record has no next */
extern int     dd2_obj_cursor;         /* [0x8BBE] */
extern int     dd2_react_slot;         /* the slot whose reaction is running */

/* Frame globals of the original (DS addresses in the comments) */
extern int     dd2_vp_l, dd2_vp_t;     /* [0x8C44], [0x8C68] -- camera box */

/* 0x1EAB -- frame: three passes (see PORT_PLAN.md 17.1) */
void Obj_ClearAll(void);
void Obj_Frame(void);
int  Obj_Alloc(void);
int  Obj_Spawn(int type, int x, int y, int dir, int info);

/* 0x2585 -- frame delta: [0x8C6A] = (ticks - previous) / 2.
   The tick source is provided by the caller (in the original -- a 32-bit
   counter maintained by the timer ISR; [0x2C8D]/[0x2C8F]). */
extern uint32_t dd2_ticks;            /* [0x2C8D]/[0x2C8F] */
void Obj_Delta(void);

/* 0x18DC -- intersection of the bboxes of two objects (AABB). */
int  Obj_Overlap(const dd2_obj *a, const dd2_obj *b);

/* 0x192E -- recompute the object's bbox from the sprite. */
void Obj_Box(dd2_obj *o);
extern int (*dd2_pushout)(dd2_obj *o);      /* 0x1AC9 with tables from the assets */
int  Obj_GroundBelow(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                     int w, int h);                      /* 0x1B9D */
void Obj_WalkerReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                     const uint8_t *east, const uint8_t *west, int w, int h,
                     int stop_at_wall);                  /* 0x4338/0x4380 */
void Obj_SkullAI(dd2_obj *o, dd2_obj *dave);        /* 0x4D13 */
void Obj_Gravity(dd2_obj *o);                      /* 0x19C0 */
void Obj_WolfJump(dd2_obj *o);                     /* 0x4C2C */
void Obj_WolfBounce(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h);      /* 0x4C30 */
extern int dd2_door_flag;                          /* [0x8C38] (defined in level.c) */
extern int dd2_lives;                              /* [0x8BC8] */
extern unsigned int dd2_score_lo, dd2_score_hi;    /* [0x9320..0x9322] */
void Obj_ChunkFan(dd2_obj *src);                  /* 0x3125 */
void Obj_PairReact(dd2_obj *self, dd2_obj *other); /* 0x41E7 / 0x49E4 */
void Score_Award(int x, int y, int kind);        /* 0x3578 */
void score_extra_reset(void);                      /* [0x8C3A..0x8C3C] = 0 */
void Obj_MastClaw(dd2_obj *o, dd2_obj *dave);      /* 0x52D2 */
void Obj_GhostChase(dd2_obj *o, dd2_obj *dave);    /* 0x4A0F */
void Obj_RopeReact1(dd2_obj *o, const uint16_t *tiles, const uint8_t *south,
                    const uint8_t *east, const uint8_t *west,
                    int w, int h);                             /* 0x4E73 */
void Obj_RopeReact2(dd2_obj *o, const uint16_t *tiles, const uint8_t *east,
                    const uint8_t *west, int w, int h);        /* 0x4F0A */
void Obj_MastFire(dd2_obj *o);
extern int dd2_master_slot1, dd2_master_slot2;     /* [0x8C20]/[0x8C22] */
void Obj_MasterWalk(dd2_obj *o);                   /* 0x50D8 */
void Obj_MasterAttack(dd2_obj *o);                 /* 0x51FF */                     /* 0x5377 */
void Obj_DelbertReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                      const uint8_t *south, int w, int h);  /* 0x53D2 */
void Obj_DelbertWin(dd2_obj *o);                   /* 0x53F8 */
void Obj_FaceDave(dd2_obj *o, dd2_obj *dave);      /* 0x310D */
void Obj_TimerCullReact(dd2_obj *o);               /* 0x2E6B */
void Obj_SlimeSpiderReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *south,
                          const uint8_t *north, const uint8_t *east,
                          const uint8_t *west, int w, int h);  /* 0x2FF6 */
void Obj_SkullReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h);       /* 0x4DB1 */
void Obj_FrankSparkReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                         const uint8_t *south, const uint8_t *east,
                         const uint8_t *west, int w, int h);  /* 0x503B */
void Obj_WolfReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                   const uint8_t *east, const uint8_t *west,
                   int w, int h);                             /* 0x4B4B */
void Obj_ChunkFly(dd2_obj *o, int fall_state);     /* 0x366B */
void Obj_SparkChunkFly(dd2_obj *o);                /* 0x3696 */
void Obj_ChunkLand(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                   const uint8_t *south, const uint8_t *east,
                   const uint8_t *west, int w, int h, int land_state);  /* 0x36A4 */
void Obj_SparkReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h);       /* 0x3707 */
void Obj_ZombieDownReact(dd2_obj *o, dd2_obj *dave, const uint16_t *tiles,
                         const uint8_t *south, const uint8_t *north,
                         int w, int h);                       /* 0x4623 */
void Obj_HunchThrow(dd2_obj *o);                              /* 0x476B */
void Obj_KnifeReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *east,
                    const uint8_t *west, int w, int h);       /* 0x47B6 */
void Obj_GhostSolid(dd2_obj *o);                              /* 0x49D1 */
void Obj_GhostStartDave(dd2_obj *o, dd2_obj *dave);           /* 0x496F */
void Obj_FrankAnim(dd2_obj *o);                    /* 0x4F9A */
void Obj_FrankThrow(dd2_obj *o);                   /* 0x4FB4 */
void Obj_SparkFly(dd2_obj *o);                     /* 0x5006 */
void Obj_SparkTimer(dd2_obj *o);                   /* 0x5025 */
void Obj_SpiderAI(dd2_obj *o, dd2_obj *dave);      /* 0x4E07 */
int  Obj_OffscreenCheck(dd2_obj *o);              /* 0x30C0 */
void Obj_DrawCullReact(dd2_obj *o);               /* 0x2E55 */
void Obj_ReloadReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                     const uint8_t *south, const uint8_t *east,
                     const uint8_t *west, int w, int h);      /* 0x2EC9 */
void Obj_SlimeWander(dd2_obj *o);                 /* 0x4878 */
void Obj_SlimeCrawl(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h);   /* 0x48B2 */
void Obj_WolfAI(dd2_obj *o, dd2_obj *dave);        /* 0x4AE1 */
void Obj_HunchbackAI(dd2_obj *o, dd2_obj *dave);  /* 0x46D4 */
void Obj_BonusTimer(dd2_obj *o, int ttl_limit);   /* 0x3504 / 0x351E */
void Obj_ZombieAI(dd2_obj *o, dd2_obj *dave, const uint16_t *tiles,
                  const uint8_t *south, const uint8_t *north,
                  int w, int h);                     /* 0x452A */   /* 0x452A (partial) */
void Obj_WalkAI(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                const uint8_t *east, const uint8_t *west, int w, int h);  /* 0x2F11 */
int  Obj_PushOutX(dd2_obj *o, const uint16_t *tiles, const uint8_t *east,
                 const uint8_t *west, int w, int h);   /* 0x1AC9 */
int  Obj_PushOutY(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                 const uint8_t *south, int w, int h);  /* 0x19EE */

/* Reactions -- far addresses in the original ([+0x56] and [+0x58]); not ported
   yet (item 3), the dispatcher calls these stubs. */
extern void (*dd2_pair_react)(dd2_obj *self, dd2_obj *other);
extern void (*dd2_frame_react)(dd2_obj *self);

/* 0x192E takes the sprite bbox from the spriteInfo table (DS:0x97B0).  The port
   builds it itself from the sprite mask (dd2.c: bx0/by0/bx1/by1), the values
   arrive here. */
extern void (*dd2_sprite_bbox)(int sprite, int *l, int *t, int *r, int *b);

#endif /* DD2_OBJECTS_H *//* 0x2B0C from the handler: the object draws a series of sprites itself (rope etc.) */
extern void (*dd2_extra_sprite)(int spr, int wx, int wy);


