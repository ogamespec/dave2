/*
 * dave.c -- Dave's states and physics, ported from the original DD2.EXE.
 *
 * Addresses in comments are offsets in the image re/ref/orig/DAVE2_IMAGE.bin.
 * Globals are named after DS addresses, as in the disassembler, so that
 * cross-checking is mechanical.  Object array slot 0 (DS:0x520A) is Dave;
 * that is exactly why hitscan 0x3C4C starts scanning at 0x5264 (slot 1).
 */
#include "dave.h"
#include <stdlib.h>
#include "objects.h"
#include "level.h"
#include "dd2.h"
#include "dd2_actorinfo.h"
#include <string.h>

/* --- input ------------------------------------------------------------- */
int dave_cmd, dave_button1, dave_button2, dave_prev_cmd;
int dave_jump_latch, dave_fire_latch;

/* 0x3B6F: SOUTH table ([0x4A94]) for the pull-up.  Wired up from main.c,
   because it needs the level tiles; it stays NULL in self-tests. */
int (*dd2_tile_south)(int row, int col);

/* --- player object ----------------------------------------------------- */
int dave_x, dave_y, dave_sprite;
int dave_xmove, dave_ymove, dave_jump_accel, dave_jump_vy;
int dd2_hit_slot = -1, dd2_hit_x, dd2_hit_y;   /* [0x9072], [0x9074] */
/* [0x8C04] -- vertical acceleration accumulator (0x19C0).  In the port this is
   a separate variable, because field +0x30 is taken by ymove. */
static int dave_grav;
int dd2_flash_9072, dd2_flash_9074;    /* [0x9072], [0x9074] -- flash counters */
int dave_dir_x = 1, dave_dir_y;
int dave_info;
int dave_frame_delta;
int dave_ammo, dave_idle, dave_thrust;
int dd2_god_mode;                      /* [0x4A8C] -- F12 menu toggle */

/* actor-info records that dispatch 0x37AE leads to (see PORT_PLAN section 19). */
#define REC_STAND       0x0780
#define REC_WALK_E1     0x0794
#define REC_OPEN        0x0834
#define REC_ENTER1      0x0848
#define REC_JUMP1       0x0884
#define REC_AIM_R       0x08D4
#define REC_AIM_UP_R    0x0910
#define REC_AIM_DN_R    0x0960
#define REC_RELOAD      0x09B0

/* sound: [0x8CB2] = sfx player; the port plugs in its own */
void (*dd2_sfx)(int id);
#define SFX(id) do { if (dd2_sfx) dd2_sfx(id); } while (0)

/* SetState/enter state.  0x1D2A. */
static void SetState(int rec)
{
    const unsigned char *ai = dd2_actorinfo_blob + (rec - DD2_ACTORINFO_BASE);
    dd2_obj *o = &dd2_objs[0];
    int delay;
    dave_info = rec;
    dd2_cur.info = (int16_t)rec;
    /* 0x1D33: `cmp [0x8c0e],0 / jle` -- dir <= 0 picks rec[0], dir > 0 rec[2]. */
    dave_sprite = (dave_dir_x > 0) ? (ai[2] | (ai[3] << 8))
                                   : (ai[0] | (ai[1] << 8));
    /* 0x1D4C: pull the frame accumulator down when the new record is sooner. */
    delay = ai[6] | (ai[7] << 8);
    if (delay < o->f40) o->f40 = (int16_t)(delay - 1);
    /* 0x1D67/0x1D71: the reaction and pair-reaction come from the record.
       Without these Dave's pass-2 reaction stayed at 0x4338 (the record 0x780
       value written at spawn), so he ran Obj_WalkerReact in EVERY state
       instead of 0x43A9 (jump/fall), 0x4380 (aim/shoot), 0x2E55 (door) or
       0x2EC9 (reload). */
    o->react     = (int16_t)(ai[0x10] | (ai[0x11] << 8));
    o->react_obj = (int16_t)(ai[0x0E] | (ai[0x0F] << 8));
}

/* 0x2E07: clamp per-frame movement (+-0x100 = +-16 px), apply it to x/y and
   recompute bbox (0x192E).  This is exactly how the object moves in the original. */
void Dave_Move(void)                                   /* 0x2E07 */
{
    if (dave_xmove >  0x100) dave_xmove =  0x100;
    else if (dave_xmove < -0x100) dave_xmove = -0x100;
    if (dave_ymove >  0x100) dave_ymove =  0x100;
    else if (dave_ymove < -0x100) dave_ymove = -0x100;
    dave_x += dave_xmove;
    dave_y += dave_ymove;
    {
        dd2_obj *o = &dd2_objs[0];
        o->x = (int16_t)dave_x;
        o->y = (int16_t)dave_y;
        o->sprite = (int16_t)dave_sprite;
        Obj_Box(o);                                    /* 0x192E */
    }
}

