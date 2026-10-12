/* input.h -- DD2 input layer (see input.c): the original's model
   DS:0x8BB2 = {cmd, button1, button2}.  GAME code -- no SDL here.
   Keys are DOS scan codes; the platform layer provides the key state. */
#ifndef DD2_INPUT_H
#define DD2_INPUT_H
#include <stdbool.h>
#include <stdint.h>

extern uint8_t dd2_cmd;       /* [0x8BB2]: 0=N 2=E 4=S 6=W 8=neutral */
extern uint8_t dd2_button1;   /* [0x8BB3] jump (DS:0x9744) */
extern uint8_t dd2_button2;
extern int dd2_input_mode;   /* [0x97D6]: 0 = keyboard, 2/3 = joystick */
extern int dd2_button_swap;  /* [0xA6DE]: F4 button-1 answer */   /* [0x8BB4] fire (DS:0x9745) */

void Input_Init(void);
void Input_Frame(const bool *keys);   /* 0x71A5: build the frame structure */
int  Input_WaitKey(void);             /* 0x75F1: wait for a key press */
int  Input_CmdFromIndex(int idx);     /* 0x72D6/0x7539: index 0..8 -> command */
const char *Input_KeyName(int sc);    /* 0x6DF5/0x6E88: key name */
int  Input_ActionKey(int i);          /* DS:0x979C+2*i binding */
void Input_SetActionKey(int i, int sc);
int  Input_BtnKey(int i);             /* DS:0x9744/0x9745 */
void Input_SetBtnKey(int i, int sc);
int  Input_LineEdit(char *buf, int maxlen, void (*echo)(int ch));   /* 0x86AC */
int  Input_DirX(void);
int  Input_DirY(void);
int  Input_MenuKey(const bool *keys);      /* 0x41C */
const char *Input_MenuKeyName(int dos_code);

#endif
