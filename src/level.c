/*
 * level.c -- spawning of level objects from the tag plane (original 0x53FF)
 * and player start (0x3741), prizes/bonuses (0x3539).
 *
 * The tag map is taken from the disassembly of 0x53FF (tables CS:0x5574 for tags
 * 1..10 and CS:0x556C for 0x0C..0x0F):
 *
 *   tag   flag               handler
 *   1     [0x51F0]           0x44DE   zombie
 *   2     [0x51F2]           0x469A
 *   3     [0x51F6]           0x47E6
 *   4     [0x51F4]           0x48FE
 *   5     [0x51F4]           0x4934
 *   6     [0x51F8]           0x4DCA
 *   7     [0x51FA]           0x4CE0
 *   8     --                 spawns nothing
 *   9     [0x51FC]           0x4AAA
 *   10    [0x51F6]           flag only
 *   0x0B  [0x51FE]           0x4F62
 *   0x0C  [0x5200]           0x50A0
 *   0x0D  [0x5200]           0x538C
 *   0x0E..0x13  --           0x3539(tag-0x0E)  six prizes
 *   0xFF  --                 0x3741            player start (slot 0)
 *   other --                 skip
 *
 * Handlers receive (col, row) -- tile coordinates -- and compute the
 * world coordinates themselves as (col<<8), (row<<8).
 */
#include "level.h"
#include "dave.h"
#include "objects.h"
#include "dd2.h"
#include "dd2_actorinfo.h"
#include <string.h>
#include <stdio.h>
#include "dd2_random.h"

/* 0x8CB2 -- sound request (defined in dave.c, wired up in main.c). */
extern void (*dd2_sfx)(int id);

int  dd2_tag_flags[10];        /* [0x51EE]..[0x5200], one word per type */
int  dd2_level_w, dd2_level_h; /* [0x9EB0], [0xA6DA] */
int  dd2_spawn_counts[16];

/* 0x8EAF: a "random" number -- a cyclic walk over a 256-byte table
   DGROUP 0x2EB0 (index [0x2EAE]).  Fully deterministic, which is why the
   enemy variants match the original frame for frame. */
static int dd2_rand_idx;               /* [0x2EAE] */
/* 0x8E8D: seed [0x2EAE].  arg != 0 -> the DOS clock (int 21h/AH=2Ch, DL =
   hundredths of a second, 0..99); arg == 0 -> 0.  The original calls it ONCE
   per program run (0x16F4, arg=1) and never resets it per level. */
/* Clock source for the 0x8E8D seed (int 21h/AH=2Ch -> hundredths).  The
   platform layer plugs it in from main.c; the self-tests leave it NULL and get
   the deterministic seed 0. */
int (*dd2_clock_hundredths)(void);
void dd2_rand_seed(int use_clock)
{
    dd2_rand_idx = use_clock ? ((dd2_clock_hundredths ? dd2_clock_hundredths() : 0) % 100) : 0;
}
void dd2_rand_set(int idx)
{
    dd2_rand_idx = idx & 0xFF;
}
int dd2_rand_byte(void)                /* 0x8EAF */
{
    dd2_rand_idx = (dd2_rand_idx + 1) & 0xFF;
    return dd2_random_table[dd2_rand_idx];
}

/* --- enemy spawners: fully analysed (Obj_Spawn arguments 0x17F4) -----------
   tag  type  info                              dir   dy (to row<<8)
    1    2     [0x128C + (rand/0x40)*2]          -1    -0x80   + [+0x4A]=1 [+0x48]=1 [+0x3E]=2 [+0x3C]=1
    2    3     0x0D48                            -1    +0x80   + [+0x4A]=1 [+0x48]=1 [+0x3E]=2
    3    4     0x0E10                            +1    +0x80   + [+0x3C]=1 [+0x4A]=1 [+0x48]=1 [+0x3E]=1
    4    6     0x0F28                            +1    +0x80   + [+0x48]=0 [+0x3E]=1
    5    6     0x0EB0                            +1    +0x80   + [+0x3C]=1 [+0x48]=0 [+0x3E]=1
    6    5     0x1004                            +1     0      + [+0x4A]=1 [+0x48]=1 [+0x3E]=1 [+0x4C]=[bx+8]
    7    8     0x0FDC                            +1     0      + [+0x48]=1 [+0x4A]=0
    9    7     0x0F64                            +1     0      + [+0x4A]=1 [+0x48]=1 [+0x3E]=3
    0x0B 10    0x1180                            -1     0      + [+0x48]=1 [+0x4A]=1 [+0x3E]=0x0F
    0x0C 11    0x1054                            +1     0      + [+0x48]=1 [+0x4A]=1 [+0x3E]=0x0F
    0x0D 12    0x111C                            +1     0      + [+0x3A]=-1 [+0x3C]=1            */
