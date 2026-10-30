/*
 * ArgonOS Mega Drive — native YM2612 sample drain for A/B vs ag_fm.
 *
 * Links against core/sound/ym2612.c.  Chip writes fill gwenesis_ym2612_buffer;
 * md_ym_render copies into the stereo mix buffers.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#include "gwenesis_bus.h"
#include "md_sound.h"
#include "ym2612.h"

int16_t gwenesis_ym2612_buffer[GWENESIS_YM2612_BUF_LEN];
int     ym2612_index;
int     ym2612_clock;
int     ym2612_base; /* the frame's sample at gwenesis_ym2612_buffer[0] */

static int s_read;

void md_ym_frame_begin(int lines_per_frame)
{
    int lines = lines_per_frame > 0 ? lines_per_frame : 262;
    int rate = lines >= 300 ? 50 : 60;
    int samples = (int)(md_sound_synth_rate() / (unsigned)rate);
    int clocks = lines * VDP_CYCLES_PER_LINE;

    if (samples > 0) {
        YM2612SetSampleDivisor(clocks / samples);
    }
    ym2612_index = 0;
    ym2612_clock = 0;
    ym2612_base = 0;
    s_read = 0;
}

void md_ym_render(int16_t *left, int16_t *right, int samples)
{
    int i;

    if (samples <= 0) {
        return;
    }
    for (i = 0; i < samples; i++) {
        int32_t s = 0;
        if (s_read < ym2612_index) {
            s = (int32_t)gwenesis_ym2612_buffer[s_read++ - ym2612_base];
        }
        left[i] = (int16_t)s;
        if (right != left) {
            right[i] = (int16_t)s;
        }
    }
    /* all of it taken: the next line's samples go in from the start */
    if (s_read == ym2612_index) {
        ym2612_base = ym2612_index;
    }
}

void gwenesis_ym2612_save_state(void) {}
void gwenesis_ym2612_load_state(void) {}
