/*
 * ArgonOS Mega Drive — YM2612 register front-end into ag_fm.
 *
 * Not chip-accurate: six one-operator voices + DAC PCM.  Timbre will be
 * recognizably wrong vs a real YM2612; games that only need the Z80 driver
 * and basic tones still get sound.  Native ym2612.c is not linked.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#include "ag_fm.h"
#include "md_sound.h"
#include "ym2612.h"

int16_t gwenesis_ym2612_buffer[1];
int     ym2612_index;
int     ym2612_clock;

static ag_fm_t  s_fm;
static uint8_t  s_addr[2];
static uint8_t  s_regs[2][0x100];
static uint16_t s_fnum[6];
static uint8_t  s_block[6];
static uint8_t  s_tl[6];
static int      s_dac_en;
static int16_t  s_dac;
static int      s_clock;

static void apply_fnum(int ch)
{
    if (ch < 0 || ch >= 6) {
        return;
    }
    /* OPN fnum is 11-bit; ag_fm expects OPLL-ish 9-bit — keep low 9. */
    ag_fm_set_fnum(&s_fm, ch, (uint16_t)(s_fnum[ch] & 0x1ffu), s_block[ch]);
}

static void apply_vol(int ch)
{
    uint8_t vol;
    if (ch < 0 || ch >= 6) {
        return;
    }
    vol = (uint8_t)(s_tl[ch] >> 3);
    if (vol > 15u) {
        vol = 15u;
    }
    ag_fm_set_inst_vol(&s_fm, ch, 1u, vol);
}

static void write_reg(int part, uint8_t reg, uint8_t data)
{
    int ch;

    s_regs[part][reg] = data;

    if (part == 0 && reg == 0x28u) {
        /* Key on/off: bits 0-2 select 0,1,2,4,5,6 → channels 0..5. */
        static const int8_t k_ch[8] = {0, 1, 2, -1, 3, 4, 5, -1};
        ch = k_ch[data & 7u];
        if (ch < 0) {
            return;
        }
        ag_fm_set_key(&s_fm, ch, (data & 0xf0u) != 0, 0);
        return;
    }

    if (part == 0 && reg == 0x2au) {
        s_dac = (int16_t)((int8_t)data) << 6;
        return;
    }
    if (part == 0 && reg == 0x2bu) {
        s_dac_en = (data & 0x80u) != 0;
        return;
    }

    if (reg >= 0xa0u && reg <= 0xa2u) {
        ch = part * 3 + (int)(reg - 0xa0u);
        s_fnum[ch] = (uint16_t)((s_fnum[ch] & 0x700u) | data);
        apply_fnum(ch);
        return;
    }
    if (reg >= 0xa4u && reg <= 0xa6u) {
        ch = part * 3 + (int)(reg - 0xa4u);
        s_fnum[ch] = (uint16_t)((s_fnum[ch] & 0xffu) |
                                (((uint16_t)data & 7u) << 8));
        s_block[ch] = (uint8_t)((data >> 3) & 7u);
        apply_fnum(ch);
        return;
    }

    /* TL on operator 1 of each channel (regs 0x40,0x44,0x48). */
    if (reg >= 0x40u && reg <= 0x4eu && ((reg & 3u) == 0u)) {
        ch = part * 3 + (int)((reg - 0x40u) / 4u);
        if (ch >= 0 && ch < 6) {
            s_tl[ch] = (uint8_t)(data & 0x7fu);
            apply_vol(ch);
        }
        return;
    }
}

void YM2612Init(void)
{
    int i;
    ag_fm_init(&s_fm, 7670453u, MD_SOUND_RATE); /* NTSC YM clock ≈ 7.67 MHz */
    for (i = 0; i < 6; i++) {
        s_fnum[i] = 0;
        s_block[i] = 0;
        s_tl[i] = 0x7f;
        apply_vol(i);
    }
    s_dac_en = 0;
    s_dac = 0;
    s_addr[0] = s_addr[1] = 0;
    ym2612_index = 0;
    ym2612_clock = 0;
    s_clock = 0;
}

void YM2612Config(unsigned char dac_bits)
{
    (void)dac_bits;
}

void YM2612ResetChip(void)
{
    YM2612Init();
}

void YM2612Write(unsigned int a, unsigned int v, int target)
{
    int part = (a & 2u) ? 1 : 0;
    (void)target;
    if ((a & 1u) == 0u) {
        s_addr[part] = (uint8_t)v;
    } else {
        write_reg(part, s_addr[part], (uint8_t)v);
    }
}

unsigned int YM2612Read(int target)
{
    (void)target;
    /* Busy clear, timers clear — enough for drivers that poll status. */
    return 0u;
}

void ym2612_run(int target)
{
    s_clock = target;
    ym2612_clock = target;
}

void md_ym_render(int16_t *left, int16_t *right, int samples)
{
    int i;
    if (samples <= 0) {
        return;
    }
    ag_fm_update(&s_fm, left, right, samples);
    if (s_dac_en) {
        for (i = 0; i < samples; i++) {
            int32_t l = (int32_t)left[i] + s_dac;
            int32_t r = (int32_t)right[i] + s_dac;
            if (l > 32767) {
                l = 32767;
            }
            if (l < -32768) {
                l = -32768;
            }
            if (r > 32767) {
                r = 32767;
            }
            if (r < -32768) {
                r = -32768;
            }
            left[i] = (int16_t)l;
            right[i] = (int16_t)r;
        }
    }
}

void gwenesis_ym2612_save_state(void) {}
void gwenesis_ym2612_load_state(void) {}