/* indexed directly by tag (1..13); 0, 8 and 10 do not spawn */
static const int spawn_type[14] = { 0, 2, 3, 4, 6, 6, 5, 8, 0, 7, 0, 10, 11, 12 };
static const int spawn_info[14] = { 0, 0x0C94, 0x0D48, 0x0E10, 0x0F28, 0x0EB0,
                                    0x1004, 0x0FDC, 0, 0x0F64, 0, 0x1180, 0x1054, 0x111C };
static const int spawn_dir [14] = { 0, -1, -1, 1, 1, 1, 1, 1, 0, 1, 0, -1, 1, 1 };
static const int spawn_dy  [14] = { 0, -0x80, 0x80, 0x80, 0x80, 0x80, 0, 0, 0, 0, 0, 0, 0, 0 };

/* 0x47E6 (tag 3) chooses the record by the NORTH table: if it is solid under
   the cell, 0x0E10 is used (offset +0x80, [+0x3C]=1), otherwise 0x0DE8 (offset 0,
   [+0x3C]=-1).  The level data is only available inside Level_SpawnTags,
   so we keep the references here. */
static const uint16_t *sp_tiles;
static const uint8_t  *sp_north;
static int             sp_w, sp_h;

/* Wired up from main.c before Level_SpawnTags: the level tiles and the NORTH table
   are needed for the conditional spawn of tag 3 (0x47E6). */
const uint16_t *dd2_spawn_tiles;
const uint8_t  *dd2_spawn_north;

static int SpawnOne(int tag, int col, int row)
{
    int info = spawn_info[tag], slot, dy = spawn_dy[tag], dir = spawn_dir[tag];
    if (tag == 1) info = 0x0C94 + 0x14 * ((dd2_rand_byte() / 0x40) & 3);  /* 0x44DE */
    if (tag == 3) {                                     /* 0x47E6: NORTH branch */
        int r = row + 1, solid = 0;
        if (sp_tiles && sp_north && r >= 0 && r < sp_h && col >= 0 && col < sp_w) {
            int t = sp_tiles[r * sp_w + col];
            solid = (t >= 0 && t <= 857) ? sp_north[t] : 0;
        }
        if (solid) { info = 0x0E10; dy = 0x80; dir = 1; }
        else       { info = 0x0DE8; dy = 0;    dir = 1; }
    }
    if (spawn_type[tag] == 0) return -1;
    slot = Obj_Spawn(spawn_type[tag], col << 8,
                     (row << 8) + dy, dir, info);
    if (slot < 0) return slot;
    switch (tag) {
    case 1: dd2_objs[slot].f48[1] = 1; dd2_objs[slot].f48[0] = 1;
            dd2_objs[slot].health = 2; dd2_objs[slot].f3c = 1; break;
    case 2: dd2_objs[slot].f48[1] = 1; dd2_objs[slot].f48[0] = 1;
            dd2_objs[slot].health = 2; break;
    case 3: /* 0x4830/0x485D: [+0x3C] = 1 if there is floor under the cell, otherwise -1;
               then the common tail 0x4862: [+0x4A]=1 [+0x48]=1 [+0x3E]=1 */
            dd2_objs[slot].f3c = (int16_t)(info == 0x0E10 ? 1 : -1);
            dd2_objs[slot].f48[1] = 1;
            dd2_objs[slot].f48[0] = 1; dd2_objs[slot].health = 1; break;
    case 4: dd2_objs[slot].f48[0] = 0; dd2_objs[slot].health = 1; break;
    case 5: dd2_objs[slot].f3c = 1; dd2_objs[slot].f48[0] = 0;
            dd2_objs[slot].health = 1; break;
    case 6: dd2_objs[slot].f48[1] = 1; dd2_objs[slot].f48[0] = 1;
            dd2_objs[slot].health = 1;
            /* 0x4DFF `mov ax,[bx+8]` / 0x4E02 `mov [bx+0x4C],ax` -- the
               object's Y (+0x08 = row<<8) is the rope/vine anchor read at
               0x4EB4/0x4EEB/0x4F11/0x4F44.  Reading f1a (+0x1A, never set)
               left the anchor at 0. */
            dd2_objs[slot].f48[2] = dd2_objs[slot].y; break;
    case 7: dd2_objs[slot].f48[0] = 1; dd2_objs[slot].f48[1] = 0; break;
    case 9: dd2_objs[slot].f48[1] = 1; dd2_objs[slot].f48[0] = 1;
            dd2_objs[slot].health = 3; break;
    /* The switch is indexed by TAG (the table above: tag -> type).  Tag 0x0C
       (12) also spawns the MASTER (type 11, record 0x1054) and must get the
       same damage flags as tag 0x0B -- the level-8 final boss is tag 0x0C
       (0x50A0: [+0x48]=1, [+0x4A]=1, [+0x3E]=0x0F).  Without this the final
       boss was invulnerable: Dave's hitscan (0x3C5C) skips any object whose
       [+0x48] is 0, so shots at it did nothing (the reported "damage to the
       last boss is not applied").  Tag 0x0D (13) is the one that carries the
       DELBERT attributes (dir=-1, [+0x3C]=1). */
    case 11: case 12: dd2_objs[slot].f48[0] = 1; dd2_objs[slot].f48[1] = 1;
            dd2_objs[slot].health = 0x0F; break;
    case 13: dd2_objs[slot].dir = -1; dd2_objs[slot].f3c = 1; break;
    }
    return slot;
}

