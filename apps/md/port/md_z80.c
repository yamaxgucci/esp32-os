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
#include "md_snd_async.h"

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

#ifdef MD_Z80PROF
static inline uint32_t zp_cc(void)
{
    uint32_t c;
    __asm__ volatile("rsr %0, ccount" : "=r"(c));
    return c;
}
uint32_t md_zp_ym_cyc, md_zp_rom_cyc, md_zp_ym_n, md_zp_rom_n;
uint32_t md_zp_fast_n, md_zp_slow_n; /* instructions, by which loop ran them */
#endif

#ifdef ARGON_SMS_SMALL
int  md_z80_slice_done(void); /* z80.c */
void md_z80_spin_until(int tstates);
/*
 * The Z80's own time, for what it writes to the chips and when it reads
 * them: its slice's start and the T-states run so far.  The end of the slice
 * stood for all of them, which put every DAC sample of a line at the line's
 * end - and the system core now runs the Z80 several lines a slice
 * (md_snd_async.c), where that would have put a drum's samples on top of each
 * other.
 */
static inline int md_z80_now(void)
{
    return zclk + md_z80_slice_done() * Z80_FREQ_DIVISOR;
}
#else
static inline int md_z80_now(void)
{
    return zclk + s_timeslice;
}
#endif

uint8_t argon_z80_read(uint16_t address)
{
    if (address < 0x4000u) {
        return (s_zram != 0) ? MD_ZRAM(address) : 0xffu;
    }
    if (address < 0x6000u) {
        int tgt = md_z80_now();
        const uint8_t v = (uint8_t)YM2612Read(tgt);
#ifdef ARGON_SMS_SMALL
        /* its status is time's: the same until a timer's next overflow */
        const int until = ym2612_status_until();
        md_z80_spin_until((until == 0x7fffffff) ? until : (until - tgt) / Z80_FREQ_DIVISOR);
#endif
        return v;
    }
    if (address >= 0x8000u) {
#ifdef MD_Z80PROF
        const uint32_t c0 = zp_cc();
        const uint8_t  v = (uint8_t)m68k_read_memory_8(zbank_addr(address));
        md_zp_rom_cyc += zp_cc() - c0;
        md_zp_rom_n++;
        return v;
#else
        return (uint8_t)m68k_read_memory_8(zbank_addr(address));
#endif
    }
    return 0xffu;
}

