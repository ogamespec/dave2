/*
 * level.h -- level spawn from the tag plane (original 0x53FF/0x3741/0x3539).
 */
#ifndef DD2_LEVEL_H
#define DD2_LEVEL_H
#include <stdint.h>

extern int dd2_tag_flags[10];   /* [0x51EE]..[0x5200] -- which types are present */
extern int dd2_level_w;         /* [0x9EB0] */
extern int dd2_level_h;         /* [0xA6DA] */

/* Enemy spawners: the index is the tag (1..10, 11..13).  Ported in item 3. */
extern void (*dd2_enemy_spawn[16])(int col, int row);

void Level_SpawnInit(void);
void dd2_rand_seed(int use_clock);   /* 0x8E8D: seed [0x2EAE] */
extern int (*dd2_clock_hundredths)(void);  /* platform clock hook */
void dd2_rand_set(int idx);          /* test hook: force [0x2EAE] */
int  dd2_rand_byte(void);            /* 0x8EAF: cyclic table walk */
extern int dd2_cam_x, dd2_cam_y, dd2_cam_l, dd2_cam_r, dd2_cam_t, dd2_cam_b;
void Level_SetCameraLimits(int w_tiles, int h_tiles);
void Level_InitCamera(void);      /* 0x1CB0 */
void Level_Camera(void);          /* 0x1BDC */
extern int dd2_door_flag;                 /* [0x8C38] */
extern int dd2_scan_col, dd2_scan_row, dd2_door_desc, dd2_door_tile, dd2_door_type;
void Level_ScanTiles(const uint16_t *tiles, const uint16_t *objs, int w, int h,
                     const uint8_t *intile, int l, int t, int r, int b);  /* 0x4422 */
int  Level_DoorOpen(uint16_t *tiles, int w, int h, int desc, int col, int row,
                    int tile, int *code_out);   /* 0x3A5B */
int  Level_Door(uint16_t *tiles, int w, int h, const uint8_t *intile, int desc);  /* 0x394F */
int  Level_SpawnTags(const uint16_t *tags, int w, int h);   /* 0x53FF */

/* Level data for the conditional spawn of tag 3 (0x47E6); set by main.c */
extern const uint16_t *dd2_spawn_tiles;
extern const uint8_t  *dd2_spawn_north;
void Level_SpawnPlayer(int col, int row);                   /* 0x3741 */
int  Level_SpawnBonus(int idx, int col, int row);           /* 0x3539 */

#endif /* DD2_LEVEL_H */
