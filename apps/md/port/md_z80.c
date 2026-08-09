/*
 * ArgonOS Mega Drive — gwenesis z80inst.h over the SMS (MAME) Z80 core.
 *
 * BUSREQ / RESET / bank window / YM / PSG bridges match upstream gwenesis
 * z80inst.c semantics; the CPU itself is apps/sms/core/z80 (BSD-3), not
 * Fayzullin.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#include "gwenesis_bus.h"
#include "m68k.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"
#include "z80inst.h"

/*
 * z80.h also declares z80_execute, but for SMS that means relative cycles.
 * Rename the include declaration to match the symbol z80.c exports under
 * ARGON_MD_Z80, then restore the gwenesis absolute-target name.
 */
#define z80_execute sms_z80_execute
#include "z80.h"
#undef z80_execute

int zclk;

static unsigned char *s_zram;
static int            s_bus_ack;
static int            s_reset;
static int            s_reset_once;
static int            s_z80_bank; /* 9-bit latch; window at <<15 */
static int            s_timeslice;

uint8_t argon_z80_read(uint16_t address);
void    argon_z80_write(uint16_t address, uint8_t data);

static int32_t md_z80_irq_cb(int32_t param)
{
    (void)param;
    return 0xff;
}

static uint8_t md_z80_in(uint16_t port)
{
    (void)port;
    return 0xffu;
}

static void md_z80_out(uint16_t port, uint8_t data)
{
    (void)port;
    (void)data;
}

static void zbank_shift(uint8_t value)
{
    s_z80_bank = (s_z80_bank >> 1) | (((int)value & 1) << 8);
}

static unsigned int zbank_addr(unsigned int address)
{
    return (address & 0x7fffu) | ((unsigned int)s_z80_bank << 15);
}

uint8_t argon_z80_read(uint16_t address)
{
    if (address < 0x4000u) {
        return (s_zram != 0) ? s_zram[address & 0x1fffu] : 0xffu;
    }
    if (address < 0x6000u) {
        int tgt = zclk + s_timeslice;
        return (uint8_t)YM2612Read(tgt);
    }
    if (address >= 0x8000u) {
        return (uint8_t)m68k_read_memory_8(zbank_addr(address));
    }
    return 0xffu;
}

void argon_z80_write(uint16_t address, uint8_t data)
{
    if (address < 0x4000u) {
        if (s_zram != 0) {
            s_zram[address & 0x1fffu] = data;
        }
        return;
    }
    if (address < 0x6000u) {
        int tgt = zclk + s_timeslice;
        YM2612Write((unsigned int)address & 3u, data, tgt);
        return;
    }
    if (address == 0x6000u) {
        zbank_shift(data);
        return;
    }
    if (address == 0x7f11u) {
        int tgt = zclk + s_timeslice;
        gwenesis_SN76489_Write((int)data, tgt);
        return;
    }
    if (address >= 0x8000u) {
        m68k_write_memory_8(zbank_addr(address), data);
    }
}

void z80_set_memory(unsigned char *buffer)
{
    s_zram = buffer;
}

void z80_start(void)
{
    static int once;
    if (!once) {
        z80_init(md_z80_irq_cb);
        cpu_writemem16 = argon_z80_write;
        cpu_writeport16 = md_z80_out;
        cpu_readport16 = md_z80_in;
        once = 1;
    }
    z80_reset();
    s_reset = 1;
    s_reset_once = 0;
    s_bus_ack = 0;
    s_z80_bank = 0;
    zclk = 0;
    s_timeslice = 0;
}

void z80_pulse_reset(void)
{
    z80_reset();
}

void z80_run(int target)
{
    int rem = 0;

    s_timeslice = 0;
    if (zclk >= target) {
        return;
    }
    s_timeslice = target - zclk;

    if (s_reset_once && !s_bus_ack && !s_reset) {
        int cycles = s_timeslice / Z80_FREQ_DIVISOR;
        if (cycles > 0) {
            rem = cycles - (int)sms_z80_execute((int32_t)cycles);
            if (rem < 0) {
                rem = 0;
            }
        }
    }
    zclk = target - rem * Z80_FREQ_DIVISOR;
}

void z80_execute(unsigned int target)
{
    z80_run((int)target);
}

static void z80_sync(void)
{
    z80_run(m68k_cycles_master());
}

void z80_write_ctrl(unsigned int address, unsigned int value)
{
    z80_sync();
    if (address == 0x1100u) {
        s_bus_ack = value ? 1 : 0;
    } else if (address == 0x1200u) {
        if (value == 0u) {
            s_reset = 1;
        } else {
            z80_pulse_reset();
            s_reset = 0;
            s_reset_once = 1;
        }
    }
}

unsigned int z80_read_ctrl(unsigned int address)
{
    z80_sync();
    if (address == 0x1100u) {
        /* Bit0 set = Z80 still owns the bus (BUSREQ not granted). */
        return s_bus_ack ? 0u : 1u;
    }
    if (address == 0x1200u) {
        return (unsigned int)s_reset;
    }
    return 0u;
}

void z80_irq_line(unsigned int value)
{
    if (!s_reset_once) {
        return;
    }
    z80_set_irq_line(0, value ? ASSERT_LINE : CLEAR_LINE);
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
    z80_write_memory_8(address + 1u, value & 0xffu);
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