/* 0x3C2D: one reload frame: sound, ammo counter, on the 8th one -- standing. */
void Dave_Reload(void)                                 /* 0x3C2D */
{
    SFX(0x0D);
    dave_ammo++;
    if (dave_ammo == 8) SetState(REC_STAND);
}

/* 0x3C4C: hitscan -- a marching ray, not a single point test.
   Arguments: x, y (1/16 px), dx, dy (step per iteration, also 1/16 px).
   Returns 1 (object hit), -1 (wall hit) or 0 (gave up outside the view window).
   [0x9072]/[0x9074] receive the hit point on both 1 and -1.
   The tile plane and its direction tables are wired up from main.c. */
const uint16_t *dd2_hs_tiles;      /* [0xA6CA] tile plane */
int dd2_hs_w, dd2_hs_h;            /* [0x9EB0], level height */
int (*dd2_hs_solid)(int tile, int dir);  /* NORTH/SOUTH/EAST/WEST tables */

/* dir: 0 = NORTH (0x4A92), 1 = SOUTH (0x4A94), 2 = EAST (0x4DDC), 3 = WEST (0x4DD8) */
static int hs_solid(int row, int col, int dir)
{
    int t;
    if (!dd2_hs_tiles || !dd2_hs_solid) return 0;
    if (row < 0 || col < 0 || row >= dd2_hs_h || col >= dd2_hs_w) return 0;
    t = dd2_hs_tiles[row * dd2_hs_w + col];
    if (t < 0 || t > 857) return 0;
    return dd2_hs_solid(t, dir);
}

int Dave_Hitscan(int x, int y, int dx, int dy)         /* 0x3C4C */
{
    for (;;) {
        int i;
        for (i = 1; i < dd2_obj_count; i++) {          /* 0x3C57: from slot 1 */
            dd2_obj *o = &dd2_objs[i];
            if (o->f02 == 0) continue;                 /* [+2]  active    */
            if (o->f48[0] == 0) continue;              /* [+0x48] vulnerable */
            if (o->bbox_l > x) continue;
            if (o->bbox_r < x) continue;
            if (o->bbox_t > y) continue;
            if (o->bbox_b < y) continue;
            /* 0x3C82: only explosive objects lose health / release the fan. */
            if (o->f48[1] != 0) {
                o->timer = 0x14;                       /* [+0x46] = 0x14 */
                o->health--;                           /* dec [+0x3E] */
                if (o->health == 0)
                    Obj_ChunkFan(o);                   /* 0x3C92 -> 0x3125 */
            }
            dd2_hit_x = x;                             /* [0x9072] */
            dd2_hit_y = y;                             /* [0x9074] */
            return 1;
        }

        /* 0x3CB1: step the point and test the tile the ray is entering. */
        {
            int tx = (x + dx) >> 8;
            int ty = (y + dy) >> 8;
            int solid = 0;

            if ((x >> 8) != tx) {                      /* column changed */
                if (dx > 0) {
                    if (tx - dd2_vp_l > 0x19) return 0;      /* 0x3CF6 */
                    solid = hs_solid(ty, tx, 2);             /* EAST 0x4DDC */
                    if (solid) { x += 0xF - (x & 0xF); goto wall; }
                } else {
                    if (dd2_vp_l - tx > 5) return 0;         /* 0x3D21 */
                    solid = hs_solid(ty, tx, 3);             /* WEST 0x4DD8 */
                    if (solid) { x -= (x & 0xF); goto wall; }
                }
            }
            x += dx;                                   /* 0x3D43 */
            if ((y >> 8) != ty) {                      /* row changed */
                if (dy > 0) {
                    if (ty - dd2_vp_t > 0xE) return 0;       /* 0x3D77 */
                    solid = hs_solid(ty, tx, 0);             /* NORTH 0x4A92 */
                    if (solid) { y += 0xF - (y & 0xF); goto wall; }
                } else {
                    if (dd2_vp_t - ty > 3) return 0;         /* 0x3DA2 */
                    solid = hs_solid(ty, tx, 1);             /* SOUTH 0x4A94 */
                    if (solid) { y -= (y & 0xF); goto wall; }
                }
            }
            y += dy;                                   /* 0x3DC4 */
        }
        continue;
wall:
        dd2_hit_x = x;                                 /* 0x3DCD */
        dd2_hit_y = y;
        return -1;
    }
}

