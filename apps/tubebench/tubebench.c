/*
 * TUBEBENCH - what the one-dimensional valve model costs per audio sample.
 *
 *   run h:\tubebench.axe bench [out [model]]
 *                                      instructions a sample, whole chain and
 *                                      each piece of it
 *   run h:\tubebench.axe bake  [out [model]]
 *                                      what a preset load costs, and where the
 *                                      bake put the operating points and axes
 *
 * `model` is jcm800 (the default), bogner or slo - two valves, two valves, and
 * three with the last one cold.  The third valve is not free and the point of
 * being able to ask is that the answer is measured rather than extrapolated.
 *
 * `out` is an optional file to copy the report into - `h:\tube.txt` with
 * `argon test -HostFs build\sd_card`.  The console transcript is a rendered
 * screen, so anything that scrolls off it is gone; a file is not (rake 18 in
 * docs/08).
 *
 * Read this next to `cktbench baked`, which is the same amplifier built the
 * other way: a table of the solved nonlinearity over two axes, with the whole
 * reactive network carried as state.  That one measured 385 instructions a
 * stage, 625 with antialiasing, and three valves with a tone stack at twice the
 * rate came to 87% of a core.  This one moves the reactive network out into
 * biquads and keeps one axis, so the comparison to make is not table against
 * table - it is what each buys for the oversampling it can then afford.
 *
 * The numbers here are instructions, not cycles, and only because QEMU is run
 * with -icount shift=0.  On the chip they can only be larger: a dependent chain
 * of FPU operations stalls on latency, a cache miss costs tens of cycles, and
 * QEMU models neither.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <argon/argon.h>

#include <stdarg.h>
#include <stdio.h>

#include "ag_amp.h"
#include "ag_biq.h"
#include "ag_ckt.h"
#include "ag_os.h"
#include "ag_tube.h"

/*
 * The tables are 32 KB, the solver the bake borrows is another 21, and both are
 * too big for a stack.  A quarter megabyte of heap covers them with room to
 * spare.
 */
AG_APP_SIZED("TUBEBENCH", "0.1", "argon", 0, 16 * 1024, 256 * 1024);

#define RATE 22050u
/* The base rate this is voiced for.  Percentages below are against one core. */
#define CPU_HZ 240000000u

/*
 * Two thousand samples a block, four blocks, smallest block wins.
 *
 * Fewer than cktbench uses, because a sample here is the whole chain rather than
 * one primitive - two thousand of them at four times oversampling is already ten
 * million instructions, and -icount slows the emulator by about an order of
 * magnitude.  Minimum rather than mean, for the same reason as everywhere else:
 * everything that makes a block slower is interference, and the cheapest run has
 * the least of it.
 */
#define BENCH_N    2000
#define BENCH_REPS 4

/* ------------------------------------------------------------------------ */

static ag_handle_t s_out = AG_INVALID_HANDLE; /* zero is a valid handle */

static void out(const char *s)
{
    ag_print(s);
    if (s_out >= 0) {
        size_t n = 0;
        while (s[n]) {
            n++;
        }
        ag_write(s_out, s, n);
    }
}

static void outf(const char *fmt, ...)
{
    static char buf[200];
    va_list     ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out(buf);
}

/*
 * A float as text, three decimals, sign kept.
 *
 * The libc shim has no float printf, so this is done by hand - and the first
 * version did it by hand wrongly: (int)v of -0.822 is 0, so V1a's axis printed
 * as 0.69 .. 0.84 V when it is -0.82 .. 0.84.  A sign that disappears turns a
 * range that straddles zero into one that does not, which is exactly the kind of
 * thing that gets read as a modelling fault.
 *
 * Four rotating buffers, so several can appear in one call.
 */
static const char *f3(float v)
{
    static char buf[4][24];
    static int  turn;
    char       *p = buf[turn++ & 3];
    int         neg = v < 0.0f;
    long        milli;
    if (neg) {
        v = -v;
    }
    milli = (long)(v * 1000.0f + 0.5f);
    snprintf(p, sizeof(buf[0]), "%s%ld.%03ld", neg ? "-" : "", milli / 1000L,
             milli % 1000L);
    return p;
}

