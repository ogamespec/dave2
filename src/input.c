/*
 * input.c -- reconstructed Dangerous Dave 2 (DOS) input layer.  GAME code.
 *
 * The original installs its own INT 9 handler (DD2.EXE 0x165 / 0x1A8 / 0x1D5),
 * keeps an array of key states and once per frame builds the structure
 *
 *     DS:0x8BB2 = { cmd, button1, button2 }        (filled by 0x71A5,
 *                                                   called from 0x754B)
 *
 * where cmd: 0 = north, 2 = east, 4 = south, 6 = west, 8 = neutral,
 * button1 = DS:0x9744 (jump), button2 = DS:0x9745 (fire).
 * The directions come from the table DS:0x979C/0x979E/0x97A0/0x97A2
 * (north/east/south/west), as proven by the configurator strings
 * "1 north / 2 east / 3 south / 4 west / 5 button1 / 6 button2".
 *
 * This file reproduces that logic and nothing else: it works with DOS scan
 * codes and produces exactly the cmd/button values the original produced, so
 * Dave's game logic (0x37AE and its handlers) works unchanged.  All SDL
 * handling lives in the platform layer (Plat_Keys, Plat_WaitScancode, ...).
 */
#include "input.h"
#include "platform.h"
#include "video.h"
#include <string.h>

/* DS:0x8BB2 -- frame structure, exactly as in the original */
uint8_t dd2_cmd;        /* [0x8BB2] */
uint8_t dd2_button1;    /* [0x8BB3] == DS:0x9744 (jump) */
uint8_t dd2_button2;    /* [0x8BB4] == DS:0x9745 (fire) */

/* DS:0x979C..0x97A3 -- remappable "actions", in the original's table order:
   0 north (0x979C), 1 north-east (0x979D), 2 east (0x979E), 3 south-east
   (0x979F), 4 south (0x97A0), 5 south-west (0x97A1), 6 west (0x97A2),
   7 north-west (0x97A3).  The port defaults to the original's fallback in
   0x8A7A: numpad 8/9/6/3/2/1/4/7. */
static int act_key[8]; /* 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW (DOS scan codes) */
static int btn_key[2];  /* 0=button1 (jump), 1=button2 (fire) */

void Input_Init(void)
{
    act_key[0] = 0x48;      /* up          -> north      */
    act_key[1] = 0x49;      /* PgUp        -> north-east */
    act_key[2] = 0x4D;      /* right       -> east       */
    act_key[3] = 0x51;      /* PgDn        -> south-east */
    act_key[4] = 0x50;      /* down        -> south      */
    act_key[5] = 0x4F;      /* End         -> south-west */
    act_key[6] = 0x4B;      /* left        -> west       */
    act_key[7] = 0x47;      /* Home        -> north-west */
    btn_key[0] = 0x1D;      /* CTRL = button 1, as in the DOS version */
    btn_key[1] = 0x38;      /* ALT  = button 2 (fire)                  */
    dd2_cmd = 8;
}

/* 0x71A5: build the per-frame input structure.
   The original tests the eight action keys in the order north, east, south,
   west, north-east, north-west, south-east, south-west and OVERWRITES the
   di/si direction components each time a key is down (so a later key wins;
   the diagonals are tested last and therefore beat the cardinals).  The
   command byte is then cs:[bx+0x72D6] with bx = 3*di + si + 4:
     0:7(NW) 1:0(N) 2:1(NE) 3:6(W) 4:8(none) 5:2(E) 6:5(SW) 7:4(S) 8:3(SE) */
static const uint8_t input_cmd_table[9] = { 7, 0, 1, 6, 8, 2, 5, 4, 3 };

/* The joystick path (0x73AE) maps its index through the same table
   (jump table CS:0x7539 -> arms storing 7,0,1,6,8,2,5,4,3). */
int Input_CmdFromIndex(int idx)
{
    if (idx < 0) return -1;
    if (idx > 8) idx = 4;
    return input_cmd_table[idx];
}