/* 0x3891: common per-frame Dave handler (standing/walking). */
void Dave_Frame(void)                                  /* 0x3891 */
{
    if (dave_button1) {
        if (dave_jump_latch) goto after_jump;
        dave_jump_latch = 1;
        dave_thrust = 0x0E;                            /* [0x907A] = 14 */
        if (dave_cmd == 6 || dave_cmd == 2)
            dave_jump_accel = dave_dir_x * 0x18;       /* [0x8C02] */
        else
            dave_jump_accel = 0;
        dave_grav = -40;                               /* [0x8C04] = 0xFFD8 */
        SetState(REC_JUMP1);
        SFX(3);
        return;
    }
    dave_jump_latch = 0;

after_jump:
    if (dave_button2 && dave_fire_latch) {             /* [+0x8BB6] and latch */
        dave_fire_latch = 0;
        dave_dir_y = 0;
        SetState(REC_AIM_R);
        return;
    }
    /* 0x3911: `if ([0x8BB2] != [0x8BB8]) call 0x37AE` -- dispatch on a command
       CHANGE.  [0x8BB8] is the previous command; it is initialised to 8 in
       0x1EB3, which sits BEFORE the frame-loop back-edge (0x21A7 `jmp 0x1EBF`),
       so that reset happens ONCE PER LEVEL and not once per frame.  The
       dispatch tail (0x387B) stores the new command back into [0x8BB8].
       Therefore: holding a direction dispatches once and the 8-record walk
       chain then runs; releasing dispatches the neutral command 8 ->
       dispatch arm 0x386F -> SetState(0x780) = standing. */
    if (dave_cmd != dave_prev_cmd) Dave_Dispatch();
    if (dave_info == REC_STAND && dave_ammo < 8) {
        dave_idle += dave_frame_delta;
        if (dave_idle > 0x1E) SetState(REC_RELOAD);
    } else {
        dave_idle = 0;                                 /* 0x3948: [0x9078] = 0 */
    }
}

/* 0x4077: jump per-frame.  Table CS:0x41D9 indexed by (cmd-1):
     cmd 1,2,3 -> 0x408C (accelerate right, +2/frame up to +0x18)
     cmd 4     -> 0x4109 (vertical: damp the horizontal component)
     cmd 5,6,7 -> 0x40CD (accelerate left, -2/frame down to -0x18)
   Then 0x415F: jump thrust [0x907A] and gravity [0x8C04]. */
void Dave_Jump(void)                                   /* 0x4077 */
{
    int i, d = dave_frame_delta > 0 ? dave_frame_delta : 1;
    int cmd = dave_cmd, si;

    if (cmd >= 1 && cmd <= 3) {
        for (si = 0; si < d; si++) {
            dave_jump_accel += 2;
            if (dave_jump_accel > 0x18) dave_jump_accel = 0x18;
            dave_xmove += dave_jump_accel;
        }
        if (dave_xmove > 0) {
            dave_dir_x = 1;
            dave_sprite = dd2_actorinfo_blob[(dave_info - DD2_ACTORINFO_BASE) + 2] |
                          (dd2_actorinfo_blob[(dave_info - DD2_ACTORINFO_BASE) + 3] << 8);
        }
    } else if (cmd >= 5 && cmd <= 7) {
        for (si = 0; si < d; si++) {
            dave_jump_accel -= 2;
            if (dave_jump_accel < -0x18) dave_jump_accel = -0x18;
            dave_xmove += dave_jump_accel;
        }
        if (dave_xmove < 0) {
            dave_dir_x = -1;
            dave_sprite = dd2_actorinfo_blob[(dave_info - DD2_ACTORINFO_BASE) + 0] |
                          (dd2_actorinfo_blob[(dave_info - DD2_ACTORINFO_BASE) + 1] << 8);
        }
    } else {                                            /* cmd 0/4/8: vertical */
        if (dave_xmove > 0) {
            for (si = 0; si < d; si++) {
                dave_jump_accel--;
                dave_xmove += dave_jump_accel;
                if (dave_jump_accel < 0) dave_jump_accel = 0;
            }
        } else if (dave_xmove < 0) {
            for (si = 0; si < d; si++) {
                dave_jump_accel++;
                dave_xmove += dave_jump_accel;
                if (dave_jump_accel > 0) dave_jump_accel = 0;
            }
        }
    }

    /* 0x415F: while the thrust counter has not run out, vertical speed comes
       from the jump vy (-40); after that -- normal gravity 0x19C0. */
    if (dave_thrust) {
        /* 0x415F: while thrust is active, vertical speed is [0x8C04] (-40)
           multiplied by the remaining thrust; gravity does NOT work at this time. */
        if (dave_thrust < d) {
            dave_ymove = dave_grav * dave_thrust;
            dave_thrust = 0;
        } else {
            dave_ymove = dave_grav * d;
            dave_thrust -= d;
        }
        if (!dave_button1) dave_thrust = 0;            /* 0x4195 */
        if (dave_thrust == 0) SetState(0x0898);        /* 0x41A9 */
    } else {
        /* 0x19C0: gravity ACCUMULATES.  The original:
               for each tick: [0x8C04] += 3 (no more than 0x50)
                              [0x8C00] += [0x8C04]
           Previously this was `ymove += 3`, i.e. the speed grew linearly --
           hence the "slow fall": in the original it is quadratic up to
           the limit of 0x50 (80). */
        for (i = 0; i < d; i++) {
            dave_grav += 3;                            /* 0x19C5 */
            if (dave_grav > 0x50) dave_grav = 0x50;    /* 0x19CA */
            dave_ymove += dave_grav;                   /* 0x19D7 */
        }
        /* 0x41B7..0x41D6: once gravity is positive and the record is neither
           of the two fall records, switch to the falling animation 0x8AC. */
        if (dave_info != 0x08AC && dave_info != 0x08C0 && dave_grav > 0)
            SetState(0x08AC);
    }
}