/* Enemy spawners (0x44DE ... 0x538C).  The tag dispatcher calls them by the tag
   index; NULL means "not ported yet". */
void (*dd2_enemy_spawn[16])(int col, int row);

static void Enemy_None(int col, int row) { (void)col; (void)row; }
static void Enemy_Tag1(int col, int row)  { SpawnOne(1, col, row); }   /* 0x44DE */
static void Enemy_Tag2(int col, int row)  { SpawnOne(2, col, row); }   /* 0x469A */
static void Enemy_Tag3(int col, int row)  { SpawnOne(3, col, row); }   /* 0x47E6 */
static void Enemy_Tag4(int col, int row)  { SpawnOne(4, col, row); }   /* 0x48FE */
static void Enemy_Tag5(int col, int row)  { SpawnOne(5, col, row); }   /* 0x4934 */
static void Enemy_Tag6(int col, int row)  { SpawnOne(6, col, row); }   /* 0x4DCA */
static void Enemy_Tag7(int col, int row)  { SpawnOne(7, col, row); }   /* 0x4CE0 */
static void Enemy_Tag9(int col, int row)  { SpawnOne(9, col, row); }   /* 0x4AAA */
static void Enemy_TagB(int col, int row)  { SpawnOne(11, col, row); }  /* 0x4F62 */
static void Enemy_TagC(int col, int row)  { SpawnOne(12, col, row); }  /* 0x50A0 */
static void Enemy_TagD(int col, int row)  { SpawnOne(13, col, row); }  /* 0x538C */

void Level_SpawnInit(void)
{
    int i;
    for (i = 0; i < 16; i++) dd2_enemy_spawn[i] = Enemy_None;
    dd2_enemy_spawn[1]  = Enemy_Tag1;   dd2_enemy_spawn[2]  = Enemy_Tag2;
    dd2_enemy_spawn[3]  = Enemy_Tag3;   dd2_enemy_spawn[4]  = Enemy_Tag4;
    dd2_enemy_spawn[5]  = Enemy_Tag5;   dd2_enemy_spawn[6]  = Enemy_Tag6;
    dd2_enemy_spawn[7]  = Enemy_Tag7;   dd2_enemy_spawn[9]  = Enemy_Tag9;
    dd2_enemy_spawn[11] = Enemy_TagB;   dd2_enemy_spawn[12] = Enemy_TagC;
    dd2_enemy_spawn[13] = Enemy_TagD;
    /* NOTE: [0x2EAE] is NOT reset here -- the original seeds it once per run
       (0x16F4 -> 0x8E8D).  Resetting per level made the enemy variants always
       start from the same table position. */
}

/* 0x3539: prize/bonus.  idx 0..5 -- index in table DGROUP 0x1260,
   dir = -1, type = 9, and [+0x4C] = idx is written to the object. */
int Level_SpawnBonus(int idx, int col, int row)        /* 0x3539 */
{
    static const int rec[6] = { 0x0AB4, 0x0ADC, 0x0B04, 0x0B2C, 0x0B54, 0x0B7C };
    int slot;
    if (idx < 0 || idx > 5) return -1;
    slot = Obj_Spawn(9, col << 8, row << 8, -1, rec[idx]);
    if (slot >= 0) dd2_objs[slot].f48[2] = (int16_t)idx;   /* [+0x4C] = idx */
    return slot;
}