void Input_Frame(const bool *keys)
{
    int di = 0, si = 0, bx;
    dd2_button1 = keys[btn_key[0]] ? 1 : 0;
    dd2_button2 = keys[btn_key[1]] ? 1 : 0;
    if (keys[act_key[0]]) di = -1;               /* 0x71B1 north */
    if (keys[act_key[2]]) si =  1;               /* 0x71C2 east  */
    if (keys[act_key[4]]) di =  1;               /* 0x71D3 south */
    if (keys[act_key[6]]) si = -1;               /* 0x71E4 west  */
    if (keys[act_key[1]]) { di = -1; si =  1; }  /* 0x71F5 north-east */
    if (keys[act_key[7]]) { di = -1; si = -1; }  /* 0x7209 north-west */
    if (keys[act_key[3]]) { di =  1; si =  1; }  /* 0x721D south-east */
    if (keys[act_key[5]]) { di =  1; si = -1; }  /* 0x7231 south-west */
    bx = 3 * di + si + 4;                        /* 0x7245..0x7251 */
    dd2_cmd = (bx >= 0 && bx <= 8) ? input_cmd_table[bx] : 8;
}

/* 0x6DF5 and 0x6E88: KEY NAME for a scan code -- two tables the original
   walks with comparisons.  The strings live in DS at 0x29E3..; the port
   returns a C string instead of a pointer. */
/* DS:0x27F8 -- scan code -> character, used as the KeyName fallback. */
static const unsigned char key_char_tab[128] = {
    0x3f,0x3f,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x30,0x2d,0x2b,0x3f,0x3f,
    0x51,0x57,0x45,0x52,0x54,0x59,0x55,0x49,0x4f,0x50,0x5b,0x5d,0x7c,0x3f,0x41,0x53,
    0x44,0x46,0x47,0x48,0x4a,0x4b,0x4c,0x3b,0x22,0x3f,0x3f,0x3f,0x5a,0x58,0x43,0x56,
    0x42,0x4e,0x4d,0x2c,0x2e,0x2f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,
    0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x0f,0x3f,0x2d,0x15,0x35,0x11,0x2b,0x3f,
    0x13,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,
    0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,
    0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f,0x3f
};

const char *Input_KeyName(int sc)
{
    /* 0x6DF5: the original compares the scan code against a list and returns a
       pointer to a string in DS; 0x6E5D builds "F" + itoa(sc-0x3A) for 0x3B..0x44
       (F1..F10); 0x6F42 falls back to the per-scan-code character table at
       DS:0x27F8 and returns that single character. */
    static char buf[4];
    int c = sc & 0x7F;
    switch (c) {
    case 0x01: return "ESC";     case 0x0E: return "BKSP";
    case 0x0F: return "TAB";     case 0x1D: return "CTRL";
    case 0x2A: return "LSHIFT";  case 0x39: return "SPACE";
    case 0x3A: return "CAPSLK";  case 0x57: return "F11";
    case 0x59: return "F12";     case 0x46: return "SCRLLK";
    case 0x1C: return "ENTER";   case 0x36: return "RSHIFT";
    case 0x37: return "PRTSC";   case 0x38: return "ALT";
    case 0x47: return "HOME";    case 0x49: return "PGUP";
    case 0x4F: return "END";     case 0x51: return "PGDN";
    case 0x52: return "INS";     case 0x53: return "DEL";
    case 0x45: return "NUMLK";   case 0x48: return "UP";
    case 0x50: return "DOWN";    case 0x4B: return "LEFT";
    case 0x4D: return "RIGHT";
    default:
        if (c >= 0x3B && c <= 0x44) {          /* 0x6E5D: F1..F10 */
            int n = c - 0x3A;
            buf[0] = 'F';
            if (n < 10) { buf[1] = (char)('0' + n); buf[2] = 0; }
            else { buf[1] = '1'; buf[2] = (char)('0' + n - 10); buf[3] = 0; }
            return buf;
        }
        buf[0] = (char)key_char_tab[c];        /* 0x6F42: DS:0x27F8 */
        buf[1] = 0;
        return buf;
    }
}


/* DS:0x97D6 -- the input device mode: 0 = keyboard (the boot default,
   set by 0x1705), 2/3 = joystick (set by the F4 calibration 0x6DB6/0x6DBE).
   0x754B dispatches on it.  DS:0xA6DE -- the F4 "(A)ttack or (J)ump" answer. */