/* 0x3B6F: aiming/pull-up.  Analysed:
     if (button1) {                       ; jump from aiming
         if ([0x8C10] < 0) {              ; aiming up -> normal jump
             [0x907A]=0x0E; [0x8C02]=0; [0x8C04]=-40; SetState(0x884); sfx(3);
             return;
         }
         ; otherwise -- pull-up: scan tiles from the left [0x8BE6] to
         ; the right [0x8BEA] in row [0x8BEC]+1; if all are empty --
         ; [0x8BDC] += 0x40 (up by 4 px), [0x8BFC]++, SetState(0x8AC)
         ; (map access: [0xA6CA] + (([0x8BEC]+1)*[0x9EB0] + si)*2,
         ;  then + [0x4A94])
     }
     if (button2 && [0x8BC6]) {           ; fire from aiming
         [0x8BC6] = 0;
         if ([0x8C10] < 0) SetState(0x924); else SetState(0x8D4);
     } */
void Dave_Aim(void)                                    /* 0x3B6F */
{
    if (dave_button1) {
        if (dave_dir_y < 0) {                          /* aiming up */
            dave_thrust = 0x0E;
            dave_jump_accel = 0;
            dave_grav = -40;                           /* [0x8C04] = 0xFFD8 */
            SetState(REC_JUMP1);
            SFX(3);
        } else {
            /* 0x3BA5: PULL-UP.  If below the bottom edge (row tx_b+1)
               no column tx_l..tx_r has an obstacle per the SOUTH table,
               Dave rises by 0x40 (4 px): [0x8BDC] += 0x40, [0x8BFC]++. */
            dd2_obj *o = &dd2_objs[0];
            int si, blocked = (dd2_tile_south == 0);
            if (!blocked)
                for (si = o->tx_l; si <= o->tx_r; si++)
                    if (dd2_tile_south(o->tx_b + 1, si)) { blocked = 1; break; }
            if (!blocked) {
                dave_y = (int16_t)(dave_y + 0x40);
                /* 0x3BD6: `inc word ptr [0x8BFC]` -- [0x8BFC] is the scratch
                   object's +0x28 (f28), the fall counter that Obj_PushOutY
                   uses as its FIRST scanned row (0x19F5: `ax = [0x8BFC];
                   inc ax`).  The port incremented tx_b instead, so the Y
                   push-out scanned from the wrong row and Dave was shoved
                   back onto the shelf he had just dropped through. */
                o->f28++;
                SetState(0x08AC);
                SFX(3);
            }
        }
        return;
    }
    if (dave_button2 && dave_fire_latch) {
        dave_fire_latch = 0;
        /* 0x3C02: dir_y < 0 -> 0x924, otherwise the DOWNWARD shooting record
           0x974 (not the horizontal 0x8D4). */
        SetState(dave_dir_y < 0 ? 0x0924 : 0x0974);
        return;
    }
    /* 0x3C1F: the same dispatch as in 0x3891, but in the aiming state. */
    if (dave_cmd != dave_prev_cmd) Dave_Dispatch();
}

/* 0x3DE0: shooting frame.  While counter [0x9076] is non-zero: reorientation by
   cmd (table CS:0x4063), choice of the aiming record by [0x8C10], then the shot
   (hitscan 0x3C4C) and the flash.  First frame -- recoil by [0x8C0E]<<6. */