/* 0x3741: player start -- object array slot 0.
     memset(obj0, 0, 90); obj0.type = 1; obj0[+0x02] = 1; obj0[+0x3A] = 1;
     obj0.x = col<<8;  obj0.y = (row<<8) + 0x10 - 1;
     obj0[+0x48] = 0;  SetState(0x780)                                    */
void Level_SpawnPlayer(int col, int row)               /* 0x3741 */
{
    dd2_obj *o = &dd2_objs[0];
    memset(o, 0, sizeof *o);
    o->type = 1;                                       /* [+0x00] */
    o->f02 = 1;                                        /* [+0x02] */
    o->dir = 1;                                        /* [+0x3A] */
    o->x = (int16_t)(col << 8);                        /* [+0x06] */
    o->y = (int16_t)((row << 8) + 0x10 - 1);           /* [+0x08] */
    o->f48[0] = 0;                                     /* [+0x48] */
    o->info = 0x0780;
    {
        const unsigned char *ai = dd2_actorinfo_blob + (0x0780 - DD2_ACTORINFO_BASE);
        o->sprite = (int16_t)(ai[2] | (ai[3] << 8));   /* SetState(0x780) */
        o->react = (int16_t)(ai[0x10] | (ai[0x11] << 8));     /* [+0x58] */
        o->react_obj = (int16_t)(ai[0x0E] | (ai[0x0F] << 8)); /* [+0x56] = 0x41E7 */
    }
    Obj_Box(o);                                        /* 0x192E */
    o->f22 = o->tx_l; o->f24 = o->tx_t;                /* edges before the move */
    o->f26 = o->tx_r; o->f28 = o->tx_b;
    if (dd2_obj_count < 1) dd2_obj_count = 1;
}

/* 0x53FF: walk the level tag plane and spawn the objects.
   Returns the number spawned. */
int Level_SpawnTags(const uint16_t *tags, int w, int h)  /* 0x53FF */
{
    /* tag 3 (0x47E6) looks at the tile under the cell via the NORTH table */
    sp_tiles = dd2_spawn_tiles; sp_north = dd2_spawn_north;
    sp_w = w; sp_h = h;

    static const int enemy_fn[11] = {
        0,          /* index 0 is not used */
        0x44DE, 0x469A, 0x47E6, 0x48FE, 0x4934,
        0x4DCA, 0x4CE0, 0, 0x4AAA, 0x0     /* 8 -- skip, 10 -- flag only */
    };
    /* tag -> flag index in dd2_tag_flags (addresses [0x51EE]+2*i):
       1->1, 2->2, 3->4, 4->3, 5->3, 6->5, 7->6, 9->7, 10->4, 0x0B->8, 0x0C/0x0D->9 */
    static const int tag_flag[16] = { -1, 1, 2, 4, 3, 3, 5, 6, -1, 7, 4, 8, 9, 9, -1, -1 };
    int row, col, n = 0;

    dd2_level_w = w;
    dd2_level_h = h;
    memset(dd2_tag_flags, 0, sizeof dd2_tag_flags);
    memset(dd2_spawn_counts, 0, sizeof dd2_spawn_counts);
    dd2_tag_flags[0] = 1;                              /* [0x51EE] = 1 */

    for (row = 0; row < h; row++)
        for (col = 0; col < w; col++) {
            int tag = tags[row * w + col];
            dd2_spawn_counts[tag < 16 ? tag : 15]++;
            if (tag >= 1 && tag <= 10) {
                int fn = enemy_fn[tag];
                if (tag == 8) continue;                /* 0x5552: skip */
                if (tag == 10) { dd2_tag_flags[4] = 1; continue; }
                if (tag_flag[tag] >= 0) dd2_tag_flags[tag_flag[tag]] = 1;
                if (dd2_enemy_spawn[tag]) { dd2_enemy_spawn[tag](col, row); n++; }
                (void)fn;
            } else if (tag >= 0x0E && tag <= 0x13) {
                if (Level_SpawnBonus(tag - 0x0E, col, row) >= 0) n++;
            } else if (tag == 0x0B || tag == 0x0C || tag == 0x0D) {
                if (tag_flag[tag] >= 0) dd2_tag_flags[tag_flag[tag]] = 1;
                if (dd2_enemy_spawn[tag]) { dd2_enemy_spawn[tag](col, row); n++; }
            } else if (tag == 0xFF) {
                Level_SpawnPlayer(col, row);           /* 0x3741 */
                n++;
            }
        }
    return n;
}