int dd2_input_mode;
int dd2_button_swap;

/* Bindings: DS:0x979C..0x97A3 (the port keeps the original's order:
   0 north, 1 north-east, 2 east, 3 south-east, 4 south, 5 south-west,
   6 west, 7 north-west) and DS:0x9744/0x9745 for the two buttons. */
int  Input_ActionKey(int i)   { return (i >= 0 && i < 8) ? act_key[i] : 0; }
void Input_SetActionKey(int i, int sc) { if (i >= 0 && i < 8) act_key[i] = sc & 0xFF; }
int  Input_BtnKey(int i)      { return (i >= 0 && i < 2) ? btn_key[i] : 0; }
void Input_SetBtnKey(int i, int sc) { if (i >= 0 && i < 2) btn_key[i] = sc & 0xFF; }

/* 0x75F1: clear the keyboard state (0x75D7) and WAIT for a key press.
   The original returns the key code in AH/AL; the port only needs the fact
   that a key was pressed.  The wait is bounded by the platform so that the
   game can never hang forever. */
#ifndef DD2_INPUT_TEST      /* the self-test does not link the platform */
int Input_WaitKey(void)
{
    /* 0x75F1 with 0x75D7 and 0x754B:
         0x75F7  call 0x75D7                 ; clear [0x2C04] and [0x2C02]
         loop:
         0x75FA  if ([0x2C02] > 0x7F) { [0x2C02] &= 0x7F; return; }  ; a key
         0x7608  call 0x754B(..., 1, ...)    ; read the joystick/buttons
         0x7627  if (no button) -> loop
       There is NO timeout: the original waits until a keyboard key OR a
       joystick button arrives.  The port used a flat 5 s timeout, so the
       Y/N prompts and the ending/info screens dismissed themselves. */
    uint32_t t0 = Plat_Ticks();
    for (;;) {
        int sc = Plat_WaitScancode(7);          /* ~one 140 Hz tick */
        if (sc > 0) return sc;                  /* 0x75FA: a key */
        if (sc < 0) return 0;                   /* the window was closed */
        if (Plat_PadButtons() & 3) return 0x39; /* 0x7627: a pad button */
        /* Headless runs (dummy video driver, --record/--shot) have no way to
           deliver a key, so the otherwise endless wait is bounded there; a
           real window waits forever, as the original does. */
        if (g_fast && Plat_Ticks() - t0 > 250) return 0;
    }
}

/* 0x86AC -- LINE EDITOR (used by the F-menu).  Original behaviour:
     buffer (arg1), limit (arg2); loop waits for a key (0x87B9), lower-cases
     it (0xBDAF), handles backspace 0x08 / delete 0x7F, accepts characters
     0x20..0x7A while si < limit, finishes on ESC (0x1B) or Enter (0x0D),
     pads the buffer with zeroes and returns 1 for Enter, otherwise 0. */
int Input_LineEdit(char *buf, int maxlen, void (*echo)(int ch))
{
    int si = 0, ch = 0;
    if (!buf || maxlen <= 0) return 0;
    for (;;) {
        int sc = Plat_WaitScancode(5000);            /* 0x87B9 */
        if (sc <= 0) { ch = 0x1B; break; }           /* timeout / quit -> abort */
        ch = Plat_ScancodeChar(sc);                  /* 0xBDAF */
        /* 0xBDAF: `test byte [bx+0x46E9],8` / `add ax,0xFFE0` -- the ctype
           bit is set for 0x61..0x7A, so a-z is converted to UPPER case. */
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch == 0x7F || ch == 8) {                 /* 0x86CD: backspace */
            if (si > 0) {                            /* 0x86D9 */
                si--;
                if (echo) echo(buf[si]);             /* erase the character */
            }
            continue;
        }
        if (ch >= 0x20 && ch <= 0x7A && si < maxlen) {   /* 0x8709 */
            buf[si++] = (char)ch;
            if (echo) echo(ch);
        }
        if (ch == 0x1B || ch == 0x0D) break;         /* 0x873A: ESC or Enter */
    }
    while (si < maxlen) buf[si++] = 0;               /* 0x874D */
    return ch == 0x0D ? 1 : 0;                       /* 0x8759 */
}
#else
int Input_WaitKey(void) { return 0; }
int Input_LineEdit(char *buf, int maxlen, void (*echo)(int ch))
{
    (void)buf; (void)maxlen; (void)echo;
    return 0;
}
#endif

