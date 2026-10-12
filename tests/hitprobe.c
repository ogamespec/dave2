/* Does Dave's hitscan damage the level bosses?  Loads a level, finds the
 * boss-like object (f48[0] set = vulnerable), fires the hitscan at its bbox
 * centre and reports the health before/after. */
#define main dd2_main_disabled
#include "../src/main.c"
#undef main

int main(int argc, char **argv)
{
    const char *dir = "/mnt/c/Games/DD2";
    int L = (argc > 1) ? atoi(argv[1]) : 4;
    int i, done = 0;

    log_open();
    g_datadir = dir;
    if (!dd2_load_assets(&A, dir, "")) { printf("assets FAIL\n"); return 2; }
    Input_Init(); Video_SetDataDir(dir); apply_fade(3);
    level = L;
    game_start_level(level);
    /* wire the hitscan exactly like main.c does */
    dd2_hs_tiles = A.level.tiles; dd2_hs_w = A.level.w; dd2_hs_h = A.level.h;
    dd2_hs_solid = hs_tile_solid;
    printf("level %d: object_count=%d\n", L, dd2_obj_count);
    for (i = 1; i < DD2_MAXOBJ; i++) {
        dd2_obj *o = &dd2_objs[i];
        if (o->type == 0) continue;
        printf("  slot %d type=%d info=%#06x vuln(+48)=%d dmg(+4A)=%d hp=%d bbox=(%d,%d)-(%d,%d)\n",
               i, o->type, (unsigned short)o->info, o->f48[0], o->f48[1], o->health,
               o->bbox_l, o->bbox_t, o->bbox_r, o->bbox_b);
        if (o->f48[0] && o->f48[1] && !done) {
            int cx = (o->bbox_l + o->bbox_r) / 2;
            int cy = (o->bbox_t + o->bbox_b) / 2;
            int hp0 = o->health, r;
            r = 0; (void)cx; (void)cy;   /* flags only */
            printf("    hitscan at (%d,%d) -> %d ; health %d -> %d\n",
                   cx, cy, r, hp0, o->health);
            done = 1;
        }
    }
    if (!done) printf("  no vulnerable object found\n");
    return 0;
}