/* -------------------------------------------------------------------------
 * 0x1BDC -- camera.  Keeps the player inside the frame and stops at the level edges:
 *   X: if (cam + 0x900 <= x) cam -= (cam+0x900 - x), but not below [0x4A88];
 *      else if (cam + 0xB00 < x) cam += (x - cam - 0xB00), but not above [0x4A82];
 *   Y: the same with 0x400 and 0x800, limits [0x4A8A] and [0x4A84].
 * 0x900 = 144 px, 0xB00 = 176 px, 0x400 = 64 px, 0x800 = 128 px (1/16 px).
 * ---------------------------------------------------------------------- */
int dd2_cam_x, dd2_cam_y;              /* [0x9060], [0x9062] */
int dd2_cam_l, dd2_cam_r;              /* [0x4A88], [0x4A82] */
int dd2_cam_t, dd2_cam_b;              /* [0x4A8A], [0x4A84] */

/* 0x0ED0: camera limits.
     [0x4A88] = [0x4A8A] = 0x200            (left and top -- 32 px, not 0!)
     [0x4A82] = (level_w - 0x16) << 8       (right)
     [0x4A84] = (level_h - 0x0F) << 8       (bottom)
   These four values are exactly what 0x1BDC and 0x1CB0 read. */
void Level_SetCameraLimits(int w_tiles, int h_tiles)   /* 0x0ED0 */
{
    dd2_cam_l = 0x200;
    dd2_cam_t = 0x200;
    dd2_cam_r = (w_tiles - 0x16) << 8;
    dd2_cam_b = (h_tiles - 0x0F) << 8;
    if (dd2_cam_r < dd2_cam_l) dd2_cam_r = dd2_cam_l;
    if (dd2_cam_b < dd2_cam_t) dd2_cam_b = dd2_cam_t;
}

/* 0x1CB0 -- camera setup at level start (NOT the same as 0x1BDC):
     [0x9060] = player_x - 0xA00 (160 px), [0x9062] = player_y - 0x700 (112 px)
     clamped to the level limits, then the clipping fields
     [0x8C44] = (cam_x >> 8) & 0xFE, [0x8C68] = (cam_y >> 8) & 0xFE --
     these are exactly what 0x1EAB compares in pass 0. */
void Level_InitCamera(void)                            /* 0x1CB0 */
{
    int px = dd2_objs[0].x, py = dd2_objs[0].y;
    dd2_cam_x = px - 0xA00;
    dd2_cam_y = py - 0x700;
    if (dd2_cam_x < dd2_cam_l || px < 0xA00) dd2_cam_x = dd2_cam_l;
    if (dd2_cam_y < dd2_cam_t || py < 0x700) dd2_cam_y = dd2_cam_t;
    if (dd2_cam_x > dd2_cam_r) dd2_cam_x = dd2_cam_r;
    if (dd2_cam_y > dd2_cam_b) dd2_cam_y = dd2_cam_b;
    dd2_vp_l = ((dd2_cam_x >> 8) & 0xFE);              /* [0x8C44] */
    dd2_vp_t = ((dd2_cam_y >> 8) & 0xFE);              /* [0x8C68] */
}

void Level_Camera(void)                                /* 0x1BDC */
{
    int px = dd2_objs[0].x, py = dd2_objs[0].y, si;

    if (dd2_cam_x + 0x900 > px) {                      /* player left of the frame */
        si = dd2_cam_x + 0x900 - px;
        if (dd2_cam_x - dd2_cam_l < si) dd2_cam_x = dd2_cam_l;
        else dd2_cam_x -= si;
    } else if (dd2_cam_x + 0xB00 < px) {
        si = px - (dd2_cam_x + 0xB00);
        if (dd2_cam_x + si > dd2_cam_r) dd2_cam_x = dd2_cam_r;
        else dd2_cam_x += si;
    }
    if (dd2_cam_y + 0x400 > py) {                      /* player above the frame */
        si = dd2_cam_y + 0x400 - py;
        if (dd2_cam_y - dd2_cam_t < si) dd2_cam_y = dd2_cam_t;
        else dd2_cam_y -= si;
    } else if (dd2_cam_y + 0x800 < py) {
        si = py - (dd2_cam_y + 0x800);
        if (dd2_cam_y + si > dd2_cam_b) dd2_cam_y = dd2_cam_b;
        else dd2_cam_y += si;
    }
    /* 0x1CA7: call 0x29AC -- the viewport box [0x8C44]/[0x8C68] is refreshed
       from the camera EVERY scroll frame.  Pass 1 of 0x1EAB gates each
       object's 0x1D76 on this box, so without the refresh every enemy outside
       the first screen stayed frozen forever.  (0x29AC uses a plain >>8 here;
       only 0x1CB0 masks with 0xFE.) */
    dd2_vp_l = dd2_cam_x >> 8;                         /* [0x8C44] */
    dd2_vp_t = dd2_cam_y >> 8;                         /* [0x8C68] */
}