void Dave_Shoot(void)                                  /* 0x3DE0 */
{
    if (dave_ammo == 0) {
        dave_x += dave_dir_x << 6;                     /* recoil 4 px */
        SetState(REC_STAND);                           /* 0x3DFA */
        SFX(7);                                        /* 0x3E03 -> 0x4056 */
        return;
    }
    /* table CS:0x4063: cmd -> signs of [0x8C0E]/[0x8C10] */
    switch (dave_cmd) {
    case 0: dave_dir_y = -1; break;
    case 1: dave_dir_x =  1; dave_dir_y = -1; break;
    case 2: dave_dir_x =  1; dave_dir_y =  0; break;
    case 3: dave_dir_x =  1; dave_dir_y =  1; break;
    case 4: dave_dir_y =  1; break;
    case 5: dave_dir_x = -1; dave_dir_y =  1; break;
    case 6: dave_dir_x = -1; dave_dir_y =  0; break;
    case 7: dave_dir_x = -1; dave_dir_y = -1; break;
    default: break;
    }
    /* 0x3E82/0x3E95/0x3EA8: re-orient the shooting record to the current
       vertical direction (up -> 0x938, horizontal -> 0x8E8, down -> 0x988). */
    if (dave_dir_y < 0) {
        if (dave_info != 0x0924) SetState(0x0938);
    } else if (dave_dir_y > 0) {
        if (dave_info != 0x0974) SetState(0x0988);
    } else {
        if (dave_info != 0x08D4) SetState(0x08E8);
    }
    if (!dd2_god_mode) dave_ammo--;                    /* 0x3EB9: [0x4A8C] == 0 */

    /* 0x3EC4..0x3F79: flash record, origin and ray range by vertical direction:
         up:         rec 0x9EC, origin y+0x10,  x+(dir_x>0?0x120:0x30),
                     rays si 0..2 (dy -128,-100,-80)
         horizontal: rec 0xA28, origin y+0xA0,  x+(dir_x>0?0x140:0xA0),
                     3 rays (y, y+0x40, y-0x80), dy = 0
         down:       rec 0xA64, origin y+0x150, x+(dir_x>0?0x120:0x30),
                     rays si 3..7 (dy 100,80,64,48,32) */
    {
        static const int adx[8] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };
        static const int ady[8] = { -128, -100, -80, 100, 80, 64, 48, 32 };
        int rec, si, si_end, bx, by, slot;

        if (dave_dir_y < 0) {
            rec = 0x09EC; si = 0; si_end = 2;
            by = dave_y + 0x10;
            bx = dave_x + (dave_dir_x > 0 ? 0x120 : 0x30);
        } else if (dave_dir_y > 0) {
            rec = 0x0A64; si = 3; si_end = 7;
            by = dave_y + 0x150;
            bx = dave_x + (dave_dir_x > 0 ? 0x120 : 0x30);
        } else {
            rec = 0x0A28; si = 0; si_end = 0;
            by = dave_y + 0xA0;
            bx = dave_x + (dave_dir_x > 0 ? 0x140 : 0xA0);
        }
        slot = Obj_Spawn(0x12, dave_x, dave_y, dave_dir_x, rec);   /* 0x3F73 */
        if (slot >= 0) dd2_objs[slot].f3c = -1;                    /* 0x3F7D */
        dd2_hit_x = 0; dd2_hit_y = 0;                              /* 0x3F82 */

        if (dave_dir_y == 0) {
            int r = Dave_Hitscan(bx, by, dave_dir_x << 7, 0);
            if (r == 0) r = Dave_Hitscan(bx, by + 0x40, dave_dir_x << 7, 0);
            if (r == 0) Dave_Hitscan(bx, by - 0x80, dave_dir_x << 7, 0);
        } else {
            for (; si <= si_end; si++) {
                int r = Dave_Hitscan(bx, by, adx[si] * dave_dir_x, ady[si]);
                if (r == 1) break;             /* 0x4015: only an object stops the fan */
            }
        }

        if (dd2_hit_x != 0) {                  /* 0x401F: object OR wall -> sparks */
            slot = Obj_Spawn(0x12, dd2_hit_x - 0x40, dd2_hit_y - 0x40, 0, 0x0AA0);
            if (slot >= 0) dd2_objs[slot].f02 = 1;
            SFX(5);                            /* 0x404E */
        } else {
            SFX(6);                            /* 0x4053: miss */
        }
    }
}

/* 0x37AE: state dispatch by cmd.  Table CS:0x387F (9 transitions):
     0 -> 0x37C1 (up: if [0x9068] != 0 -- climbing, otherwise [0x8C10] = -1)
     1 -> NE: [0x8C0E]=+1,[0x8C10]=-1 ; 2 -> E:  [0x8C0E]=+1
     3 -> SE: +1,+1 ; 4 -> S: [0x8C10]=+1 ; 5 -> SW: -1,+1
     6 -> W: [0x8C0E]=-1 ; 7 -> NW: -1,-1 ; 8 -> [0x780] (standing)
   At the end always [0x8BB8] = [0x8BB2]. */
