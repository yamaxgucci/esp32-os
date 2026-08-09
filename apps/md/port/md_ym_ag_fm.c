/*
 * ArgonOS Mega Drive — YM2612 register front-end into ag_fm (4-op OPN).
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
static uint8_t  s_alg[6];
static uint8_t  s_fb[6];
/* Per channel, 4 operators in YM SLOT order: 0=M1, 1=M2, 2=C1, 3=C2. */
static uint8_t  s_mul[6][4];
static uint8_t  s_tl[6][4];
static uint8_t  s_ar[6][4];
static uint8_t  s_dr[6][4];
static uint8_t  s_sl[6][4];
static uint8_t  s_rr[6][4];
static int      s_dac_en;
static int16_t  s_dac;
static int      s_clock;

/* OPN MUL nibble → ag_fm x2 units (0 → 0.5). */
static uint16_t mul_x2(uint8_t opn_mul)
{
    opn_mul &= 0x0fu;
    if (opn_mul == 0u) {
        return 1u;
    }
    return (uint16_t)opn_mul * 2u;
}

static void apply_voice(int ch)
{
    int s;

    if (ch < 0 || ch >= 6) {
        return;
    }
    ag_fm_set_fnum(&s_fm, ch, s_fnum[ch], s_block[ch]);
    for (s = 0; s < 4; s++) {
        ag_fm_set_opn_op(&s_fm, ch, s, mul_x2(s_mul[ch][s]), s_tl[ch][s],
                         s_ar[ch][s], s_dr[ch][s], s_sl[ch][s], s_rr[ch][s]);
    }
    ag_fm_set_alg(&s_fm, ch, s_alg[ch], s_fb[ch]);
}

static int ch_from_op_reg(int part, uint8_t reg)
{
    return part * 3 + (int)(reg & 3u);
}

static int slot_from_op_reg(uint8_t reg)
{
    /* Matches ym2612 SLOT[] indices via OPN_SLOT. */
    return (int)((reg >> 2) & 3u);
}

static void write_reg(int part, uint8_t reg, uint8_t data)
{
    int ch;
    int slot;

    s_regs[part][reg] = data;

    if (part == 0 && reg == 0x28u) {
        static const int8_t k_ch[8] = {0, 1, 2, -1, 3, 4, 5, -1};
        ch = k_ch[data & 7u];
        if (ch < 0) {
            return;
        }
        /* YM: bit4=SLOT1, bit5=SLOT2, bit6=SLOT3, bit7=SLOT4 → op_mask order. */
        ag_fm_set_opn_key(&s_fm, ch, (uint8_t)((data >> 4) & 0x0fu));
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
        apply_voice(ch);
        return;
    }
    if (reg >= 0xa4u && reg <= 0xa6u) {
        ch = part * 3 + (int)(reg - 0xa4u);
        s_fnum[ch] = (uint16_t)((s_fnum[ch] & 0xffu) |
                                (((uint16_t)data & 7u) << 8));
        s_block[ch] = (uint8_t)((data >> 3) & 7u);
        apply_voice(ch);
        return;
    }

    if (reg >= 0xb0u && reg <= 0xb2u) {
        ch = part * 3 + (int)(reg - 0xb0u);
        s_alg[ch] = (uint8_t)(data & 7u);
        s_fb[ch] = (uint8_t)((data >> 3) & 7u);
        apply_voice(ch);
        return;
    }

    if (reg >= 0x30u && reg <= 0x3eu) {
        ch = ch_from_op_reg(part, reg);
        slot = slot_from_op_reg(reg);
        if (ch >= 0 && ch < 6) {
            s_mul[ch][slot] = (uint8_t)(data & 0x0fu);
            apply_voice(ch);
        }
        return;
    }
    if (reg >= 0x40u && reg <= 0x4eu) {
        ch = ch_from_op_reg(part, reg);
        slot = slot_from_op_reg(reg);
        if (ch >= 0 && ch < 6) {
            s_tl[ch][slot] = (uint8_t)(data & 0x7fu);
            apply_voice(ch);
        }
        return;
    }
    if (reg >= 0x50u && reg <= 0x5eu) {
        ch = ch_from_op_reg(part, reg);
        slot = slot_from_op_reg(reg);
        if (ch >= 0 && ch < 6) {
            s_ar[ch][slot] = (uint8_t)(data & 0x1fu);
            apply_voice(ch);
        }
        return;
    }
    if (reg >= 0x60u && reg <= 0x6eu) {
        ch = ch_from_op_reg(part, reg);
        slot = slot_from_op_reg(reg);
        if (ch >= 0 && ch < 6) {
            s_dr[ch][slot] = (uint8_t)(data & 0x1fu);
            apply_voice(ch);
        }
        return;
    }
    if (reg >= 0x80u && reg <= 0x8eu) {
        ch = ch_from_op_reg(part, reg);
        slot = slot_from_op_reg(reg);
        if (ch >= 0 && ch < 6) {
            s_sl[ch][slot] = (uint8_t)((data >> 4) & 0x0fu);
            s_rr[ch][slot] = (uint8_t)(data & 0x0fu);
            apply_voice(ch);
        }
        return;
    }
}

void YM2612Init(void)
{
    int i, s;
    ag_fm_init(&s_fm, 7670453u, MD_SOUND_RATE);
    ag_fm_set_clk_div(&s_fm, AG_FM_CLKDIV_OPN);
    for (i = 0; i < 6; i++) {
        s_fnum[i] = 0;
        s_block[i] = 0;
        s_alg[i] = 0;
        s_fb[i] = 0;
        for (s = 0; s < 4; s++) {
            s_mul[i][s] = 1;
            s_tl[i][s] = 0x7f;
            s_ar[i][s] = 31;
            s_dr[i][s] = 0;
            s_sl[i][s] = 0;
            s_rr[i][s] = 15;
        }
        apply_voice(i);
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
    return 0u;
}

void ym2612_run(int target)
{
    s_clock = target;
    ym2612_clock = target;
}

void md_ym_frame_begin(int lines_per_frame)
{
    (void)lines_per_frame;
    ym2612_index = 0;
    ym2612_clock = 0;
    s_clock = 0;
}

void md_ym_render(int16_t *left, int16_t *right, int samples)
{
    int i;
    if (samples <= 0) {
        return;
    }
    ag_fm_update(&s_fm, left, right, samples);
    /* Match ~14-bit channel headroom; pad so six voices + PSG rarely crush. */
    for (i = 0; i < samples; i++) {
        int32_t l = (int32_t)left[i] >> 1;
        int32_t r = (int32_t)right[i] >> 1;
        if (s_dac_en) {
            l += (int32_t)s_dac >> 1;
            r += (int32_t)s_dac >> 1;
        }
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

void gwenesis_ym2612_save_state(void) {}
void gwenesis_ym2612_load_state(void) {}
