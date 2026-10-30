/*
 * ArgonOS Mega Drive - the sound side on the other core (the CYD).
 *
 * On the CYD one core is not enough for the whole machine: the 68000 takes
 * 10-12 ms of a 16.7 ms frame, the YM2612 about 5 and Sonic's Z80, playing
 * its drums from flash, 6-8 more (30 Oct 2026).  So the 68000 keeps the
 * application core to itself, and the Z80, the YM2612 and the PSG run on the
 * system core, in the picture's thread, a frame behind:
 *
 *  - the 68000's writes to the YM2612 and the PSG, and to the Z80's bus
 *    request and reset, go into a log stamped with their line instead of
 *    reaching the chips - a log a frame, two of them, handed over at the
 *    frame's end;
 *  - the system core plays a frame's log line by line in the order the frame
 *    loop always kept: the line's writes, the Z80 to the line's end, the YM
 *    caught up and the line's samples mixed - and the Z80's interrupt at the
 *    start of vertical blank, which it can work out for itself;
 *  - the Z80's RAM is shared: the 68000 reads and writes it directly, as it
 *    does through the bus on the machine, and sees it a frame late - a sound
 *    command reaches the Z80 that much early, which no driver measured here
 *    minds;
 *  - what the 68000 reads back - the bus request's state, the YM's status -
 *    comes from its own copy and from the last the system core left.
 *
 * If the system core has not finished the last frame's sound when the next
 * frame ends, the 68000 waits for it: the sound is never dropped, and the
 * game slows only if the two cores together cannot keep up.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

#include <argon/argon.h>

#include "gwenesis_bus.h"
#include "gwenesis_sn76489.h"
#include "md_snd_async.h"
#include "md_sound.h"
#include "ym2612.h"
#include "z80inst.h"

extern int scan_line;

/*
 * An event is sixteen bits: two of kind, fourteen of what it carries.  The
 * line is an event of its own, logged only when it changes - a frame's
 * writes come in runs on a few lines, and 2 KB of log was what the CYD did
 * not have.
 */
#define EV_KIND_SHIFT 14
#define EV_LINE 0u /* the line the next events belong to */
#define EV_YM   1u /* (port << 8) | value */
#define EV_PSG  2u /* value */
#define EV_ZCTL 3u /* (bit 8: reset, else the bus request) | value */

/*
 * Events a log carries.  A frame with more - a game setting the YM's every
 * register at start - is handed over in pieces: the log so far goes to the
 * system core as the frame up to the current line, and the rest of the frame
 * continues in the other log.  Nothing is dropped; 256 is what the CYD's
 * memory holds two of.
 */
#define MD_EV_MAX 256

int md_snd_async;

/* A log: the events, and the lines of a frame they cover. */
typedef struct {
    uint16_t ev[MD_EV_MAX];
    uint16_t n;
    int16_t  last;  /* the line last marked, -1 for none */
    uint16_t from;  /* the first line this log plays */
    uint16_t to;    /* one past its last */
    uint16_t lines; /* the frame's */
    uint16_t vis;   /* lines shown: the Z80's interrupt comes after them */
    uint8_t  ends;  /* the frame's last piece */
} md_log_t;

static md_log_t          s_log[2];
static volatile uint8_t  s_ready[2];
static int               s_fill;  /* the 68000's log */
static int               s_play;  /* the next the system core plays */
static uint32_t          s_split; /* frames handed over in pieces, counted */
static int               s_z80_on;
static int               s_sound_on;
static volatile uint32_t s_play_us; /* the system core's time on sound */
static volatile uint8_t  s_ym_status;
volatile uint32_t md_snd_z80_us; /* the Z80's share of s_play_us */

static void log_reset(md_log_t *l, int from)
{
    l->n = 0;
    l->last = -1;
    l->from = (uint16_t)from;
}

void md_snd_async_start(int z80_on, int sound_on)
{
    s_z80_on = z80_on;
    s_sound_on = sound_on;
    s_fill = s_play = 0;
    log_reset(&s_log[0], 0);
    log_reset(&s_log[1], 0);
    s_ready[0] = s_ready[1] = 0;
    __sync_synchronize();
    md_snd_async = 1;
}

void md_snd_68k_frame_begin(int lines, int visible)
{
    md_log_t *l = &s_log[s_fill];
    l->lines = (uint16_t)lines;
    l->vis = (uint16_t)visible;
}

/* The 68000's log to the system core, covering lines up to `to`, and the
 * other one taken once the system core has finished with it. */
static void hand_over(int to, int ends)
{
    md_log_t *l = &s_log[s_fill];
    l->to = (uint16_t)to;
    l->ends = (uint8_t)ends;
    const uint16_t lines = l->lines, vis = l->vis;
    __sync_synchronize();
    s_ready[s_fill] = 1;
    s_fill ^= 1;
    while (md_snd_async && s_ready[s_fill]) {
        ag_delay(1);
    }
    md_log_t *next = &s_log[s_fill];
    log_reset(next, ends ? 0 : to);
    next->lines = lines;
    next->vis = vis;
}