void Dave_Dispatch(void)                                /* 0x37AE */
{
    switch (dave_cmd) {
    case 0:                                             /* 0x37C1 */
        /* If there is a door tile under Dave, scan 0x4422 has put the column into
           [0x9068] and the type into [0x9070]; otherwise -- aim up. */
        if (dd2_scan_col != 0) {
            if (dd2_door_type == 1) {                   /* 0x37C8: type 1 -- "open" */
                dave_x = dd2_scan_col << 8;             /* [0x8BDA] */
                SetState(REC_OPEN);                     /* 0x834 */
            } else {                                    /* 0x37DF: type 2 -- "enter" */
                dave_x = ((dd2_scan_col - 1) << 8) + 0x40;
                SetState(REC_ENTER1);                   /* 0x848 */
            }
        } else {                                        /* 0x37F9 */
            dave_dir_y = -1;                            /* [0x8C10] = -1 */
            SetState(REC_AIM_UP_R);                     /* 0x910 */
        }
        break;
    case 1: dave_dir_x =  1; dave_dir_y = -1; SetState(REC_AIM_UP_R); break;
    case 2: dave_dir_x =  1;                  SetState(REC_WALK_E1);  break;
    case 3: dave_dir_x =  1; dave_dir_y =  1; SetState(REC_AIM_DN_R); break;
    case 4:                  dave_dir_y =  1; SetState(REC_AIM_DN_R); break;
    case 5: dave_dir_x = -1; dave_dir_y =  1; SetState(REC_AIM_DN_R); break;
    case 6: dave_dir_x = -1;                  SetState(REC_WALK_E1);  break;
    case 7: dave_dir_x = -1; dave_dir_y = -1; SetState(REC_AIM_UP_R); break;
    default: SetState(REC_STAND); break;
    }
    dave_prev_cmd = dave_cmd;                           /* [0x8BB8] = [0x8BB2] */
}

/* Analogue of `call word ptr [0x8C2C]` from 0x1EAB: picks the handler by the current
   actor-info record and applies the movement (0x2E07).  Works on the
   scratch object (dd2_cur), like the original. */
int dd2_dave_state_calls;      /* counter for the self-test */
int dave_react_calls;          /* debug: how many times Dave_React was called */

/* Door (0x394F) -- fn of record 0x870; the level data is in level.c, so it is
   wired up from main.c. NULL in self-tests. */
void (*dd2_door_fn)(void);
/* 0x3A5B -- fn of record 0x834 (the type-1 door state reached by cmd 0):
   opens the door.  It is a PER-FRAME function, not a reaction: the only
   record with fn == 0x3A5B is 0x834, and no record has react == 0x3A5B
   (checked over the whole actor table at DGROUP 0x780).  The port had it
   in the reaction dispatcher, so pressing UP only played Dave's own
   animation while the door tiles never changed. */
void (*dd2_door_open_fn)(void);

/* Dave's state decoding is IDENTICAL to the original: not by address ranges,
   but by the record's fn field (`call word ptr [bx+0xC]` at 0x1D76).  Previously
   there were ranges here, and the shooting chains 0x924/0x974 (fn 0x3DE0) did not
   fall into them -- Dave did not shoot, although handler 0x3DE0 was written. */
static void Dave_Step(void)
{
    const unsigned char *r;
    int fn;
    dd2_dave_state_calls++;
    if (dave_info < DD2_ACTORINFO_BASE) return;
    r = dd2_actorinfo_blob + (dave_info - DD2_ACTORINFO_BASE);
    fn = r[0x0C] | (r[0x0D] << 8);
    switch (fn) {
    case 0x3891: Dave_Frame();    break;   /* standing/walking/reload/door */
    case 0x4077: Dave_Jump();     break;   /* 0x884..0x8C0 */
    case 0x3B6F: Dave_Aim();      break;   /* 0x910, 0x960 */
    case 0x3DE0: Dave_Shoot();    break;   /* 0x8D4, 0x924, 0x974 */
    case 0x3C2D: Dave_Reload();   break;   /* 0x9C4 */
    case 0x4073: Dave_Dispatch(); break;   /* 0x8FC, 0x94C, 0x99C */
    case 0x394F: if (dd2_door_fn) dd2_door_fn(); break;   /* 0x870 */
    case 0x3A5B: if (dd2_door_open_fn) dd2_door_open_fn(); break;  /* 0x834 */
    default: break;                        /* fn == 0 -- no handler */
    }
}


