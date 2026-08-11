/*
 * Bitmap skin profiles (VGA 640x400 / QVGA 320x240) for AMP.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AMP_SKIN_H
#define AMP_SKIN_H

#include <stdint.h>

typedef enum amp_panel {
    AMP_PANEL_MAIN = 0,
    AMP_PANEL_EQ,
    AMP_PANEL_PL,
    AMP_PANEL_N
} amp_panel_t;

typedef enum amp_ctrl {
    AMP_CTRL_NONE = 0,
    AMP_CTRL_PREV,
    AMP_CTRL_PLAY,
    AMP_CTRL_PAUSE,
    AMP_CTRL_STOP,
    AMP_CTRL_NEXT,
    AMP_CTRL_EJECT,
    AMP_CTRL_VOL,
    AMP_CTRL_BAL,
    AMP_CTRL_SEEK,
    AMP_CTRL_EQ_TOGGLE,
    AMP_CTRL_PL_TOGGLE,
    AMP_CTRL_REPEAT,
    AMP_CTRL_SHUFFLE,
    AMP_CTRL_EQ_ON,
    AMP_CTRL_EQ_AUTO,
    AMP_CTRL_EQ_PREAMP,
    AMP_CTRL_EQ_BAND0,
    AMP_CTRL_EQ_BAND1,
    AMP_CTRL_EQ_BAND2,
    AMP_CTRL_EQ_BAND3,
    AMP_CTRL_EQ_BAND4,
    AMP_CTRL_EQ_BAND5,
    AMP_CTRL_EQ_BAND6,
    AMP_CTRL_EQ_BAND7,
    AMP_CTRL_EQ_BAND8,
    AMP_CTRL_EQ_BAND9,
    AMP_CTRL_PL_LIST,
    AMP_CTRL_N
} amp_ctrl_t;

typedef struct amp_rect {
    int16_t  x, y;
    uint16_t w, h;
} amp_rect_t;

typedef struct amp_skin_panel {
    amp_rect_t     pos;      /* on-screen origin + size */
    uint16_t      *pixels;   /* RGB565 w*h */
    uint16_t       w, h;
} amp_skin_panel_t;

typedef struct amp_skin {
    int             qvga; /* 1 = one panel at a time */
    amp_skin_panel_t panel[AMP_PANEL_N];
    amp_rect_t      ctrl[AMP_CTRL_N]; /* relative to owning panel */
    amp_panel_t     ctrl_panel[AMP_CTRL_N];
    amp_rect_t      ticker;   /* relative to main */
    amp_rect_t      spectrum; /* relative to main */
    amp_rect_t      timebox;  /* relative to main */
    int             pl_row_h;
} amp_skin_t;

/* Build skin bitmaps into heap; returns 0 on success. */
int  amp_skin_load(amp_skin_t *sk, uint16_t fb_w, uint16_t fb_h);
void amp_skin_free(amp_skin_t *sk);

/* Hit-test screen coords → control (or NONE). */
amp_ctrl_t amp_skin_hit(const amp_skin_t *sk, amp_panel_t focus, int x, int y,
                        amp_panel_t *out_panel);

#endif /* AMP_SKIN_H */
