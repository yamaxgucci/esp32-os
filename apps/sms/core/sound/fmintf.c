/*
    fmintf.c --
    Interface to YM2413-compatible FM.

    ArgonOS: do not link ym2413.c (needs sinf/logf).  Instead decode the
    OPLL register protocol and drive apps/common/fm/ag_fm (integer lite synth).
*/
#include "shared.h"
#include "ag_fm.h"

static FM_Context fm_context;
static ag_fm_t    s_fm;
static uint16_t   s_fnum[9];
static uint8_t    s_block[9];

static void apply_reg(uint8_t reg, uint8_t data)
{
	int ch;

	if (reg <= 0x07u) {
		fm_context.reg[reg] = data;
		ag_fm_set_patch(&s_fm, &fm_context.reg[0]);
		return;
	}

	if (reg == 0x0eu) {
		fm_context.reg[reg] = data;
		ag_fm_set_rhythm(&s_fm, data);
		return;
	}

	if (reg >= 0x10u && reg <= 0x18u) {
		ch = (int)(reg - 0x10u);
		fm_context.reg[reg] = data;
		s_fnum[ch] = (uint16_t)((s_fnum[ch] & 0x100u) | data);
		ag_fm_set_fnum(&s_fm, ch, s_fnum[ch], s_block[ch]);
		return;
	}

	if (reg >= 0x20u && reg <= 0x28u) {
		ch = (int)(reg - 0x20u);
		fm_context.reg[reg] = data;
		s_fnum[ch] = (uint16_t)(fm_context.reg[0x10u + ch] | ((uint16_t)(data & 1u) << 8));
		s_block[ch] = (uint8_t)((data >> 1) & 7u);
		ag_fm_set_fnum(&s_fm, ch, s_fnum[ch], s_block[ch]);
		ag_fm_set_key(&s_fm, ch, (data & 0x10u) != 0, (data & 0x20u) != 0);
		return;
	}

	if (reg >= 0x30u && reg <= 0x38u) {
		ch = (int)(reg - 0x30u);
		fm_context.reg[reg] = data;
		ag_fm_set_inst_vol(&s_fm, ch, (uint8_t)((data >> 4) & 0x0fu),
		                   (uint8_t)(data & 0x0fu));
		return;
	}

	if (reg < 0x40u) {
		fm_context.reg[reg] = data;
	}
}

void FM_Init(void)
{
	int i;
	ag_fm_init(&s_fm, (uint32_t)snd.fm_clock, (uint32_t)snd.sample_rate);
	fm_context.latch = 0;
	for (i = 0; i < 0x40; i++) {
		fm_context.reg[i] = 0;
	}
	for (i = 0; i < 9; i++) {
		s_fnum[i] = 0;
		s_block[i] = 0;
	}
}

void FM_Shutdown(void) {}

void FM_Reset(void)
{
	FM_Init();
}

void FM_Update(int16_t **buffer, int32_t length)
{
	if (buffer == NULL || buffer[0] == NULL || buffer[1] == NULL || length <= 0) {
		return;
	}
	ag_fm_update(&s_fm, buffer[0], buffer[1], length);
}

void FM_WriteReg(uint8_t reg, uint8_t data)
{
	FM_Write(0, reg);
	FM_Write(1, data);
}

void FM_Write(uint32_t offset, uint8_t data)
{
	if (offset & 1u) {
		apply_reg(fm_context.latch, data);
	} else {
		fm_context.latch = data;
	}
}

void FM_GetContext(uint8_t *data)
{
	memcpy(data, &fm_context, sizeof(FM_Context));
}

void FM_SetContext(uint8_t *data)
{
	int i;
	memcpy(&fm_context, data, sizeof(FM_Context));
	ag_fm_set_patch(&s_fm, &fm_context.reg[0]);
	ag_fm_set_rhythm(&s_fm, fm_context.reg[0x0e]);
	for (i = 0; i < 9; i++) {
		uint8_t r20 = fm_context.reg[0x20 + i];
		s_fnum[i] = (uint16_t)(fm_context.reg[0x10 + i] | ((uint16_t)(r20 & 1u) << 8));
		s_block[i] = (uint8_t)((r20 >> 1) & 7u);
		ag_fm_set_fnum(&s_fm, i, s_fnum[i], s_block[i]);
		ag_fm_set_key(&s_fm, i, (r20 & 0x10u) != 0, (r20 & 0x20u) != 0);
		ag_fm_set_inst_vol(&s_fm, i,
		                   (uint8_t)((fm_context.reg[0x30 + i] >> 4) & 0x0fu),
		                   (uint8_t)(fm_context.reg[0x30 + i] & 0x0fu));
	}
}

uint32_t FM_GetContextSize(void)
{
	return sizeof(FM_Context);
}

uint8_t *FM_GetContextPtr(void)
{
	return (uint8_t *)&fm_context;
}

uint32_t YM2413_GetContextSize(void)
{
	return 0;
}

uint8_t *YM2413_GetContextPtr(void)
{
	return NULL;
}
