/* End-to-end test for the level flow: death -> game over -> new game.
 *
 *   make flow && ./flow
 *
 * The collision sweep (tests/sweep.c) resets the end-of-level flag itself and
 * therefore never runs level_after_frame(); this test drives that function for
 * real, so it exercises the whole chain
 *
 *     hazard/death -> 0x2338 (dec [0x8BC8]) -> 0x2343 (GAME OVER panel)
 *                  -> 0x239E (high score + "Continue from level N ?")
 *                  -> 0x2566/0x21C5 (restart)  or  0x256E (back to the title)
 *
 * and asserts the invariants that the 2024-10 "lives ran into the negative"
 * bug violated:
 *
 *   * the life counter never goes below zero,
 *   * the score never decreases while a game is running,
 *   * the level number stays inside 1..9.
 *
 * Run headless:  SDL_VIDEODRIVER=dummy ./flow
 */
#define main dd2_main_disabled
#include "../src/main.c"
#undef main

int main(void)
{
    bool keys[128];
    int f, failures = 0;
    int min_lives = 999, max_lives = -1, game_overs = 0, restarts = 0;
    unsigned prev_score = 0, score_drops = 0;
    const char *dir = "/mnt/c/Games/DD2";

    log_open();
    g_datadir = dir;
    if (!dd2_load_assets(&A, dir, "")) { printf("assets: FAIL\n"); return 2; }
    Input_Init();
    Video_SetDataDir(dir);
    apply_fade(3);
    level = 1;
    dd2_lives = 4;
    dd2_score_lo = dd2_score_hi = 0;
    game_start_level(level);

    for (f = 0; f < 4000; f++) {
        memset(keys, 0, sizeof keys);
        keys[0x4D] = (f / 120) % 2 == 0;                 /* walk right/left */
        if (f % 41 < 3) keys[0x1D] = true;               /* jump */
        if (f % 67 < 2) keys[0x38] = true;               /* fire */
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

        /* Force the hazard that the flow is supposed to survive: every 300
           frames pretend the deadly tile was touched (0x44B6 sets -8). */
        if (dd2_door_flag == 0 && (f % 300) == 299)
            dd2_door_flag = -8;

        if (level_after_frame()) {
            if (dd2_lives <= 0) game_overs++;            /* the GAME OVER branch */
            /* Mirror the main loop: 0x256E sends the game back to the title,
               and the title's 0x15EE calls 0x21B0 to start a new one. */
            if (game_to_title) { game_to_title = 0; game_restart(); restarts++; }
            prev_score = 0;                              /* a restart zeroes it */
        }

        if (dd2_lives < min_lives) min_lives = dd2_lives;
        if (dd2_lives > max_lives) max_lives = dd2_lives;
        if (dd2_lives < 0) {
            printf("FAIL: lives went negative (%d) at frame %d\n", dd2_lives, f);
            failures++;
            break;
        }
        {
            unsigned sc = dd2_score_lo + (dd2_score_hi * 65536u);
            if (sc < prev_score) { score_drops++; }
            prev_score = sc;
        }
        if (level < 1 || level > 9) {
            printf("FAIL: level %d out of range at frame %d\n", level, f);
            failures++;
            break;
        }
    }

    printf("flow: frames=%d lives %d..%d  game overs=%d  restarts=%d  "
           "score drops=%u\n", f, min_lives, max_lives, game_overs, restarts,
           score_drops);
    if (score_drops) { printf("FAIL: score decreased %u times\n", score_drops); failures++; }
    if (!game_overs) { printf("FAIL: no game over was reached\n"); failures++; }
    printf(failures ? "flow: FAILED\n" : "flow: ok\n");
    return failures ? 1 : 0;
}