/* The shim's %s ignores field width, so the column is padded here. */
static const char *pad16(const char *s)
{
    static char p[20];
    int         i = 0;
    while (s[i] && i < 16) {
        p[i] = s[i];
        i++;
    }
    while (i < 16) {
        p[i++] = ' ';
    }
    p[16] = '\0';
    return p;
}

/*
 * Without this the loop measures nothing and says so in a way that is easy to
 * miss - see rake 1 in docs/08, where an empty loop reported 0.008 cycles.  The
 * empty asm with a "+f" constraint forces the value through a register every
 * pass and emits no instruction at all.
 */
#if defined(__XTENSA__)
#define OPAQUE_F(x) __asm__ __volatile__("" : "+f"(x))
#else
#define OPAQUE_F(x) __asm__ __volatile__("" : "+x"(x))
#endif

/* Instructions a sample, in thousandths, and percent of one core at RATE. */
static void report(const char *label, uint32_t block_us)
{
    const uint32_t mi = (uint32_t)(((uint64_t)block_us * 1000000u) / BENCH_N);
    /*
     * mi is thousandths of an instruction a sample.  One instruction a sample at
     * RATE against CPU_HZ, in basis points, is 10000 * RATE / CPU_HZ = 0.919, so
     * the whole conversion collapses to mi * 919 / 1000000.  Getting a scaling
     * like this wrong is how the first run of cktbench reported a two
     * instruction multiply as 40% of the CPU.
     */
    const uint32_t bp = (uint32_t)(((uint64_t)mi * 10000ull * RATE) /
                                   ((uint64_t)CPU_HZ * 1000ull));
    /*
     * Milliseconds of CPU per 20 ms of audio, which is just the percentage
     * scaled: using c% of a core means 20 ms of audio costs 0.2*c ms.  Derived
     * from bp rather than from mi with a constant in it, because the first
     * version of that constant was for 48 kHz and reported 60 ms where the
     * answer is 5.6 - a number five times too large, next to a percentage that
     * was right, in the same row.
     */
    const uint32_t us20 = bp * 2u; /* microseconds: 20 ms * bp / 10000 */
    outf("  %s %6u.%03u instr  %3u.%02u%% core  %2u.%03u ms / 20 ms\n",
         pad16(label), mi / 1000u, mi % 1000u, bp / 100u, bp % 100u, us20 / 1000u,
         us20 % 1000u);
}

/* ------------------------------------------------------------------------ */

/* A tone to drive it with, since a constant input would let the compiler hoist
 * everything and would not walk the curve. */
#define SINE_N 512
/*
 * How fast to walk the table, and it is not a detail.
 *
 * Stepping by one gives a 43 Hz tone at this rate, and a 43 Hz tone moves the
 * input by a third of a table cell a sample - so every lookup took the
 * same-cell path of the antialiasing average, the cheapest of its three, and
 * the whole benchmark measured a case that does not occur.  Real material slews
 * a hundred cells a sample.  Stepping by 23 is 990 Hz, which is a note a guitar
 * actually plays.
 */
#define SINE_STEP 23
static float s_sine[SINE_N];

static void sine_fill(void)
{
    int i;
    for (i = 0; i < SINE_N; i++) {
        float sn, cs;
        ag_sincosf(6.28318531f * (float)i / (float)SINE_N, &sn, &cs);
        s_sine[i] = 0.5f * sn;
    }
}

static ag_ckt_t *s_ckt;
static float    *s_tab;
static ag_amp_t *s_amp;
static float    *s_probe;
/* A third of a second: long enough for the attack and the top of the decay,
 * which is what the axis fit has to see, and short enough that four passes of it
 * are not most of the bake. */
#define PROBE_N 4000

static int alloc_all(void)
{
    s_ckt = (ag_ckt_t *)ag_malloc(sizeof(ag_ckt_t));
    s_tab = (float *)ag_malloc(sizeof(float) * AG_AMP_TAB_FLOATS);
    s_amp = (ag_amp_t *)ag_malloc(sizeof(ag_amp_t));
    s_probe = (float *)ag_malloc(sizeof(float) * PROBE_N);
    if (s_ckt == 0 || s_tab == 0 || s_amp == 0 || s_probe == 0) {
        out("out of heap\n");
        return -1;
    }
    ag_amp_probe_pluck(s_probe, PROBE_N, (float)RATE);
    return 0;
}

