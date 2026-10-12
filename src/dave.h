/*
 * dave.h -- Dave's states and physics (DD2, original 0x3891/0x4077/0x3B6F/
 * 0x3DE0/0x3C2D/0x3C4C/0x2E07).  All globals are named after the original DS addresses.
 */
#ifndef DD2_DAVE_H
#define DD2_DAVE_H
#include <stdint.h>
#include "objects.h"

/* frame input (fills Input_Frame, 0x754B/0x71A5) */
extern int dave_cmd;            /* [0x8BB2] 0=N 1=NE 2=E ... 8=neutral */
extern int dave_button1;        /* [0x8BB4] jump */
extern int dave_button2;        /* [0x8BB6] fire  */
extern int dave_prev_cmd;       /* [0x8BB8] */
extern int dave_jump_latch;     /* [0x8BBA] */
extern int dave_fire_latch;     /* [0x8BC6] */

/* player object state (object array slot 0, DS:0x520A) */
/* 0x3B6F: SOUTH table by tile coordinates -- needed by the pull-up.
   Wired up from main.c; NULL in self-tests. */
extern int (*dd2_tile_south)(int row, int col);
/* 0x394F: door for Dave's record 0x870 -- the level data lives in main.c */
extern void (*dd2_door_fn)(void);       /* 0x394F: enter/teleport (record 0x870) */
extern void (*dd2_door_open_fn)(void);  /* 0x3A5B: open the door (record 0x834) */

extern int dave_x;              /* [0x8BDA] */
extern int dave_y;              /* [0x8BDC] */
extern int dave_sprite;         /* [0x8BD8] */
extern int dave_xmove;          /* [0x8BFE] */
extern int dave_ymove;          /* [0x8C00] */
extern int dave_jump_accel;     /* [0x8C02] */
extern int dave_jump_vy;        /* [0x8C04] */
extern int dave_dir_x;          /* [0x8C0E] */
extern int dave_dir_y;          /* [0x8C10] */
extern int dave_info;           /* [0x8C18] current actor-info record */
extern int dave_frame_delta;    /* [0x8C6A] */
extern int dave_ammo;           /* [0x9076] reload/ammo counter */
extern int dave_idle;           /* [0x9078] idle counter */
extern int dave_thrust;         /* [0x907A] jump thrust counter */
extern int dd2_god_mode;        /* [0x4A8C] -- F12 menu toggle (0x341) */

/* Sound: in the original, a call to 0x8CB2(id).  The port plugs its own player in here. */
extern void (*dd2_sfx)(int id);

void Dave_PhysReact(dd2_obj *o, const uint16_t *tiles, const uint8_t *north,
                    const uint8_t *south, const uint8_t *east,
                    const uint8_t *west, int w, int h);   /* 0x43A9 */
void Dave_Dispatch(void);        /* 0x37AE -- state dispatch by cmd */
void Dave_Init(void);
void Dave_Move(void);           /* 0x2E07 -- clamp + apply + bbox */
void Dave_Frame(void);          /* 0x3891 -- common per-frame */
void Dave_Jump(void);           /* 0x4077 */
void Dave_Aim(void);            /* 0x3B6F */
void Dave_Shoot(void);          /* 0x3DE0 */
void Dave_Reload(void);         /* 0x3C2D */
int  Dave_Hitscan(int x, int y, int dx, int dy);   /* 0x3C4C: marching ray */
extern const uint16_t *dd2_hs_tiles;   /* [0xA6CA] tile plane for 0x3C4C */
extern int dd2_hs_w, dd2_hs_h;         /* [0x9EB0] level width/height */
/* dir: 0=NORTH 1=SOUTH 2=EAST 3=WEST; returns nonzero for a solid tile */
extern int (*dd2_hs_solid)(int tile, int dir);
extern int dave_react_calls;
void Dave_React(dd2_obj *o);           /* analogue of call [0x8C2C] */

#endif /* DD2_DAVE_H */
