/*
 * ArgonOS Mega Drive - the sound side on the other core (the CYD); see
 * md_snd_async.c.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef MD_SND_ASYNC_H
#define MD_SND_ASYNC_H

#include <stdint.h>

/* Nonzero while the Z80 and the chips run on the system core. */
extern int md_snd_async;

void md_snd_async_start(int z80_on, int sound_on);
void md_snd_async_stop(void);

/* The 68000's side: what it writes is logged against the current line. */
void         md_snd_68k_ym(unsigned int port, unsigned int value);
unsigned int md_snd_68k_ym_status(void);
void         md_snd_68k_psg(unsigned int value);
void         md_snd_68k_zctl(unsigned int address, unsigned int value);
void         md_snd_68k_frame_begin(int lines, int visible);
void         md_snd_68k_frame_end(int lines, int visible);

/* The system core's side: a frame's log played if one is waiting. */
int      md_snd_core0_poll(void);
uint32_t md_snd_core0_us(void);
uint32_t md_snd_split(void); /* frames handed over in pieces */

/* md_z80.c: a bus request or a reset, applied (the system core's half). */
void md_z80_ctl_apply(unsigned int address, unsigned int value);

#endif /* MD_SND_ASYNC_H */
