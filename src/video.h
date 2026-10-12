/*
 * video.h -- DD2 video layer: EGA 320x200, 4 planes, functions tagged with
 * their original addresses (see video.c).
 *
 * IMPORTANT: the original screen is not linear 320x200. The CRTC is set to
 * 0x28 words per row (0x7E79) and [0x3004] = 0x50 bytes (0x7FB2), so the
 * frame lives in a 640x200 buffer and the visible 320 px window is selected
 * by the CRT start address (0x90A8: start = s>>3 bytes, pel pan = s&7).
 * That is also where the title screen's horizontal scroll comes from.
 */
#ifndef DD2_VIDEO_H
#define DD2_VIDEO_H

#include <stdint.h>
#include "dd2.h"

#define DD2_W 320
#define DD2_H 200

#define DD2_STRIDE    0x50          /* [0x3004] -- bytes per row (640 px) */
#define DD2_VIRTUAL_W 640
#define DD2_PLANE     0x10000       /* EGA window A000:0000-FFFF per plane */
#define DD2_VRAM_SIZE (4 * DD2_PLANE)

extern uint8_t dd2_vram[DD2_VRAM_SIZE];
extern int dd2_base;                /* [0x2FFE] -- output base within the plane */
extern int dd2_stride;              /* [0x3004] */
extern int dd2_scroll;              /* [0x9060] -- window shift, in pixels */
extern int dd2_ega_plane;
extern int g_fast;

extern int dd2_text_x, dd2_text_y, dd2_text_color;   /* [0x4228],[0x422A],[0x4226] */
extern int dd2_text_left, dd2_text_right;            /* [0x9798],[0x979A] */

const char *Video_DataDir(void);
void  Video_SetDataDir(const char *d);

void  Video_Clear(void);
void  Video_SetWidth(int w);                                  /* 0x7FB2 */
void  Video_Fill(int x, int row, int nbytes, int rows, int v);/* 0x92BC */
void  Video_RowCopy(int nbytes, int rows, int src, int dst);  /* 0x9313 */
void  Video_SetScroll(int px);                                /* 0x90A8 */
void  Video_SetPlane(int plane);                              /* 0x9076 */
void  Video_Tile8(const dd2_assets *A, int tile, int px_x, int px_y);   /* 0x915A */
void  Video_Panel(const dd2_assets *A, int x, int y, int w, int h);      /* 0x7FC9 */
void  Video_CenterPanel(const dd2_assets *A, int w, int h);              /* 0x812F */
void  Video_ShowPicAt(const char *name, int row_off);         /* 0x7C77 */
void  Video_ShowPic(const char *name, int yoff);              /* 0x7C77 (compat.) */
void  Video_TitlePics(void);                                  /* 0x14A0 */
void  Video_Prep(void);                                       /* 0x7938 */
void  Video_TickDelay(void);                                  /* 0x8C70 */

int   Video_StrWidth(const char *s);                          /* 0x8819 */
void  Video_PrintStr(const char *s);                          /* 0x876C */
void  Video_PrintStrC(const char *s);                         /* 0x8851 */
void  Video_PrintChar(int ch);                                /* 0x9347 */

/* Attract mode: 0x6720 (STARPIC + two lines), then 0x14F8 (scroll loop). */
void  Video_AttractBegin(void);
void  Video_TitleBegin(void);
void  Video_AttractFrame(void);        /* one iteration of the 0x14F8 loop */

void  Video_ToIndexed(uint8_t *fb, int w, int h);
void  Video_FromIndexed(const uint8_t *fb, int w, int h);
uint32_t Video_EGAPalette(int idx);
void  Video_TitleToIndexed(uint8_t *fb, int w, int h, int wipe);   /* compat. */

#endif /* DD2_VIDEO_H */