static void put(uint32_t kind, uint32_t payload)
{
    const int line = (scan_line < 0) ? 0 : (scan_line & 0x1ff);
    md_log_t *l = &s_log[s_fill];
    if (l->n + 2u > MD_EV_MAX) {
        /* full: this line and the ones before it go now */
        s_split++;
        hand_over(line + 1, 0);
        l = &s_log[s_fill];
    }
    if (line != l->last) {
        l->ev[l->n++] = (uint16_t)((EV_LINE << EV_KIND_SHIFT) | (uint32_t)line);
        l->last = (int16_t)line;
    }
    l->ev[l->n++] = (uint16_t)((kind << EV_KIND_SHIFT) | (payload & 0x3fffu));
}

void md_snd_68k_ym(unsigned int port, unsigned int value)
{
    put(EV_YM, ((port & 3u) << 8) | (value & 0xffu));
}

unsigned int md_snd_68k_ym_status(void)
{
    return s_ym_status;
}

void md_snd_68k_psg(unsigned int value)
{
    put(EV_PSG, value & 0xffu);
}

void md_snd_68k_zctl(unsigned int address, unsigned int value)
{
    put(EV_ZCTL, ((address == 0x1200u) ? 0x100u : 0u) | (value ? 1u : 0u));
}

void md_snd_68k_frame_end(int lines, int visible)
{
    md_log_t *l = &s_log[s_fill];
    l->lines = (uint16_t)lines;
    l->vis = (uint16_t)visible;
    hand_over(lines, 1);
}

/* One event, at `clock` - the start of its line. */
static void apply(uint16_t e, int clock)
{
    const uint32_t kind = (uint32_t)e >> EV_KIND_SHIFT;
    const uint32_t p = (uint32_t)e & 0x3fffu;
    if (kind == EV_YM) {
        YM2612Write((p >> 8) & 3u, p & 0xffu, clock);
    } else if (kind == EV_PSG) {
        gwenesis_SN76489_Write((int)(p & 0xffu), clock);
    } else if (kind == EV_ZCTL) {
        if (s_z80_on) {
            z80_run(clock);
        }
        md_z80_ctl_apply((p & 0x100u) ? 0x1200u : 0x1100u, p & 1u);
    }
}

/* One log - a frame, or a piece of one - on the system core. */
static void play(const md_log_t *l)
{
    const int       lines = l->lines;
    const int       vis = l->vis;
    const uint16_t  n = l->n;
    const uint16_t *ev = l->ev;
    uint16_t        i = 0;

    if (s_sound_on && l->from == 0) {
        md_sound_begin_frame(lines);
    }
    for (int line = l->from; line < (int)l->to && line < lines; line++) {
        const int start = line * VDP_CYCLES_PER_LINE;
        const int end = start + VDP_CYCLES_PER_LINE;
        while (i < n) {
            const uint16_t e = ev[i];
            if (((uint32_t)e >> EV_KIND_SHIFT) == EV_LINE) {
                if ((int)(e & 0x3fffu) > line) {
                    break; /* the next run belongs to a later line */
                }
                i++;
                continue;
            }
            apply(e, start);
            i++;
        }
        if (s_z80_on) {
            const ag_time_t z0 = ag_micros();
            z80_run(end);
            md_snd_z80_us += (uint32_t)(ag_micros() - z0);
        } else if (end > zclk) {
            zclk = end;
        }
        if (s_sound_on && MD_SOUND_LINE_DUE(line, lines)) {
            ym2612_run(end);
            md_sound_line(line, lines);
        }
        if (s_z80_on) {
            if (line + 1 == vis) {
                z80_irq_line(1);
            } else if (line + 1 == vis + 1) {
                z80_irq_line(0);
            }
        }
    }
    /* what is left: stamped past the piece's lines - at its end */
    const int last = ((int)l->to < lines ? (int)l->to : lines) * VDP_CYCLES_PER_LINE;
    for (; i < n; i++) {
        if (((uint32_t)ev[i] >> EV_KIND_SHIFT) != EV_LINE) {
            apply(ev[i], last);
        }
    }
    if (!l->ends) {
        return;
    }
    if (s_sound_on) {
        md_sound_end_frame();
    }
    zclk = (zclk >= last) ? zclk - last : 0;
    s_ym_status = (uint8_t)YM2612Read(0);
}

int md_snd_core0_poll(void)
{
    if (!md_snd_async) {
        return 0;
    }
    const int k = s_play;
    if (!s_ready[k]) {
        return 0;
    }
    __sync_synchronize();
    const ag_time_t t0 = ag_micros();
    play(&s_log[k]);
    s_play_us += (uint32_t)(ag_micros() - t0);
    __sync_synchronize();
    s_ready[k] = 0;
    s_play = k ^ 1;
    return 1;
}

uint32_t md_snd_core0_us(void)
{
    return s_play_us;
}

uint32_t md_snd_split(void)
{
    return s_split;
}

void md_snd_async_stop(void)
{
    md_snd_async = 0;
    __sync_synchronize();
}