/* -------------------------------------------------------------------------
 * 0x43A9 -- Dave's REACTION for the jump/fall records (0x884, 0x898, 0x8AC,
 * 0x8C0).  It is what does the touch physics, and the port did not have it:
 *
 *   call 0x19E7                          ; collision on BOTH X AND Y
 *   if hit on the left or right: [0x8C02] = 0    ; reset X acceleration
 *   if hit below:                                ; LANDING
 *       call 0x4073 (= 0x37AE, dispatch)
 *       SFX(4)
 *       [0x8BDA] += [0x8C0E] << 6        ; x += dir_x * 64
 *       Obj_Box() ; call 0x19E7          ; recompute and push out again
 *   else if hit above:                           ; HEAD BUMP
 *       [0x8C04] = 0                     ; reset vertical acceleration
 *       SFX(0x18)
 *       SetState(0x8AC)                  ; switch to falling
 *   0x4405: call 0x1BDC (not analysed) ; draw(0x2B0C)
 * ---------------------------------------------------------------------- */
void Dave_PhysReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h)
{
    if (!o) return;
    Obj_PushOutX(o, tiles, east, west, w, h);          /* 0x43A9: call 0x19E7 */
    Obj_PushOutY(o, tiles, north, south, w, h);

    if (o->hit_left || o->hit_right) {                 /* 0x43AC */
        dave_jump_accel = 0;                           /* [0x8C02] = 0 */
        dd2_xmove = 0;
    }
    if (o->hit_below) {                                /* 0x43C0 */
        Dave_Dispatch();                               /* 0x43C7: call 0x4073 */
        SFX(4);
        dave_x = (int16_t)(dave_x + (dd2_dir_x << 6)); /* 0x43D3 */
        o->x = (int16_t)dave_x;
        Obj_Box(o);                                    /* 0x43DE: call 0x192E */
        Obj_PushOutX(o, tiles, east, west, w, h);      /* 0x43E1: call 0x19E7 */
        Obj_PushOutY(o, tiles, north, south, w, h);
    } else if (o->hit_above) {                         /* 0x43E6 */
        dave_grav = 0;                                 /* [0x8C04] = 0 */
        SFX(0x18);
        SetState(0x08AC);                              /* 0x43FC */
    }
    /* 0x4405: call 0x1BDC -- recompute the camera and the view window AFTER
       the push-out and the landing step.  In the port this is Level_Camera()
       (0x1BDC is also called in the main loop, but the original calls it here
       too, so that the frame is built from the already corrected position). */
    Level_Camera();
}

void Dave_React(dd2_obj *o)
{
    dave_react_calls++;
    dave_x = o->x; dave_y = o->y; dave_sprite = o->sprite; dave_info = o->info;
    /* 0x1D76 has already computed the movement from the record:
           [0x8BFE] = rec[+8]  * [0x8C0E]
           [0x8C00] = rec[+0xA] * [0x8C10]
       Dave applies it at 0x2E07, so we take the value from there, and after
       the handler we put it back -- 0x2E07 may adjust it (jump). */
    dave_xmove = dd2_xmove;
    dave_ymove = dd2_ymove;
    Dave_Step();
    dd2_xmove = dave_xmove;
    dd2_ymove = dave_ymove;
    dd2_dir_x = dave_dir_x;          /* [0x8C0E] -- sign for the next frame */
    dd2_dir_y = 1;                   /* [0x8C10] is not overwritten in the original */
    o->x = (int16_t)dave_x; o->y = (int16_t)dave_y;
    o->sprite = (int16_t)dave_sprite; o->info = (int16_t)dave_info;
}

void Dave_Init(void)
{
    dave_cmd = 8; dave_prev_cmd = 8;
    dave_jump_latch = dave_fire_latch = 0;
    dave_dir_x = 1; dave_dir_y = 0;
    dave_ammo = 8;
    dave_idle = dave_thrust = 0;
    dave_info = REC_STAND;
    dave_grav = 0;
}

/* ------------------------------------------------------------------ test */
#ifdef DD2_DAVE_TEST
#include <stdio.h>
static int t_south_free(int r, int c) { (void)r; (void)c; return 0; }
static int t_south_wall(int r, int c) { (void)r; (void)c; return 1; }