static void free_all(void)
{
    ag_free(s_ckt);
    ag_free(s_tab);
    ag_free(s_amp);
    ag_free(s_probe);
}

/*
 * One configuration, timed.
 *
 * The chain is built once by the caller and only re-voiced here.  Building per
 * configuration made this run seven full bakes for six numbers - 1.5 billion
 * instructions of emulator time to measure 60 million - and oversampling does not
 * change the curve, only which filters are designed for which rate.
 */
static void bench_chain(const char *label, int os, int adaa)
{
    ag_amp_cfg_t cfg = s_amp->cfg;
    uint32_t     best = 0xffffffffu;
    int          rep;

    cfg.os = os;
    cfg.adaa = adaa;
    if (ag_amp_set_voicing(s_amp, &cfg) != 0) {
        outf("  %s re-voice failed\n", label);
        return;
    }
    for (rep = 0; rep < BENCH_REPS; rep++) {
        float     acc = 0.0f;
        ag_time_t t0, t1;
        uint32_t  i;
        ag_amp_reset(s_amp);
        t0 = ag_micros();
        for (i = 0; i < BENCH_N; i++) {
            OPAQUE_F(acc);
            acc += ag_amp_tick(s_amp, s_sine[(i * SINE_STEP) & (SINE_N - 1)]);
        }
        t1 = ag_micros();
        {
            volatile float sink = acc;
            (void)sink;
        }
        if ((uint32_t)(t1 - t0) < best) {
            best = (uint32_t)(t1 - t0);
        }
    }
    report(label, best);
}

/* ------------------------------------------------------------------------ */
/* The pieces, so that the total is explicable rather than merely known.      */
/* ------------------------------------------------------------------------ */

