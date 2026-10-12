/* F4 joystick-calibration regression test (`make joycfg && ./joycfg`).
 *
 * Drives the real Screen_JoyConfig() (0x6BB2) with an SDL3 VIRTUAL gamepad and
 * checks it against the disassembly:
 *
 *  1. Spinner position.  0x6BF8-0x6C07 computes the column from the LIVE text
 *     cursor -- `ax = [0x4228]; ax = (ax + 7) >> 3; [0x9340] = ax` -- and
 *     0x915A takes its first argument in BYTES (`add di, [bp+4]`).  The port
 *     used the invented `dd2_text_left + 0x60` and rounded DOWN, so the arrow
 *     landed on "press a button:" (measured 160 px vs the original's 176 px).
 *
 *  2. Corner capture.  0x6C6E/0x6D2A erase the spinner with tile 0x1B and
 *     0x6C80-0x6CA0 (0x6D3C-0x6D58) re-read 0x73AE until button 1 is RELEASED
 *     before the next prompt.  Without that wait the LOWER RIGHT corner is
 *     sampled while the button from the UPPER LEFT press is still down and the
 *     stick is still in the first corner, so min == max and every threshold
 *     pair collapses (low == high == the first corner).  A resting stick then
 *     reads "past" the collapsed threshold: measured cmd 3 (south-east ->
 *     AIM_DN_R), i.e. Dave lies/aims at the ground no matter what, while the
 *     buttons still work.
 *
 * The test scripts the pad on a wall clock: the button is held across the
 * transition (as a player naturally does) and released only later.
 */
#define main dd2_main_disabled
#include "../src/main.c"
#undef main

#include <SDL3/SDL.h>
#include <stdio.h>

static SDL_Joystick *vpad;
static Uint64        v_t0;
static int           v_scripting;      /* 1 while Screen_JoyConfig runs */
static int           v_calls;

/* spinner detection: union of the pixels that changed between two consecutive
   presented frames while the spinner animates (the text around it is static) */
static uint8_t v_prev[DD2_W * DD2_H];
static int     v_prev_ok;
static int     v_spin_minx = 1 << 20, v_spin_maxx = -1;

static void vpad_set(int ax, int ay, int btn)
{
    SDL_SetJoystickVirtualAxis(vpad, 0, (Sint16)ax);
    SDL_SetJoystickVirtualAxis(vpad, 1, (Sint16)ay);
    SDL_SetJoystickVirtualButton(vpad, 0, btn ? true : false);
}

static void vpad_spin_capture(void)
{
    int x, y;
    if (!v_prev_ok) {
        memcpy(v_prev, fb, sizeof v_prev);
        v_prev_ok = 1;
        return;
    }
    for (y = 70; y < 82; y++)
        for (x = 0; x < DD2_W; x++)
            if (v_prev[y * DD2_W + x] != fb[y * DD2_W + x]) {
                if (x < v_spin_minx) v_spin_minx = x;
                if (x > v_spin_maxx) v_spin_maxx = x;
            }
    memcpy(v_prev, fb, sizeof v_prev);
    /* Optional visual check: DD2_JOYCFG_SHOT=<file.bmp> dumps the screen with
       the spinner on it. */
    if (v_spin_maxx > v_spin_minx) {
        static int shot_done;
        const char *shot = getenv("DD2_JOYCFG_SHOT");
        if (shot && !shot_done) {
            static uint32_t rgba[DD2_W * DD2_H];
            int i;
            for (i = 0; i < DD2_W * DD2_H; i++) rgba[i] = cur_pal[fb[i] & 15];
            write_bmp(shot, rgba, DD2_W, DD2_H);
            shot_done = 1;
        }
    }
}

/* SDL calls this from SDL_UpdateJoysticks (i.e. from Plat_PumpEvents inside the
   F4 loops), so it is the script's clock AND its hook into the live screen. */