/* -------------------------------------------------------------------------
 * 0x394F -- doors.  This is the PER-FRAME function of actor-info record 0x870
 * (sprite 43, delay 25, next -> 0x780), not a reaction: when the door object
 * reaches it, the game stamps the door pattern into the level and moves Dave.
 *
 *   desc = [0x906C] -- (col << 8) | row, or 0x8000 (special case)
 *   col/row -> Dave's position ([0x8BDA]/[0x8BDC] = col<<8 / row<<8)
 *   tile = tile plane[row][col]
 *   if the tile behaviour (table [0x5204]) != 2:
 *       code = 3 for tile 0x27A, 4 for 0x2E4, otherwise 2
 *       a 3x2 pattern from DGROUP 0x744 + code*12 + (di*2+si)*2 is written
 *       into tiles [row-1..row+1][col-1..col]
 *   then 0x192E (bbox), a frame copy and 0x1CB0 (camera reset).
 * ---------------------------------------------------------------------- */
int dd2_door_flag;           /* [0x8C38] */

/* DGROUP 0x744: three different door patterns of six tiles each.
   Both 0x394F and 0x3A5B use them. */
static const uint16_t dd2_door_tmpl[5][6] = {
    { 0x0094, 0x0097, 0x0095, 0x0098, 0x0096, 0x0099 },
    { 0x0094, 0x0097, 0x0095, 0x0098, 0x0096, 0x0099 },
    { 0x004E, 0x004F, 0x005B, 0x005C, 0x0068, 0x0069 },
    { 0x004E, 0x004F, 0x005B, 0x005C, 0x0068, 0x0069 },
    { 0x0341, 0x0340, 0x02D4, 0x02D5, 0x02E1, 0x02E2 },
};
int dd2_scan_col, dd2_scan_row;   /* [0x9068], [0x906A] */
int dd2_door_desc;                /* [0x906C] */
int dd2_door_tile;                /* [0x906E] */
int dd2_door_type;                /* [0x9070] */

int Level_Door(uint16_t *tiles, int w, int h, const uint8_t *intile, int desc)
{
    int col, row, tile, code, di, si;

    if (desc == 0x8000) {            /* 0x3964 */
        dd2_door_flag = 1;           /* [0x8C38] = 1 */
        dd2_objs[0].f02 = 0;         /* 0x396A: [0x8BD6] = 0 -- deactivate */
        return 0;
    }
    col = (desc >> 8) & 0x7F;        /* 0x3973 */
    row = desc & 0x7F;               /* 0x3980 */
    /* 0x3989/0x398E write the SCRATCH x/y ([0x8BDA]/[0x8BDC]); 0x1D76 copies
       the scratch back to the object at 0x1E96.  In the port Dave's handler
       keeps its position in dave_x/dave_y and writes it back at the end of
       Dave_React, so writing only dd2_objs[0].x/y was overwritten and the
       teleport was lost (the camera still moved). */
    dave_x = col << 8;               /* [0x8BDA] */
    dave_y = row << 8;               /* [0x8BDC] */
    dd2_objs[0].x = (int16_t)dave_x;
    dd2_objs[0].y = (int16_t)dave_y;
    if (col < 0 || col >= w || row < 0 || row >= h) return 0;

    tile = tiles[row * w + col];     /* 0x3999 */
    if (intile && tile < 1024 && intile[tile] == 2) goto after_stamp;  /* 0x39B8 */

    code = (tile == 0x27A) ? 3 : (tile == 0x2E4) ? 4 : 2;              /* 0x39BD */
    for (di = 0; di < 3; di++)                                         /* 0x39DE */
        for (si = 0; si < 2; si++) {                                   /* 0x39E2 */
            int r = row - 1 + di, c = col - 1 + si;
            if (r < 0 || r >= h || c < 0 || c >= w) continue;
            tiles[r * w + c] = dd2_door_tmpl[code][di * 2 + si];      /* 0x3A1E */
        }
after_stamp:
    Obj_Box(&dd2_objs[0]);           /* 0x3A2D: call 0x192E */
    memcpy(dd2_prev_frame, dd2_cur_frame, sizeof dd2_prev_frame);      /* 0x3A30 */
    Level_InitCamera();              /* 0x3A52: call 0x1CB0 */
    return 1;
}

