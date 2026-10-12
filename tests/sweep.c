/* Cross-level collision sweep for the DD2 port.
 *
 *   make sweep && ./sweep
 *
 * Runs every level for 900 frames with a scripted input (walk right/left,
 * periodic jump and fire), driving the SAME static dispatchers as the game by
 * including main.c.  It reports, per level:
 *
 *   frames   how far the run got
 *   deaths   times [0x8C38] went negative (touching an enemy / hazard)
 *   final_y  Dave's last y
 *   max_y    the lowest point he reached
 *   bottom   level height * 256
 *   verdict  "FELL THROUGH" when he leaves the map, otherwise "ok"
 *
 * This is the regression test for the collision push-out: the right-side
 * correction in Obj_PushOutX (0x1AFB) used to have its operands swapped, so
 * Dave was pushed INTO walls and could walk off the right edge of levels 3,
 * 6 and 7 and fall forever.
 */
#define main dd2_main_disabled
#include "../src/main.c"
#undef main
extern int dd2_unported_fn, dd2_unported_react;

int main(void)
{
    bool keys[128];
    int L, f, deaths;
    const char *dir = "/mnt/c/Games/DD2";
    int fails = 0;

    log_open();                        /* so DD2_TRACE_UNPORTED reaches a file */
    g_datadir = dir;
    if (!dd2_load_assets(&A, dir, "")) { printf("assets: FAIL\n"); return 2; }
    Input_Init();
    apply_fade(3);
    printf("level  frames  deaths  final_y   max_y   bottom  verdict\n");
    for (L = 1; L <= 8; L++) {
        if (!dd2_load_level(&A, dir, L)) { printf("L%d: load FAIL\n", L); fails++; continue; }
        Obj_ClearAll();
        Dave_Init();
        dave_ammo = 8;
        dd2_sprite_bbox = port_sprite_bbox;
        Level_SpawnInit();
        dd2_spawn_tiles = A.level.tiles;
        dd2_spawn_north = A.north;
        Level_SpawnTags(A.level.objs, A.level.w, A.level.h);
        Level_SetCameraLimits(A.level.w, A.level.h);
        Level_InitCamera();
        dd2_obj_perframe = obj_perframe;
        dd2_sfx = 0;
        dd2_extra_sprite = extra_sprite;
        dd2_tile_south = tile_south;
        dd2_door_fn = dave_door;
        dd2_frame_react = obj_react;
        dd2_pair_react = Obj_PairReact;
        dd2_hs_tiles = A.level.tiles;
        dd2_hs_w = A.level.w;
        dd2_hs_h = A.level.h;
        dd2_hs_solid = hs_tile_solid;
        dd2_door_flag = 0;
        dd2_lives = 4;
        deaths = 0;
        {
            int maxy = -100000, bottom = A.level.h * 256, through = 0;
            int oob = 0, first_oob = -1;
            for (f = 0; f < 900; f++) {
                int phase = (f / 90) % 4;
                memset(keys, 0, sizeof keys);
                if (phase == 0 || phase == 2) keys[0x4D] = true;   /* right */
                else                          keys[0x4B] = true;   /* left */
                if (f % 41 < 3) keys[0x1D] = true;                 /* jump */
                if (f % 67 < 2) keys[0x38] = true;                 /* fire */
                Input_Frame(keys);
                dave_cmd = dd2_cmd;
                dave_button1 = dd2_button1;
                dave_button2 = dd2_button2;
                dave_frame_delta = 1;
                dd2_ticks += 2;
                Obj_Delta();
                if (dd2_button2 == 0) dave_fire_latch = 1;
                Obj_Frame();
                Level_Camera();
                Level_ScanTiles(A.level.tiles, A.level.objs, A.level.w, A.level.h,
                                A.intile, dd2_objs[0].tx_l, dd2_objs[0].tx_t,
                                dd2_objs[0].tx_r, dd2_objs[0].tx_b);
                if (dd2_objs[0].y > maxy) maxy = dd2_objs[0].y;
                if (dd2_door_flag != 0) {
                    if (dd2_door_flag < 0 && ++deaths > 20) break;
                    dd2_door_flag = 0;
                }
                if (dd2_objs[0].type == 0) break;
                if (dd2_objs[0].y > bottom + 128) { through = 1; break; }
                /* enemies/objects must not leave the map either (projectiles,
                   effects and the score pop-ups are allowed to). */
                {
                    int k;
                    for (k = 1; k < dd2_obj_count; k++) {
                        dd2_obj *e = &dd2_objs[k];
                        if (e->f02 == 0 || e->type == 0) continue;
                        switch (e->type) {
                        case 0x0A: case 0x0B: case 0x0C: case 0x0D:
                        case 0x0E: case 0x0F: case 0x11: case 0x12:
                        case 0x13: continue;         /* bullets / effects / score */
                        default: break;
                        }
                        if (e->y > bottom + 128 || e->x < -256 || e->x > A.level.w * 256 + 256) {
                            if (first_oob < 0) first_oob = k;
                            if (oob < 3)
                                printf("   oob f=%d slot %d type %d info %#x x=%d y=%d (w=%d h=%d) dir=%d\n",
                                       f, k, e->type, e->info, e->x, e->y, A.level.w, A.level.h, e->dir);
                            oob++;
                        }
                    }
                }
            }
            printf("L%d     %6d  %6d  %7d  %7d  %7d  %s%s\n",
                   L, f, deaths, dd2_objs[0].y, maxy, bottom,
                   through ? "FELL THROUGH" : "ok",
                   oob ? "  OBJECTS-OOB" : "");
            if (first_oob >= 0)
                printf("       first out-of-bounds object: slot %d type %d info %#x\n",
                       first_oob, dd2_objs[first_oob].type, dd2_objs[first_oob].info);
            /* Only the PLAYER leaving the level is a hard failure: these are
               the disassembly-backed invariants (0x1AD0/0x19EE flat indexing
               plus 0x30C0 culling).  Enemies can legitimately end up outside
               the map when the level has an open edge -- 0x30C0 only removes
               them once they leave the camera window -- so that is a warning,
               not a failure. */
            if (through) fails++;
            if (oob) printf("       warning: %d enemy frames out of map (0x30C0 rules)\n", oob);
        }
    }
    printf("dispatch: unported fn=%d react=%d\n", dd2_unported_fn, dd2_unported_react);
    if (dd2_unported_fn || dd2_unported_react) fails++;
    printf(fails ? "sweep: %d level(s) FAILED\n" : "sweep: all levels ok\n", fails);
    return fails ? 1 : 0;
}