static void bench_pieces(void)
{
    ag_biq_chain_t ch;
    ag_os2_t      *os2;
    ag_os4_t      *os4;

    /* One table lookup, no antialiasing: the thing the whole design is for. */
    {
        uint32_t best = 0xffffffffu;
        int      rep;
        for (rep = 0; rep < BENCH_REPS; rep++) {
            float     acc = 0.0f;
            ag_time_t t0, t1;
            uint32_t  i;
            (void)ag_tube_set_adaa(&s_amp->tube[0], 0);
            t0 = ag_micros();
            for (i = 0; i < BENCH_N; i++) {
                OPAQUE_F(acc);
                acc += ag_tube_tick(&s_amp->tube[0],
                                    s_sine[(i * SINE_STEP) & (SINE_N - 1)]);
            }
            t1 = ag_micros();
            {
                volatile float sink = acc;
                (void)sink;
            }
            if ((uint32_t)(t1 - t0) < best) {
                best = (uint32_t)(t1 - t0);
            }
        }
        report("curve lookup", best);
    }

    /* The same with the antialiasing average, which is where the branches are. */
    {
        uint32_t best = 0xffffffffu;
        int      rep;
        for (rep = 0; rep < BENCH_REPS; rep++) {
            float     acc = 0.0f;
            ag_time_t t0, t1;
            uint32_t  i;
            (void)ag_tube_set_adaa(&s_amp->tube[0], 1);
            ag_tube_reset(&s_amp->tube[0]);
            t0 = ag_micros();
            for (i = 0; i < BENCH_N; i++) {
                OPAQUE_F(acc);
                acc += ag_tube_tick(&s_amp->tube[0],
                                    s_sine[(i * SINE_STEP) & (SINE_N - 1)]);
            }
            t1 = ag_micros();
            {
                volatile float sink = acc;
                (void)sink;
            }
            if ((uint32_t)(t1 - t0) < best) {
                best = (uint32_t)(t1 - t0);
            }
        }
        report("+ antialiasing", best);
    }

    /* Three biquads, which is what F2 is. */
    {
        uint32_t best = 0xffffffffu;
        int      rep;
        ag_biq_chain_init(&ch);
        (void)ag_biq_hp1(ag_biq_chain_push(&ch), (float)RATE, 142.0f);
        (void)ag_biq_shelf1(ag_biq_chain_push(&ch), (float)RATE, 285.0f, -4.0f);
        (void)ag_biq_peak(ag_biq_chain_push(&ch), (float)RATE, 700.0f, 5.0f, 0.8f);
        for (rep = 0; rep < BENCH_REPS; rep++) {
            float     acc = 0.0f;
            ag_time_t t0, t1;
            uint32_t  i;
            t0 = ag_micros();
            for (i = 0; i < BENCH_N; i++) {
                OPAQUE_F(acc);
                acc += ag_biq_chain_tick(&ch, s_sine[(i * SINE_STEP) & (SINE_N - 1)]);
            }
            t1 = ag_micros();
            {
                volatile float sink = acc;
                (void)sink;
            }
            if ((uint32_t)(t1 - t0) < best) {
                best = (uint32_t)(t1 - t0);
            }
        }
        report("3 biquads", best);
    }

    /* The oversampling filters on their own, both directions. */
    os2 = (ag_os2_t *)ag_malloc(sizeof(ag_os2_t));
    os4 = (ag_os4_t *)ag_malloc(sizeof(ag_os4_t));
    if (os2 != 0) {
        uint32_t best = 0xffffffffu;
        int      rep;
        ag_os2_init(os2, AG_OS_M);
        for (rep = 0; rep < BENCH_REPS; rep++) {
            float     acc = 0.0f;
            ag_time_t t0, t1;
            uint32_t  i;
            t0 = ag_micros();
            for (i = 0; i < BENCH_N; i++) {
                float u[2];
                OPAQUE_F(acc);
                ag_os2_up(os2, s_sine[(i * SINE_STEP) & (SINE_N - 1)], u);
                acc += ag_os2_down(os2, u[0], u[1]);
            }
            t1 = ag_micros();
            {
                volatile float sink = acc;
                (void)sink;
            }
            if ((uint32_t)(t1 - t0) < best) {
                best = (uint32_t)(t1 - t0);
            }
        }
        report("2x up + down", best);
    }
    if (os4 != 0) {
        uint32_t best = 0xffffffffu;
        int      rep;
        ag_os4_init(os4);
        for (rep = 0; rep < BENCH_REPS; rep++) {
            float     acc = 0.0f;
            ag_time_t t0, t1;
            uint32_t  i;
            t0 = ag_micros();
            for (i = 0; i < BENCH_N; i++) {
                float u[4];
                OPAQUE_F(acc);
                ag_os4_up(os4, s_sine[(i * SINE_STEP) & (SINE_N - 1)], u);
                acc += ag_os4_down(os4, u);
            }
            t1 = ag_micros();
            {
                volatile float sink = acc;
                (void)sink;
            }
            if ((uint32_t)(t1 - t0) < best) {
                best = (uint32_t)(t1 - t0);
            }
        }
        report("4x up + down", best);
    }
    ag_free(os2);
    ag_free(os4);
}

/* ------------------------------------------------------------------------ */

static void run_bench(int model)
{
    ag_amp_cfg_t cfg;

    ag_amp_model(&cfg, model, (float)RATE);
    if (ag_amp_build(s_amp, s_ckt, &cfg, s_tab, 0, s_probe, PROBE_N) != 0) {
        out("  build failed\n");
        return;
    }
    outf("\nWhole chain: %d valve%s and their filter blocks, plus both voicing\n"
         "banks, no cabinet.\n",
         s_amp->n, s_amp->n == 1 ? "" : "s");
    out("The cabinet convolution that follows costs 981 instructions a sample on\n"
        "its own - 9.0% of a core at 22.05 kHz, measured by `cktbench fx` - so\n"
        "read these against that.\n\n");
    bench_chain("1x", 1, 0);
    bench_chain("1x + adaa", 1, 1);
    bench_chain("2x", 2, 0);
    bench_chain("2x + adaa", 2, 1);
    bench_chain("4x", 4, 0);
    bench_chain("4x + adaa", 4, 1);
    bench_chain("8x", 8, 0);
    bench_chain("8x + adaa", 8, 1);

    out("\nThe pieces, at the base rate.  Multiply the two valve rows and the\n"
        "biquad row by the oversampling factor; the filters outside the\n"
        "oversampled region and the up/down pair are paid once.\n\n");
    bench_pieces();
}