/* in the hot part: a drum is the Z80 writing the YM through here (md_z80_fast) */
MD_HOT void argon_z80_write(uint16_t address, uint8_t data)
{
    if (address < 0x4000u) {
        if (s_zram != 0) {
            MD_ZRAM(address) = data;
        }
        return;
    }
    if (address < 0x6000u) {
        int tgt = md_z80_now();
#ifdef MD_HOST_YMHASH
        {
            extern uint32_t md_host_ymhash;
            md_host_ymhash = md_host_ymhash * 31u + ((address & 3u) << 8 | data) * 7u + (uint32_t)tgt;
        }
#endif
#ifdef MD_Z80PROF
        const uint32_t c0 = zp_cc();
        YM2612Write((unsigned int)address & 3u, data, tgt);
        md_zp_ym_cyc += zp_cc() - c0;
        md_zp_ym_n++;
#else
        YM2612Write((unsigned int)address & 3u, data, tgt);
#endif
        return;
    }
    if (address == 0x6000u) {
        zbank_shift(data);
        return;
    }
    if (address == 0x7f11u) {
        int tgt = md_z80_now();
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

#ifdef MD_Z80PROF
#include <argon/argon.h>
/* -DMD_Z80PROF: where the Z80 spends its time - its PC at the start of each
 * line's slice, the commonest eight in `profile`, with the code around them */
static uint16_t s_zp_pc[24];
static uint32_t s_zp_n[24];
void md_z80_prof_report(void)
{
    ag_printf("md: z80 ym writes %u (%u us), rom reads %u (%u us)\n",
              (unsigned)md_zp_ym_n, (unsigned)(md_zp_ym_cyc / 240u),
              (unsigned)md_zp_rom_n, (unsigned)(md_zp_rom_cyc / 240u));
    ag_printf("md: z80 instructions %u from the hot part, %u from flash\n",
              (unsigned)md_zp_fast_n, (unsigned)md_zp_slow_n);
    md_zp_fast_n = md_zp_slow_n = 0;
    md_zp_ym_cyc = md_zp_rom_cyc = md_zp_ym_n = md_zp_rom_n = 0;
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < 24; i++) {
            if (s_zp_n[i] && (best < 0 || s_zp_n[i] > s_zp_n[best])) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        const uint16_t pc = s_zp_pc[best];
        ag_printf("md: z80 pc %04x x%u:", (unsigned)pc, (unsigned)s_zp_n[best]);
        for (int b = -6; b < 10; b++) {
            ag_printf(" %02x", (unsigned)argon_z80_read((uint16_t)(pc + b)));
        }
        ag_printf("\n");
        s_zp_n[best] = 0;
    }
    for (int i = 0; i < 24; i++) {
        s_zp_n[i] = 0;
    }
}
static void zp_sample(void)
{
    const uint16_t pc = (uint16_t)(Z80.pc.d & 0xffffu);
    int free_i = -1;
    for (int i = 0; i < 24; i++) {
        if (s_zp_n[i] && s_zp_pc[i] == pc) {
            s_zp_n[i]++;
            return;
        }
        if (!s_zp_n[i] && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i >= 0) {
        s_zp_pc[free_i] = pc;
        s_zp_n[free_i] = 1;
    }
}
#endif

#ifdef ARGON_SMS_SMALL
int  md_z80_idle_pass(void);    /* z80.c */
void md_z80_idle_advance(int n);
#endif

void z80_run(int target)
{
    int rem = 0;
#ifdef MD_Z80PROF
    zp_sample();
#endif

    s_timeslice = 0;
    if (zclk >= target) {
        return;
    }
    s_timeslice = target - zclk;

    if (s_reset_once && !s_bus_ack && !s_reset) {
        int cycles = s_timeslice / Z80_FREQ_DIVISOR;
#ifdef ARGON_SMS_SMALL
        /*
         * A slice it can only wait through: the whole passes counted out
         * (z80.c), and the last one - a few instructions - run, so the slice
         * ends where the interpreter alone would have ended it.
         */
        const int idle = (cycles > 0) ? md_z80_idle_pass() : 0;
        if (idle > 0 && cycles > idle) {
            const int n = (cycles - 1) / idle;
            md_z80_idle_advance(n);
            cycles -= n * idle;
        }
#endif
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

/* The 68000's view of the bus request and the reset while the Z80 runs on
 * the other core (md_snd_async.c): what it last wrote, which is what it is
 * answered - the Z80 a frame behind gives the bus up at once. */
static int s_bus_ack68;
static int s_reset68 = 1;

void md_z80_ctl_apply(unsigned int address, unsigned int value)
{
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

void z80_write_ctrl(unsigned int address, unsigned int value)
{
    if (md_snd_async) {
        if (address == 0x1100u) {
            s_bus_ack68 = value ? 1 : 0;
        } else if (address == 0x1200u) {
            s_reset68 = (value == 0u) ? 1 : 0;
        }
        md_snd_68k_zctl(address, value);
        return;
    }
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
    if (md_snd_async) {
        if (address == 0x1100u) {
            return s_bus_ack68 ? 0u : 1u;
        }
        return (address == 0x1200u) ? (unsigned int)s_reset68 : 0u;
    }
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
        MD_ZRAM(address) = (unsigned char)value;
    }
}

void z80_write_memory_16(unsigned int address, unsigned int value)
{
    z80_write_memory_8(address, value >> 8);
    z80_write_memory_8(address + 1u, value & 0xffu);
}

unsigned int z80_read_memory_8(unsigned int address)
{
    return (s_zram != 0) ? MD_ZRAM(address) : 0u;
}

unsigned int z80_read_memory_16(unsigned int address)
{
    return (z80_read_memory_8(address) << 8) | z80_read_memory_8(address + 1u);
}

void gwenesis_z80inst_save_state(void) {}
void gwenesis_z80inst_load_state(void) {}