static void SDLCALL vpad_update(void *ud)
{
    Uint64 t;
    (void)ud;
    if (!v_scripting) return;
    v_calls++;
    t = SDL_GetTicks() - v_t0;
    if      (t <  200) vpad_set(-32768, -32768, 0);   /* UPPER LEFT, no button */
    else if (t <  450) vpad_set(-32768, -32768, 1);   /* press, button still held */
    else if (t <  650) vpad_set(-32768, -32768, 0);   /* release (0x6C80 wait) */
    else if (t <  750) vpad_set( 32767,  32767, 0);   /* move to LOWER RIGHT */
    else if (t <  950) vpad_set( 32767,  32767, 1);   /* second press */
    else if (t < 1100) vpad_set( 32767,  32767, 0);   /* release */
    else               vpad_set(      0,      0, 1);  /* answer 0x6DCD */
    if (t < 450) vpad_spin_capture();                 /* spinner is on screen */
}

static int failures;

static void expect_int(const char *what, int got, int want)
{
    if (got != want) {
        printf("  FAIL %-34s got %d, want %d\n", what, got, want);
        failures++;
    } else {
        printf("  ok   %-34s %d\n", what, got);
    }
}

static int cmd_for(int ax, int ay)
{
    vpad_set(ax, ay, 0);
    Plat_PumpEvents();
    Plat_PumpEvents();
    return Input_CmdFromIndex(Plat_PadCmd());
}

int main(void)
{
    SDL_VirtualJoystickDesc desc;
    const char *dir = "/mnt/c/Games/DD2";
    SDL_JoystickID id;

    setenv("SDL_VIDEODRIVER", "dummy", 1);
    log_open();
    g_datadir = dir;
    if (!dd2_load_assets(&A, dir, "")) { printf("assets: FAIL\n"); return 2; }
    Input_Init();
    if (!Plat_VideoInit(1)) { printf("video: FAIL\n"); return 2; }
    apply_fade(3);
    g_fast = 0;                        /* keep the spinner loops ticking */

    SDL_zero(desc);
    desc.version     = (Uint32)sizeof(desc);
    desc.type        = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes       = 2;
    desc.nbuttons    = 2;
    desc.axis_mask   = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY);
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST);
    desc.name        = "DD2 test pad";
    desc.Update      = vpad_update;
    {
        id = SDL_AttachVirtualJoystick(&desc);
        if (!id) { printf("virtual pad: FAIL (%s)\n", SDL_GetError()); return 2; }
        vpad = SDL_OpenJoystick(id);
        if (!vpad) { printf("open virtual pad: FAIL (%s)\n", SDL_GetError()); return 2; }
    }
    vpad_set(0, 0, 0);
    Plat_PumpEvents();

    printf("F4 joystick calibration (0x6BB2)\n");
    v_scripting = 1;
    v_t0 = SDL_GetTicks();
    Screen_JoyConfig();
    v_scripting = 0;
    printf("  script pumped %d update(s)\n", v_calls);

    /* 0x6BF8: x_bytes = ([0x4228] + 7) >> 3; 0x915A takes x in bytes, and the
       port's Video_Tile8 takes pixels, so the arrow must start at 176. */
    expect_int("spinner left pixel", v_spin_minx, 176);
    expect_int("spinner right pixel", v_spin_maxx, 183);

    /* 0x6DB6: [0x97D6] = 2 */
    expect_int("input mode after F4", dd2_input_mode, 2);

    /* 0x73AE + CS:0x7539: centred stick -> index 4 -> cmd 8 (neutral). */
    expect_int("cmd centred", cmd_for(0, 0), 8);
    expect_int("cmd east", cmd_for(32767, 0), 2);
    expect_int("cmd west", cmd_for(-32768, 0), 6);
    expect_int("cmd north", cmd_for(0, -32768), 0);
    expect_int("cmd south", cmd_for(0, 32767), 4);
    expect_int("cmd north-west", cmd_for(-32768, -32768), 7);
    expect_int("cmd south-east", cmd_for(32767, 32767), 3);

    SDL_CloseJoystick(vpad);
    SDL_DetachVirtualJoystick(id);
    printf("F4 joycfg: %s (%d failure(s))\n", failures ? "FAIL" : "ok", failures);
    return failures ? 1 : 0;
}