/* -------------------------------------------------------------------------
 * 0x4422 -- scan of the tiles under Dave's bbox.  It walks the rectangle
 * [left>>8 .. right>>8] x [top>>8 .. bottom>>8] and looks at the tile behaviour
 * (table [0x5204]):
 *
 *   type 0    -- nothing
 *   type 13   -- deadly: [0x8C38] = -8
 *   type 99   -- level exit: [0x8C38] = 1
 *   type 1, 2 -- door/niche: the TAG is taken from the object plane, and if it
 *               is non-zero, then [0x906C] = tag (these are packed (col<<8)|row),
 *               [0x906E] = tile, [0x9070] = type, [0x9068] = column,
 *               [0x906A] = row.  It is [0x906C] that 0x394F reads.
 * ---------------------------------------------------------------------- */
void Level_ScanTiles(const uint16_t *tiles, const uint16_t *objs, int w, int h,
                     const uint8_t *intile, int l, int t, int r, int b)
{
    int di, si;

    dd2_scan_col = 0;                                  /* [0x9068] = 0 */
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r >= w) r = w - 1;
    if (b >= h) b = h - 1;
    for (di = l; di <= r; di++)                        /* 0x44CF */
        for (si = t; si <= b; si++) {                  /* 0x44C5 */
            int tile = tiles[si * w + di];             /* 0x443E */
            int type = intile ? intile[tile] : 0;      /* 0x4454 */
            if (type == 0) continue;
            if (type == 13) { dd2_door_flag = -8; continue; }   /* 0x44B6 */
            if (type == 99) { dd2_door_flag = 1;  continue; }   /* 0x44BE */
            if (type != 1 && type != 2) continue;      /* 0x4476 */
            {
                int tag = objs[si * w + di];           /* 0x447F */
                if (tag == 0) continue;                /* 0x4499 */
                dd2_door_tile = tile;                  /* [0x906E] */
                dd2_door_desc = tag;                   /* [0x906C] */
                dd2_door_type = type;                  /* [0x9070] */
                dd2_scan_col = di;                     /* [0x9068] */
                dd2_scan_row = si;                     /* [0x906A] */
            }
        }
}

/* -------------------------------------------------------------------------
 * 0x3A5B -- fn of record 0x834: "the door opens".
 *
 *   call 0x8CB2(0x0C)                       ; sound
 *   [0x8C60] = 2
 *   ... address of the door in video memory -> [0x8C6E] (not needed by the port:
 *       it draws the level from the tile plane every frame)
 *   code = ([0x906C] > 0x8000) ? 0 : 2
 *   if [0x906C] > 0x8000:                   ; SECRET door
 *       Level_SpawnBonus([0x906C] - 0x8001, [0x9068], [0x906A] - 1)
 *   if [0x906E] == 0x27A -> code++
 *   else if [0x906E] == 0x2E4 -> code += 2
 *   3x2 pattern from DGROUP 0x744 into tiles [row-1..row+1][col-1..col]
 * ---------------------------------------------------------------------- */
int Level_DoorOpen(uint16_t *tiles, int w, int h, int desc, int col, int row,
                   int tile, int *code_out)
{
    int code = (desc > 0x8000) ? 0 : 2;              /* 0x3A92 */
    int di, si;

    if (dd2_sfx) dd2_sfx(0x0C);                      /* 0x3A63: OPENDOOR */
    if (desc > 0x8000)                               /* 0x3AAE: secret door */
        Level_SpawnBonus(desc - 0x8001, col, row - 1);
    if (tile == 0x27A) code++;                       /* 0x3AC4 */
    else if (tile == 0x2E4) code += 2;               /* 0x3AD1 */
    if (code_out) *code_out = code;
    for (di = 0; di < 3; di++)                       /* 0x3AEB */
        for (si = 0; si < 2; si++) {                 /* 0x3AEF */
            int r = row - 1 + di, c = col - 1 + si;
            if (r < 0 || r >= h || c < 0 || c >= w) continue;
            tiles[r * w + c] = dd2_door_tmpl[code][di * 2 + si];   /* 0x3B2C */
        }
    return 1;
}