static void run_bake(int model)
{
    ag_amp_cfg_t cfg;
    ag_time_t    t0, t1;
    int          i;

    ag_amp_model(&cfg, model, (float)RATE);
    out("\nWhat a preset load costs.  Nearly all of it is the DC sweep going\n"
        "through the full matrix solver - about 18 600 instructions a point - so\n"
        "this is a preset load and not a knob, and about what the two-axis bake\n"
        "costs.  The change that would fix it is named in ag_tube.h.\n\n");

    t0 = ag_micros();
    if (ag_amp_build(s_amp, s_ckt, &cfg, s_tab, 0, s_probe, PROBE_N) != 0) {
        out("  build failed\n");
        return;
    }
    t1 = ag_micros();
    outf("  full build, axes fitted in three passes: %u instr, %u ms at 240 MHz\n",
         (uint32_t)(t1 - t0) * 1000u,
         (uint32_t)(((uint64_t)(t1 - t0) * 1000ull) / (CPU_HZ / 1000u)));
    outf("  tables: %u points a stage, %u KB for the pair\n",
         (unsigned)AG_AMP_TAB_N, (unsigned)(AG_AMP_TAB_FLOATS * 4u / 1024u));

    /* The live stages, not all four: past a->n the tube structs were never
     * baked, and printing an unbaked one is a row of zeroes that reads like a
     * stage which failed. */
    for (i = 0; i < s_amp->n; i++) {
        const ag_tube_t *tb = &s_amp->tube[i];
        static const char *const name[AG_AMP_STAGES] = { "V1a", "V1b", "V2a",
                                                         "V2b" };
        outf("  %s plate %s V  cathode %s V  gain %s\n", name[i],
             f3(tb->vq_plate), f3(tb->vq_cath),
             f3(s_amp->spec[i].ccath > 0.0f ? tb->gain_bypassed
                                            : tb->gain_unbypassed));
        outf("      axis %s .. %s V, %u nonconverged grid points\n", f3(tb->lo),
             f3(tb->hi), (unsigned)tb->bake_noncvg);
        cfg.axis_lo[i] = tb->lo;
        cfg.axis_hi[i] = tb->hi;
    }
    outf("  latency %s samples before the cabinet\n", f3(s_amp->latency_samples));

    /*
     * And again with those axes handed back, which is the case that matters for a
     * knob: no fitting passes, no coarse bakes, one sweep a stage.  A preset that
     * remembers where its own axes were is the difference between a control that
     * can be turned and one that reloads.
     */
    t0 = ag_micros();
    if (ag_amp_build(s_amp, s_ckt, &cfg, s_tab, 0, 0, 0) == 0) {
        t1 = ag_micros();
        outf("  rebuild with the axes remembered: %u instr, %u ms at 240 MHz\n",
             (uint32_t)(t1 - t0) * 1000u,
             (uint32_t)(((uint64_t)(t1 - t0) * 1000ull) / (CPU_HZ / 1000u)));
    }
}

int ag_main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "bench";
    int         model = AG_AMP_MODEL_JCM800;
    int         rc = 0;

    if (argc > 2 && argv[2][0] != '-') {
        s_out = ag_open(argv[2], AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
        if (s_out < 0) {
            ag_printf("cannot write %s (%d); console only\n", argv[2],
                      (int)s_out);
        }
    }
    /*
     * The model after the report file, because what a three-valve chain costs is
     * a different number from what a two-valve one costs and it cannot be got by
     * multiplying: the extra valve runs inside the oversampled region and brings
     * its own filter block with it, while the up/down pair and the first block
     * are paid once whatever the topology.
     */
    if (argc > 3) {
        model = ag_amp_model_by_name(argv[3]);
        if (model < 0) {
            ag_printf("unknown model '%s'; known: jcm800 bogner slo\n",
                      argv[3]);
            return 2;
        }
    }

    out("TUBEBENCH 0.1 - cost of the one-dimensional valve model\n");
    outf("model: %s\n", ag_amp_model_name(model));
    if (alloc_all() != 0) {
        return 1;
    }
    sine_fill();

    if (mode[0] == 'b' && mode[1] == 'a') {
        run_bake(model);
    } else if (mode[0] == 'b') {
        run_bench(model);
    } else {
        ag_printf("unknown mode '%s'; known: bench, bake\n", mode);
        rc = 2;
    }

    free_all();
    if (s_out >= 0) {
        ag_close(s_out);
        s_out = AG_INVALID_HANDLE;
        ag_printf("report written to %s\n", argv[2]);
    }
    return rc;
}
