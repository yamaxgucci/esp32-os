/*
 * ArgonOS - the silent half of the Mega Drive.
 *
 * A Mega Drive has a whole second computer for sound: a Z80 with its own 8 KB of
 * RAM driving a YM2612 and a PSG.  This milestone runs the 68000 and the VDP
 * only, so all of that is stubbed here and none of it is compiled in.  That is
 * not just about speed - it also keeps two files out of the image whose licences
 * we do not want (see README, "Licensing").
 *
 * What matters for correctness is that a game must not hang waiting for hardware
 * that is not there:
 *
 *  - reading the Z80 bus-request port has to report the bus as granted, or the
 *    68000 spins in "wait until the Z80 lets go" forever;
 *  - reading the YM2612 status has to report "not busy" for the same reason;
 *  - zclk has to keep advancing to the clock target the frame loop asks for,
 *    because it is compared against the master clock elsewhere.
 *
 * Games will still write sound registers into the void, which is silent but
 * harmless.  When the sound block lands it replaces this file, keeps the same
 * signatures, and runs on the other core.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#include "gwenesis_bus.h"
#include "gwenesis_savestate.h"
#include "gwenesis_sn76489.h"
#include "ym2612.h"
#include "z80inst.h"

/* The Z80's view of the clock.  The frame loop hands us targets; we just arrive. */
int zclk = 0;

static unsigned char *s_zram;

void z80_set_memory(unsigned char *buffer) { s_zram = buffer; }

void z80_start(void) { zclk = 0; }
void z80_pulse_reset(void) { zclk = 0; }

void z80_execute(unsigned int target) { zclk = (int)target; }
void z80_run(int target)
{
    if (target > zclk) {
        zclk = target;
    }
}

void z80_irq_line(unsigned int value) { (void)value; }

void z80_write_ctrl(unsigned int address, unsigned int value)
{
    (void)address;
    (void)value;
}

/*
 * 0xA11100 is BUSREQ: bit 0 set means "the Z80 still owns the bus".  With no Z80
 * at all the honest answer is "the bus is yours", and it is also the only answer
 * that lets a game get past its startup handshake.
 */
unsigned int z80_read_ctrl(unsigned int address)
{
    (void)address;
    return 0u;
}

void z80_write_memory_8(unsigned int address, unsigned int value)
{
    if (s_zram != 0) {
        s_zram[address & (MAX_Z80_RAM_SIZE - 1u)] = (unsigned char)value;
    }
}

void z80_write_memory_16(unsigned int address, unsigned int value)
{
    z80_write_memory_8(address, value >> 8);
    z80_write_memory_8(address + 1u, value & 0xFFu);
}

unsigned int z80_read_memory_8(unsigned int address)
{
    return (s_zram != 0) ? s_zram[address & (MAX_Z80_RAM_SIZE - 1u)] : 0u;
}

unsigned int z80_read_memory_16(unsigned int address)
{
    return (z80_read_memory_8(address) << 8) | z80_read_memory_8(address + 1u);
}

void gwenesis_z80inst_save_state(void) {}
void gwenesis_z80inst_load_state(void) {}

/* ---- YM2612 ------------------------------------------------------------- */

void YM2612Init(void) {}
void YM2612Config(unsigned char dac_bits) { (void)dac_bits; }
void YM2612ResetChip(void) {}

void YM2612Write(unsigned int a, unsigned int v, int target)
{
    (void)a;
    (void)v;
    (void)target;
}

/* Status register: bit 7 is "busy", bits 0-1 are the timers.  Zero says idle. */
unsigned int YM2612Read(int target)
{
    (void)target;
    return 0u;
}

void ym2612_run(int target) { (void)target; }

int YM2612LoadContext(unsigned char *state)
{
    (void)state;
    return 0;
}

int YM2612SaveContext(unsigned char *state)
{
    (void)state;
    return 0;
}

void gwenesis_ym2612_save_state(void) {}
void gwenesis_ym2612_load_state(void) {}

/* ---- SN76489 (PSG) ------------------------------------------------------ */

void gwenesis_SN76489_Init(int clock, int rate, int divisor)
{
    (void)clock;
    (void)rate;
    (void)divisor;
}

void gwenesis_SN76489_Reset(void) {}
void gwenesis_SN76489_start(void) {}

void gwenesis_SN76489_Write(int data, int target)
{
    (void)data;
    (void)target;
}

void gwenesis_SN76489_run(int target) { (void)target; }

void gwenesis_SN76489_SetContext(uint8 *data) { (void)data; }
void gwenesis_SN76489_GetContext(uint8 *data) { (void)data; }
uint8 *gwenesis_SN76489_GetContextPtr(void) { return 0; }
int gwenesis_SN76489_GetContextSize(void) { return 0; }

void gwenesis_sn76489_save_state(void) {}
void gwenesis_sn76489_load_state(void) {}

/* ---- Save states -------------------------------------------------------- */

/*
 * Every core in gwenesis knows how to serialise itself, but where the bytes go
 * is the platform's business, and this milestone has nowhere to put them yet.
 * Opening always fails, which makes every save and load a no-op rather than a
 * crash: the cores check the handle before writing through it.
 */
SaveState *saveGwenesisStateOpenForRead(const char *fileName)
{
    (void)fileName;
    return 0;
}

SaveState *saveGwenesisStateOpenForWrite(const char *fileName)
{
    (void)fileName;
    return 0;
}

int saveGwenesisStateGet(SaveState *state, const char *tagName)
{
    (void)state;
    (void)tagName;
    return 0;
}

void saveGwenesisStateSet(SaveState *state, const char *tagName, int value)
{
    (void)state;
    (void)tagName;
    (void)value;
}

void saveGwenesisStateGetBuffer(SaveState *state, const char *tagName,
                                void *buffer, int length)
{
    (void)state;
    (void)tagName;
    (void)buffer;
    (void)length;
}

void saveGwenesisStateSetBuffer(SaveState *state, const char *tagName,
                                void *buffer, int length)
{
    (void)state;
    (void)tagName;
    (void)buffer;
    (void)length;
}