/* ------------------------------------------------------------------ test */
#ifdef DD2_LEVEL_TEST
int main(void)
{
    uint16_t tags[8 * 4];
    int i, n;
    memset(tags, 0, sizeof tags);
    tags[0] = 0xFF;        /* player */
    tags[1] = 1;           /* zombie */
    tags[2] = 0x0E;        /* prize 100 */
    tags[3] = 8;           /* skip */
    tags[4] = 0x13;        /* prize 1UP index 5 */
    Obj_ClearAll();
    Level_SpawnInit();
    n = Level_SpawnTags(tags, 8, 4);
    printf("spawned objects: %d\n", n);
    printf("player: slot0 type=%d x=%d y=%d dir=%d sprite=%d\n",
           dd2_objs[0].type, dd2_objs[0].x, dd2_objs[0].y,
           dd2_objs[0].dir, dd2_objs[0].sprite);
    for (i = 0; i < DD2_MAXOBJ; i++)
        if (dd2_objs[i].type == 9)
            printf("bonus: slot=%d info=%#06x [+0x4C]=%d\n", i,
                   (unsigned short)dd2_objs[i].info, dd2_objs[i].f48[2]);
    for (i = 0; i < DD2_MAXOBJ; i++)
        if (dd2_objs[i].type == 2)
            printf("zombie: slot=%d info=%#06x x=%d y=%d dir=%d health=%d [+0x3C]=%d\n",
                   i, (unsigned short)dd2_objs[i].info, dd2_objs[i].x, dd2_objs[i].y,
                   dd2_objs[i].dir, dd2_objs[i].health, dd2_objs[i].f3c);
    printf("flags: [0x51F0](tag1)=%d [0x51F2](tag2)=%d\n", dd2_tag_flags[1], dd2_tag_flags[2]);
    /* door: stamp of the 3x2 pattern */
    {
        uint16_t t2[8 * 6]; uint8_t beh[1024];
        int r;
        memset(t2, 0, sizeof t2); memset(beh, 0, sizeof beh);
        t2[3 * 8 + 3] = 0x027A;              /* tile 0x27A -> code 3 */
        beh[0x027A] = 0;
        dd2_objs[0].x = dd2_objs[0].y = 0;
        r = Level_Door(t2, 8, 6, beh, (3 << 8) | 3);
        printf("door 0x27A: stamp=%d, tiles around =", r);
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 0; dx++)
            printf(" %#06x", t2[(3 + dy) * 8 + (3 + dx)]);
        printf("\n");
        printf("Dave pos after door: x=%d y=%d (expected 768/768)\n",
               dd2_objs[0].x, dd2_objs[0].y);
    }
    /* tile scan: door tile with a tag -> descriptor */
    {
        uint16_t t3[8 * 6], o3[8 * 6]; uint8_t beh[1024];
        memset(t3, 0, sizeof t3); memset(o3, 0, sizeof o3); memset(beh, 0, sizeof beh);
        t3[4 * 8 + 5] = 0x027A; beh[0x027A] = 1;      /* door */
        o3[4 * 8 + 5] = (5 << 8) | 4;                 /* tag = (col,row) */
        dd2_door_desc = 0; dd2_door_flag = 0;
        Level_ScanTiles(t3, o3, 8, 6, beh, 5, 4, 5, 4);
        printf("scan: descriptor=%#06x tile=%#06x type=%d col=%d row=%d (expected 0x0504/0x27A/1/5/4)\n",
               dd2_door_desc, dd2_door_tile, dd2_door_type, dd2_scan_col, dd2_scan_row);
        beh[0x027A] = 13; dd2_door_flag = 0;
        Level_ScanTiles(t3, o3, 8, 6, beh, 5, 4, 5, 4);
        printf("deadly tile: [0x8C38] = %d (expected -8)\n", dd2_door_flag);
        beh[0x027A] = 99; dd2_door_flag = 0;
        Level_ScanTiles(t3, o3, 8, 6, beh, 5, 4, 5, 4);
        printf("level exit: [0x8C38] = %d (expected 1)\n", dd2_door_flag);
    }
    /* door opening: 0x3A5B */
    {
        uint16_t t4[8 * 6]; int code = -1;
        memset(t4, 0, sizeof t4);
        Level_DoorOpen(t4, 8, 6, 0x0304, 3, 3, 0x027A, &code);
        printf("open door 0x27A: code=%d (expected 3), tiles =", code);
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 0; dx++)
            printf(" %#06x", t4[(3 + dy) * 8 + (3 + dx)]);
        printf("\n");
        memset(t4, 0, sizeof t4); code = -1;
        Level_DoorOpen(t4, 8, 6, 0x8005, 3, 3, 0x0001, &code);
        printf("secret door: code=%d (expected 0)\n", code);
    }
    return 0;
}
#endif