int Input_DirX(void) { return dd2_cmd == 2 ? 1 : (dd2_cmd == 6 ? -1 : 0); }
int Input_DirY(void) { return dd2_cmd == 4 ? 1 : (dd2_cmd == 0 ? -1 : 0); }

/* ------------------------------------------------------ F-menu (0x41C) --- */
/* The original dispatcher reads the last scan code from [0x2C02] (0x80 means
   key release) and switches on it:
       0x01 ESC -> 0x51F   quit / confirm quit
       0x0F TAB -> 0x55B   status window ("TAB : Status window")
       0x3B F1  -> 0x75D7 + 0x239   information/help
       0x3C F2  -> 0x47F   sound on/off ("F2 : Sound on / off")
       0x3D F3  -> 0x4C8   keyboard mode / custom
       0x3E F4  -> 0x4D1   joystick mode
       0x3F F5  -> 0x4E0   reset game ("F5 : Reset game")
       0x58 F12 -> 0x519
   Below is the same dispatch, on DOS scan codes. */
static const struct { int dos; const char *what; } menu_keys[] = {
    { 0x01, "ESC  quit" },
    { 0x0F, "TAB  status window" },
    { 0x3B, "F1   information" },
    { 0x3C, "F2   sound on/off" },
    { 0x3D, "F3   keyboard mode/custom" },
    { 0x3E, "F4   joystick mode" },
    { 0x3F, "F5   reset game" },
    { 0x58, "F12" },
    { 0, 0 }
};

/* Returns the DOS scan code if a menu key is held (otherwise 0).
   [0x2C02] in the original holds only the last key -- same idea here. */
int Input_MenuKey(const bool *keys)
{
    int i;
    for (i = 0; menu_keys[i].dos; i++)
        if (keys[menu_keys[i].dos]) return menu_keys[i].dos;
    return 0;
}

const char *Input_MenuKeyName(int dos_code)
{
    int i;
    for (i = 0; menu_keys[i].dos; i++)
        if (menu_keys[i].dos == dos_code) return menu_keys[i].what;
    return "";
}

/* ------------------------------------------------------------- self-test */
#ifdef DD2_INPUT_TEST
#include <stdio.h>
int main(void)
{
    bool keys[128];
    struct { int sc; const char *what; } t[] = {
        { 0x48, "up" }, { 0x4D, "right" }, { 0x50, "down" }, { 0x4B, "left" },
        { 0x49, "PgUp(NE)" }, { 0x51, "PgDn(SE)" }, { 0x4F, "End(SW)" },
        { 0x47, "Home(NW)" },
        { -1, 0 }
    };
    int i;
    Input_Init();
    for (i = 0; t[i].what; i++) {
        memset(keys, 0, sizeof keys);
        keys[t[i].sc] = true;
        Input_Frame(keys);
        printf("%-10s -> cmd=%d (dirX=%d dirY=%d)\n", t[i].what,
               dd2_cmd, Input_DirX(), Input_DirY());
    }
    /* 0x71A5: north+east (two cardinals) combines into NE (cmd 1). */
    memset(keys, 0, sizeof keys);
    keys[0x48] = true; keys[0x4D] = true;
    Input_Frame(keys);
    printf("up+right   -> cmd=%d (expected 1 = NE)\n", dd2_cmd);
    /* a held diagonal beats a held cardinal (diagonals are tested last). */
    memset(keys, 0, sizeof keys);
    keys[0x48] = true; keys[0x4F] = true;     /* up + End(SW) */
    Input_Frame(keys);
    printf("up+End     -> cmd=%d (expected 5 = SW)\n", dd2_cmd);
    memset(keys, 0, sizeof keys);
    keys[0x1D] = true; keys[0x38] = true;      /* CTRL + ALT */
    Input_Frame(keys);
    printf("ctrl+alt -> button1=%d button2=%d\n", dd2_button1, dd2_button2);
    return 0;
}
#endif