int main(void)
{
    Obj_ClearAll();
    dd2_obj_count = 2;
    Dave_Init();
    dave_x = 100; dave_y = 100;
    dave_button1 = 1;
    Dave_Frame();
    /* 0x3891 sets [0x8C04] = -40 (dave_grav), the initial vertical speed,
       and the thrust counter to 14. */
    printf("jump: state=%#06x vy=%d accel=%d thrust=%d (expected 0x0884, -40, 0, 14)\n",
           dave_info, dave_grav, dave_jump_accel, dave_thrust);
    dave_cmd = 2; dave_button1 = 0;
    dave_jump_vy = -40; dave_frame_delta = 1;
    Dave_Jump();
    printf("after 1 frame right: xmove=%d accel=%d\n", dave_xmove, dave_jump_accel);
    /* While the thrust counter lasts, vertical speed is the jump speed
       multiplied by the ticks in this frame and the counter counts down
       (0x415F).  The button must stay held, otherwise 0x4195 clears it. */
    dave_xmove = 0; dave_jump_accel = 0; dave_thrust = 14; dave_frame_delta = 3;
    dave_button1 = 1; dave_grav = -40;
    Dave_Jump();
    printf("thrust 3 frames: ymove=%d thrust=%d (expected -120, 11)\n", dave_ymove, dave_thrust);
    /* After the thrust runs out, gravity ACCUMULATES (0x19C0): two ticks of
       delta 1 give [0x8C04] = -40+3 = -37 then -34, so ymove = -71. */
    dave_ymove = 0; dave_thrust = 0; dave_frame_delta = 2; dave_grav = -40;
    Dave_Jump();
    printf("gravity 2 frames: ymove=%d accel=%d (expected -71, -34)\n",
           dave_ymove, dave_grav);
    dave_ammo = 7; dd2_god_mode = 0; dave_dir_x = 1; dave_dir_y = 0;
    dave_cmd = 6;
    Dave_Shoot();
    printf("shot: ammo=%d dir_x=%d dir_y=%d\n", dave_ammo, dave_dir_x, dave_dir_y);
    {
        int n = 0, i, spr = -1;
        for (i = 0; i < DD2_MAXOBJ; i++)
            if (dd2_objs[i].type == 0x12) { n++; spr = dd2_objs[i].sprite; }
        printf("flash: objects of type 0x12 = %d, sprite = %d (expected GUNFLASH*)\n", n, spr);
        Dave_Shoot(); Dave_Shoot();
        n = 0;
        for (i = 0; i < DD2_MAXOBJ; i++) if (dd2_objs[i].type == 0x12) n++;
        printf("after second shot: objects of type 0x12 = %d\n", n);
    }
    dave_ammo = 0;
    Dave_Shoot();
    printf("reload end: state=%#06x ammo=%d\n", dave_info, dave_ammo);

    /* 0x37C1: entering a door via cmd 0 */
    Dave_Init();
    dd2_scan_col = 5; dd2_door_type = 1;     /* [0x9068], [0x9070] */
    dave_cmd = 0; dave_prev_cmd = 8;
    Dave_Dispatch();
    printf("door type 1: state=%#06x x=%d (expected %#06x / %d)\n",
           dave_info, dave_x, REC_OPEN, 5 << 8);
    Dave_Init();
    dd2_scan_col = 5; dd2_door_type = 2;
    dave_cmd = 0; dave_prev_cmd = 8;
    Dave_Dispatch();
    printf("door type 2: state=%#06x x=%d (expected %#06x / %d)\n",
           dave_info, dave_x, REC_ENTER1, ((5 - 1) << 8) + 0x40);
    Dave_Init();
    dd2_scan_col = 0;
    dave_cmd = 0; dave_prev_cmd = 8;
    Dave_Dispatch();
    printf("no door:  state=%#06x dir_y=%d (expected %#06x / -1)\n",
           dave_info, dave_dir_y, REC_AIM_UP_R);
    {
        /* door record chain: 0x848 -> 0x85C -> 0x870 (fn 0x394F) -> 0x780 */
        static const int chain[3] = { 0x0848, 0x085C, 0x0870 };
        int k;
        for (k = 0; k < 3; k++) {
            const unsigned char *r = dd2_actorinfo_blob + (chain[k] - DD2_ACTORINFO_BASE);
            int fn = r[0x0C] | (r[0x0D] << 8), nx = r[0x12] | (r[0x13] << 8);
            printf("  record %#06x: fn=%#06x next=%#06x\n", chain[k], fn, nx);
        }

        /* 0x3B6F: pull-up.  With button 1 held and NOT aiming up,
           if there is no obstacle below the bottom edge (SOUTH table), Dave
           rises by 0x40.  We test both cases. */
        {
            int y0;
            Dave_Init();
            dd2_objs[0].tx_l = 4; dd2_objs[0].tx_r = 4; dd2_objs[0].tx_b = 3;
            dave_button1 = 1; dave_button2 = 0; dave_dir_y = 1; dave_prev_cmd = 8; dave_cmd = 8;
            dd2_tile_south = t_south_free;  y0 = dave_y; Dave_Aim();
            printf("climb (path free): dy=%d (expected 64)\n", dave_y - y0);
            Dave_Init();
            dd2_objs[0].tx_l = 4; dd2_objs[0].tx_r = 4; dd2_objs[0].tx_b = 3;
            dave_button1 = 1; dave_button2 = 0; dave_dir_y = 1; dave_prev_cmd = 8; dave_cmd = 8;
            dd2_tile_south = t_south_wall;  y0 = dave_y; Dave_Aim();
            printf("climb (obstacle):    dy=%d (expected 0)\n", dave_y - y0);
            dd2_tile_south = 0;
        }
    }
    return 0;
}
#endif
