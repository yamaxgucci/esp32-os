/*
 * CKTBENCH - what an analogue circuit costs per audio sample on this CPU.
 *
 *   run h:\cktbench.axe check [out]   is the cycle counter telling the truth?
 *   run h:\cktbench.axe prim  [out]   cost of the primitives everything is built of
 *   run h:\cktbench.axe parts [out] [settle] [meas]
 *   run h:\cktbench.axe ramp  [out] [settle] [meas] [stages]
 *   run h:\cktbench.axe mono  [out] [settle] [meas] [stages]
 *   run h:\cktbench.axe baked [out] [settle] [meas] [stages] [flags]
 *   run h:\cktbench.axe fx    [out] [blocks]  convolution and reverb
 *
 * baked flags: bit 0 turns on antialiasing, bit 1 folds a tone stack into
 * every stage.
 *
 * `out` is an optional file to copy the report into - `h:\ckt.txt` with
 * `argon run -HostFs build\sd_card`.  The console transcript is a rendered
 * screen, so anything that scrolls off it is gone; a file is not.
 *
 * Why primitives first.  The question behind this program is "how many
 * simulated components will the board carry in real time", and that question
 * has no single answer, because components do not cost the same thing:
 *
 *   - resistors are free at run time; they fold into constants when the
 *     circuit matrix is built and never appear again;
 *   - capacitors and inductors set the size N of the state vector, and the
 *     state update is a dense N x N matrix-vector product, so they cost
 *     O(N^2) - the tenth capacitor is dearer than the first;
 *   - diodes, JFETs and triodes are the only components that cost transcendental
 *     arithmetic, inside a Newton iteration that runs several times per sample,
 *     multiplied again by the oversampling factor.
 *
 * So the budget is decided by exp/log/pow latency, float multiply-accumulate
 * latency and float divide latency, and everything else is arithmetic on top of
 * those.  Measure them once, honestly, and the rest of the answer is
 * multiplication.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include <stdarg.h>
#include <stdio.h>

#include "ag_ckt.h"
#include "ag_fft.h"
#include "ag_fx.h"
#include "ag_ir.h"
#include "ag_mathf.h"
#include "ckt_circuits.h"

/* A megabyte of heap: the fx sweep loads a one-second impulse, which is 87
 * partitions, and the two spectra for those are 348 KB before the impulse
 * itself and the resampling buffer. */
AG_APP_SIZED("CKTBENCH", "0.3", "argon", 0, 16 * 1024, 1024 * 1024);

/*
 * Each probe runs BENCH_N iterations, BENCH_REPS times, and the smallest block
 * wins.  Minimum, not mean: everything that makes a block slower - a timer
 * interrupt, the console task, a cache miss on first touch - is interference,
 * and the cheapest run is the one with the least of it.  A mean would report
 * the interference as if it were the cost of the arithmetic.
 */
#define BENCH_N    20000
#define BENCH_REPS 8

/* CPU clock assumed for the percentages.  ESP32-S3 runs at 240 MHz. */
#define CPU_HZ 240000000u

/* ------------------------------------------------------------------------ */
/* report sink: console always, file too when asked                          */
/* ------------------------------------------------------------------------ */

/* Zero is a valid handle - ag_open reports failure with a negative value. */
static ag_handle_t s_out = AG_INVALID_HANDLE;

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
    /* One buffer, one place that knows the width; the kernel printf truncates
     * at 255 anyway, so nothing here may be longer. */
    static char buf[200];
    va_list     ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out(buf);
}

/* The shim's %s ignores field width, so the column is padded here instead. */
static const char *pad13(const char *s)
{
    static char p[16];
    int         i = 0;
    while (s[i] && i < 13) {
        p[i] = s[i];
        i++;
    }
    while (i < 13) {
        p[i++] = ' ';
    }
    p[13] = '\0';
    return p;
}

/* ------------------------------------------------------------------------ */
/* the optimiser barrier                                                     */
/* ------------------------------------------------------------------------ */

/*
 * Without this the first version of this program measured nothing and said so
 * in a way that was easy to miss: "empty loop 0.008 cyc", "int64 mul 0.000
 * cyc".  GCC is entitled to turn `for (i) acc += i` into a closed form and
 * `for (i) acc = acc*3+7` into whatever it likes, because only the final value
 * is used - and at -Os it takes that right.  The empty asm with a "+r"/"+f"
 * constraint forces the value through a register on every pass without
 * emitting a single instruction, so the loop stays a loop and costs exactly
 * what its body costs.
 */
#if defined(__XTENSA__)
#define OPAQUE_I(x) __asm__ __volatile__("" : "+r"(x))
#define OPAQUE_F(x) __asm__ __volatile__("" : "+f"(x))
#else
#define OPAQUE_I(x) __asm__ __volatile__("" : "+r"(x))
#define OPAQUE_F(x) __asm__ __volatile__("" : "+x"(x))
#endif

static uint32_t s_loop_overhead; /* milli-instructions per iteration */

/*
 * Instructions retired per iteration, in thousandths.
 *
 * The argument is the microseconds the block took, and it is an instruction
 * count only because QEMU is run with -icount shift=0, where one instruction
 * advances virtual time by exactly one nanosecond.  Without that switch this
 * number is host wall time and means nothing about the chip; with it, it is
 * the same on a loaded machine as on an idle one, which is the whole point.
 */
static uint32_t minstr(uint32_t block_us)
{
    return (uint32_t)(((uint64_t)block_us * 1000000u) / BENCH_N);
}

static void report(const char *label, uint32_t block_us)
{
    uint32_t mi = minstr(block_us);
    uint32_t net = mi > s_loop_overhead ? mi - s_loop_overhead : 0u;
    /*
     * "% of one core" is the number that decides the design: an operation
     * costing c cycles once per sample takes c * 48000 cycles of the
     * 240 000 000 a core has in a second.  Printed at one cycle per
     * instruction, which is the floor - a dependent chain of FPU operations
     * stalls on latency and costs more.
     *
     * net is in thousandths of an instruction, so the whole conversion -
     * /1000 to instructions, x48000 samples, /240e6 cycles, x10000 for basis
     * points - collapses to a division by 500.  Getting that scaling wrong is
     * how the first run of this program reported a two-instruction multiply as
     * 40% of the CPU.
     */
    uint32_t bp = net / 500u; /* basis points: 100 = 1.00% */

    outf("  %s %5u.%03u instr %3u.%02u%%\n", pad13(label), net / 1000u,
         net % 1000u, bp / 100u, bp % 100u);
}

#define BENCH_I(label, INIT, BODY)                                            \
    do {                                                                      \
        uint32_t best = 0xffffffffu;                                          \
        int      rep;                                                         \
        for (rep = 0; rep < BENCH_REPS; rep++) {                              \
            volatile int32_t sink;                                            \
            int32_t          acc = (INIT);                                    \
            ag_time_t        t0, t1;                                          \
            uint32_t         i;                                               \
            t0 = ag_micros();                                                 \
            for (i = 0; i < BENCH_N; i++) {                                   \
                OPAQUE_I(acc);                                                \
                BODY;                                                         \
            }                                                                 \
            t1 = ag_micros();                                                 \
            sink = acc;                                                       \
            (void)sink;                                                       \
            if ((uint32_t)(t1 - t0) < best) {                                 \
                best = (uint32_t)(t1 - t0);                                   \
            }                                                                 \
        }                                                                     \
        report((label), best);                                                \
    } while (0)

#define BENCH_F(label, INIT, BODY)                                            \
    do {                                                                      \
        uint32_t best = 0xffffffffu;                                          \
        int      rep;                                                         \
        for (rep = 0; rep < BENCH_REPS; rep++) {                              \
            volatile float sink;                                              \
            float          acc = (INIT);                                      \
            ag_time_t      t0, t1;                                            \
            uint32_t       i;                                                 \
            t0 = ag_micros();                                                 \
            for (i = 0; i < BENCH_N; i++) {                                   \
                OPAQUE_F(acc);                                                \
                BODY;                                                         \
            }                                                                 \
            t1 = ag_micros();                                                 \
            sink = acc;                                                       \
            (void)sink;                                                       \
            if ((uint32_t)(t1 - t0) < best) {                                 \
                best = (uint32_t)(t1 - t0);                                   \
            }                                                                 \
        }                                                                     \
        report((label), best);                                                \
    } while (0)

/* ------------------------------------------------------------------------ */
/* check - does the cycle counter mean cycles?                               */
/* ------------------------------------------------------------------------ */

/*
 * Trusting a counter because it exists is how three earlier defects in this
 * project survived: the tool that measures has to be measured first.  Two
 * questions, and both have a right answer known in advance.
 *
 * 1. Does it scale with work?  Ten times the iterations must cost ten times
 *    the cycles.  If it does not, the counter is sampling something else.
 * 2. What does it imply about the clock?  Divide cycles by the microseconds
 *    the same block took.  On silicon that quotient is 240 for every workload,
 *    because that is what "240 MHz" means.  If the quotient comes out
 *    different for integer work and for float work, the counter is counting
 *    instructions, not cycles - which is exactly what an emulator would do,
 *    and it makes every number here a lower bound rather than an estimate.
 */
static void run_check(void)
{
    static const uint32_t k_n[3] = { 4000u, 40000u, 400000u };
    int                   which;

    out("\ncheck: scaling with work (must be linear, and repeatable)\n");
    /* instr/it is only a count of instructions when QEMU runs with
     * -icount shift=0, where one instruction advances the clock by one
     * nanosecond.  On silicon the same column is just microseconds spread
     * over the iterations. */
    out("        iters   cycles  cyc/it      us  instr/it\n");

    for (which = 0; which < 2; which++) {
        int step;
        outf("  %s\n", which == 0 ? "integer chain" : "float chain");
        for (step = 0; step < 3; step++) {
            uint32_t n = k_n[step];
            uint32_t bestc = 0xffffffffu;
            uint32_t bestns = 0xffffffffu;
            int      rep;
            for (rep = 0; rep < 4; rep++) {
                uint32_t  t0, t1, i;
                ag_time_t u0, u1;
                u0 = ag_micros();
                t0 = (uint32_t)ag_cycles();
                if (which == 0) {
                    volatile int32_t sink;
                    int32_t          a = 1;
                    for (i = 0; i < n; i++) {
                        OPAQUE_I(a);
                        a = a * 3 + 7;
                    }
                    sink = a;
                    (void)sink;
                } else {
                    volatile float sink;
                    float          a = 1.0f;
                    for (i = 0; i < n; i++) {
                        OPAQUE_F(a);
                        a = a * 0.9999f + 0.001f;
                    }
                    sink = a;
                    (void)sink;
                }
                t1 = (uint32_t)ag_cycles();
                u1 = ag_micros();
                if ((uint32_t)(t1 - t0) < bestc) {
                    bestc = (uint32_t)(t1 - t0);
                }
                if ((uint32_t)(u1 - u0) < bestns) {
                    bestns = (uint32_t)(u1 - u0);
                }
            }
            outf("      %7u %8u  %2u.%03u %7u  %4u.%03u\n", n, bestc,
                 (uint32_t)((uint64_t)bestc * 1000u / n / 1000u),
                 (uint32_t)((uint64_t)bestc * 1000u / n % 1000u), bestns,
                 (uint32_t)((uint64_t)bestns * 1000000u / n / 1000u),
                 (uint32_t)((uint64_t)bestns * 1000000u / n % 1000u));
        }
    }

    out("\ncheck: implied clock (cycles per microsecond)\n");
    out("  240 for every row = real cycles.  Different per workload = the\n"
        "  counter is counting instructions, so every cost here is a floor.\n");

    for (which = 0; which < 3; which++) {
        uint32_t  n = 400000u;
        uint32_t  c0, c1, i;
        ag_time_t u0, u1;
        uint64_t  dc, du;
        volatile float fsink;
        volatile int32_t isink;
        float          fa = 1.0f;
        int32_t        ia = 1;

        u0 = ag_micros();
        c0 = (uint32_t)ag_cycles();
        if (which == 0) {
            for (i = 0; i < n; i++) {
                OPAQUE_I(ia);
                ia = ia * 3 + 7;
            }
        } else if (which == 1) {
            for (i = 0; i < n; i++) {
                OPAQUE_F(fa);
                fa = fa * 0.9999f + 0.001f;
            }
        } else {
            for (i = 0; i < n / 8u; i++) {
                OPAQUE_F(fa);
                fa = ag_expf(fa * 1.0e-6f);
            }
        }
        c1 = (uint32_t)ag_cycles();
        u1 = ag_micros();
        fsink = fa;
        isink = ia;
        (void)fsink;
        (void)isink;

        dc = (uint32_t)(c1 - c0);
        du = (uint64_t)(u1 - u0);
        if (du == 0) {
            du = 1;
        }
        outf("  %s cycles %9u  us %7u  implied %4u MHz\n",
             pad13(which == 0 ? "int chain"
                              : (which == 1 ? "float chain" : "ag_expf")),
             (uint32_t)dc, (uint32_t)du, (uint32_t)(dc / du));
    }
}

/* ------------------------------------------------------------------------ */
/* prim - the primitives a circuit solver is built from                      */
/* ------------------------------------------------------------------------ */

/* A 256-point table with linear interpolation - what every "solved
 * nonlinearity cached in a table" path reduces to. */
static float s_lut[257];

static void lut_fill(void)
{
    int i;
    for (i = 0; i < 257; i++) {
        s_lut[i] = ag_tanhf(((float)i - 128.0f) * (1.0f / 32.0f));
    }
}

static float lut_read(float x)
{
    float u = x * 32.0f + 128.0f;
    int   i;
    float f;
    if (u < 0.0f) {
        u = 0.0f;
    }
    if (u > 255.0f) {
        u = 255.0f;
    }
    i = (int)u;
    f = u - (float)i;
    return s_lut[i] + (s_lut[i + 1] - s_lut[i]) * f;
}

static void run_prim(void)
{
    out("\nprim: instructions retired, min of 8 blocks x 20000 iterations.\n"
        "Chains are dependent on purpose - that is what a Newton step pays.\n"
        "%: one such operation, once per sample at 48 kHz, against one\n"
        "240 MHz core at one cycle per instruction (a floor).\n\n");

    {
        uint32_t best = 0xffffffffu;
        int      rep;
        for (rep = 0; rep < BENCH_REPS; rep++) {
            volatile int32_t sink;
            int32_t          acc = 1;
            ag_time_t        t0, t1;
            uint32_t         i;
            t0 = ag_micros();
            for (i = 0; i < BENCH_N; i++) {
                OPAQUE_I(acc);
            }
            t1 = ag_micros();
            sink = acc;
            (void)sink;
            if ((uint32_t)(t1 - t0) < best) {
                best = (uint32_t)(t1 - t0);
            }
        }
        s_loop_overhead = minstr(best);
        outf("  %s %5u.%03u instr (subtracted below)\n", pad13("bare loop"),
             s_loop_overhead / 1000u, s_loop_overhead % 1000u);
    }

    out("\ninteger\n");
    BENCH_I("mul+add", 1, acc = acc * 3 + 7);
    BENCH_I("64x64>>15", 1, acc = (int32_t)(((int64_t)acc * 30000) >> 15) + 1);

    out("\nfloat (hardware FPU)\n");
    BENCH_F("mul+add", 1.0f, acc = acc * 0.9999f + 0.001f);
    BENCH_F("divide", 1.0f, acc = 1.0f / (acc + 2.0f));
    BENCH_F("compare", 1.0f, acc = acc > 0.5f ? acc * 0.5f : acc + 0.5f);
    BENCH_F("int<->float", 1.0f, acc = (float)((int)(acc * 100.0f) + 1) * 0.01f);

    out("\ntranscendental (ag_mathf, no libm)\n");
    BENCH_F("ag_expf", 0.0f, acc = ag_expf(acc * 1.0e-6f));
    BENCH_F("ag_logf", 1.0f, acc = ag_logf(acc + 1.0f) + 1.0f);
    BENCH_F("ag_powf", 1.0f, acc = ag_powf(acc + 1.0f, 1.4f) * 0.5f);
    BENCH_F("ag_tanhf", 0.5f, acc = ag_tanhf(acc) + 0.1f);
    BENCH_F("ag_sqrtf", 2.0f, acc = ag_sqrtf(acc) + 1.0f);

    out("\ntable lookup - a solved nonlinearity, cached\n");
    lut_fill();
    BENCH_F("lut256 lerp", 0.5f, acc = lut_read(acc) + 0.3f);

    /*
     * Reads of the timer cannot be costed with the timer under -icount: the
     * clock is driven by instructions retired, and an MMIO read is not one of
     * them, so a loop of nothing but timer reads shows no elapsed time at all.
     * Left in because a zero here is a positive signal that icount is on.
     */
    out("\nthe timer itself (0.000 = icount is on, see the note)\n");
    BENCH_I("ag_micros()", 0, acc += (int32_t)ag_micros());

    out("\nbudget, one core: 5442 cyc/sample at 44.1k, 5000 at 48k,\n"
        "2500 at 96k.  Divide by the oversampling factor for the part of\n"
        "the circuit that is nonlinear.\n");
}

/* ------------------------------------------------------------------------ */
/* parts and ramp - what a real circuit costs per sample                     */
/* ------------------------------------------------------------------------ */

#define CKT_FS  48000.0f
#define SINE_N  48 /* one period of 1 kHz at 48 kHz                        */

static ag_ckt_t s_ckt;
static float    s_sine[SINE_N];

/*
 * A 1 kHz period, built once by the magic-circle recurrence so that no
 * trigonometry runs inside a measured window.  The oscillator would only cost
 * a handful of instructions against the thousands the solver spends, but a
 * benchmark that includes its own signal generator in the figure is a
 * benchmark nobody can compare against anything.
 */
static void sine_fill(void)
{
    const float eps = 0.13080626f; /* 2*sin(pi*1000/48000) */
    float       s = 0.0f, c = 1.0f;
    int         i;
    for (i = 0; i < SINE_N; i++) {
        s_sine[i] = s;
        s += eps * c;
        c -= eps * s;
    }
}

static void bench_head(void)
{
    out("  circuit          unk  nl   iters  instr/smp ms/20ms  %core x8OS\n");
}

/*
 * "How long does 20 ms of audio take" is the number worth quoting, and it is
 * the same measurement wearing different units: 20 ms at 48 kHz is 960
 * samples, a 240 MHz core does 240 000 cycles in a millisecond, so at one
 * cycle per instruction the answer is instructions/sample divided by 250.
 * Under 20 means it fits.  Printed in hundredths of a millisecond.
 */
static uint32_t ms20_x100(uint32_t instr_per_sample)
{
    return instr_per_sample * 2u / 5u;
}

/*
 * Report one circuit.  `iters` is the mean Newton passes per sample and is the
 * number that explains the rest: the solve is repeated that many times.
 */
static void bench_report(const char *name, const ag_ckt_t *k,
                         uint32_t instr_per_sample, uint32_t meas_n)
{
    const uint32_t it100 =
        meas_n ? (uint32_t)(((uint64_t)k->iters_total * 100u) / meas_n) : 0u;
    /* 5000 cycles is one sample at 48 kHz on a 240 MHz core, so an
     * instruction count divided by 50 is a percentage directly. */
    const uint32_t ms100 = ms20_x100(instr_per_sample);
    const uint32_t p10 = instr_per_sample / 5u; /* tenths of a percent */
    const uint32_t p10x8 = instr_per_sample * 8u / 5u;

    outf("  %s %3d %3d %4u.%02u %9u %4u.%02u %4u.%u%% %4u%%\n", pad13(name),
         k->n, k->nnl, it100 / 100u, it100 % 100u, instr_per_sample,
         ms100 / 100u, ms100 % 100u, p10 / 10u, p10 % 10u, p10x8 / 10u);
}

/*
 * Settle the bias, then measure a whole number of signal periods.  The settle
 * is not part of the figure: a valve stage takes tens of milliseconds to reach
 * its operating point through the cathode bypass, and the first samples cost
 * far more Newton passes than the steady state does.
 */
static uint32_t bench_run(ag_ckt_t *k, int out_node, float amp, uint32_t settle,
                          uint32_t meas)
{
    uint32_t  i;
    ag_time_t t0, t1;
    volatile float sink = 0.0f;
    float     acc = 0.0f;

    for (i = 0; i < settle; i++) {
        acc += ag_ckt_tick(k, amp * s_sine[i % SINE_N], out_node);
    }

    k->iters_total = 0;
    t0 = ag_micros();
    for (i = 0; i < meas; i++) {
        acc += ag_ckt_tick(k, amp * s_sine[i % SINE_N], out_node);
    }
    t1 = ag_micros();
    sink = acc;
    (void)sink;

    return meas ? (uint32_t)(((uint64_t)(t1 - t0) * 1000u) / meas) : 0u;
}

static void run_parts(uint32_t settle, uint32_t meas)
{
    int out_node;

    out("\nparts: one sample through one circuit, instructions retired.\n"
        "unk is the size of the linear system, nl the number of devices that\n"
        "have to be re-linearised on every Newton pass.  %core is one 240 MHz\n"
        "core at 48 kHz; x4OS and x8OS are the same circuit oversampled, which\n"
        "an overdrive needs and a tone stack does not.\n\n");
    bench_head();

    out_node = ckt_build_rc(&s_ckt, CKT_FS);
    if (out_node) {
        bench_report("RC lowpass", &s_ckt,
                     bench_run(&s_ckt, out_node, 1.0f, 64u, meas), meas);
    }

    out_node = ckt_build_tonestack(&s_ckt, CKT_FS);
    if (out_node) {
        bench_report("tone stack", &s_ckt,
                     bench_run(&s_ckt, out_node, 1.0f, 512u, meas), meas);
    }

    out_node = ckt_build_diode_clipper(&s_ckt, CKT_FS);
    if (out_node) {
        bench_report("diode pair", &s_ckt,
                     bench_run(&s_ckt, out_node, 5.0f, 512u, meas), meas);
    }

    out_node = ckt_build_jfet_stage(&s_ckt, CKT_FS);
    if (out_node) {
        bench_report("JFET stage", &s_ckt,
                     bench_run(&s_ckt, out_node, 0.2f, settle, meas), meas);
    }

    out_node = ckt_build_preamp(&s_ckt, CKT_FS, 1, 22.0f);
    if (out_node) {
        bench_report("12AX7 clean", &s_ckt,
                     bench_run(&s_ckt, out_node, 0.01f, settle, meas), meas);
        out_node = ckt_build_preamp(&s_ckt, CKT_FS, 1, 22.0f);
        bench_report("12AX7 driven", &s_ckt,
                     bench_run(&s_ckt, out_node, 3.0f, settle, meas), meas);
    }
}

static void stage_name(char *buf, int s)
{
    buf[0] = (char)('0' + s);
    buf[1] = ' ';
    buf[2] = 's';
    buf[3] = 't';
    buf[4] = 'a';
    buf[5] = 'g';
    buf[6] = 'e';
    buf[7] = s > 1 ? 's' : ' ';
    buf[8] = '\0';
}

static void run_mono(int max_stages, uint32_t settle, uint32_t meas)
{
    int s;

    out("\nmono: the whole preamp as one matrix.  This is what a circuit\n"
        "simulator does and it is the wrong shape for a cascade - the solve\n"
        "grows as the cube of the unknowns, and the stages multiply the\n"
        "solver's own rounding error by the total gain of the chain.\n\n");
    bench_head();

    for (s = 1; s <= max_stages; s++) {
        const int out_node = ckt_build_preamp(&s_ckt, CKT_FS, s, 22.0f);
        char      name[16];
        if (!out_node) {
            outf("  %d stages: does not fit the solver\n", s);
            break;
        }
        stage_name(name, s);
        bench_report(name, &s_ckt,
                     bench_run(&s_ckt, out_node, 1.0f, settle, meas), meas);
    }
}

static ckt_chain_t s_chain;

static void run_ramp(int max_stages, uint32_t settle, uint32_t meas)
{
    int s;

    out("\nramp: 12AX7 stages, one matrix per stage, each carrying a lumped\n"
        "copy of the next one's input network as its load.  Driven at 1 kHz\n"
        "hard enough that every stage is clipping.\n\n");
    bench_head();

    if (max_stages > CKT_MAX_CHAIN) {
        max_stages = CKT_MAX_CHAIN;
    }
    for (s = 1; s <= max_stages; s++) {
        char     name[16];
        uint32_t i;
        ag_time_t t0, t1;
        volatile float sink;
        float    acc = 0.0f;
        uint32_t instr;

        if (!ckt_build_chain(&s_chain, CKT_FS, s)) {
            outf("  %d stages: does not fit\n", s);
            break;
        }
        for (i = 0; i < settle; i++) {
            acc += ckt_chain_tick(&s_chain, s_sine[i % SINE_N]);
        }
        ckt_chain_reset_stats(&s_chain);
        t0 = ag_micros();
        for (i = 0; i < meas; i++) {
            acc += ckt_chain_tick(&s_chain, s_sine[i % SINE_N]);
        }
        t1 = ag_micros();
        sink = acc;
        (void)sink;
        instr = meas ? (uint32_t)(((uint64_t)(t1 - t0) * 1000u) / meas) : 0u;

        stage_name(name, s);
        {
            const uint32_t it100 =
                meas ? (unsigned)(ckt_chain_iters(&s_chain) * 100u) / meas : 0u;
            const uint32_t ms100 = ms20_x100(instr);
            const uint32_t p10 = instr / 5u;
            outf("  %s %3d %3d %4u.%02u %9u %4u.%02u %4u.%u%% %4u%%\n",
                 pad13(name), s_chain.stage[0].n, s, it100 / 100u, it100 % 100u,
                 instr, ms100 / 100u, ms100 % 100u, p10 / 10u, p10 % 10u,
                 instr * 8u / 50u);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* baked - the same stages with the nonlinearity solved in advance           */
/* ------------------------------------------------------------------------ */

static ckt_baked_chain_t s_baked;

/* flags: bit 0 = antialiasing, bit 1 = tone stack folded into every stage */
static void run_baked(int max_stages, uint32_t meas, unsigned flags)
{
    const int adaa = (flags & 1u) ? 1 : 0;
    const int variant =
        (flags & 2u) ? CKT_VARIANT_TONESTACK : CKT_VARIANT_PLAIN;
    int s;

    out("\nbaked: the same 12AX7 stages, each reduced at build time to a few\n"
        "dozen constants and a table of the solved nonlinearity.  Same model,\n"
        "same circuit, same answer to within -63 dB of the solver - measured\n"
        "in host-tests/test_ckt.c, not asserted here.\n");
    outf("  antialiasing: %s;  tone stack folded in: %s\n\n",
         adaa ? "on" : "off", variant == CKT_VARIANT_TONESTACK ? "yes" : "no");
    bench_head();

    if (max_stages > CKT_MAX_CHAIN) {
        max_stages = CKT_MAX_CHAIN;
    }
    for (s = 1; s <= max_stages; s++) {
        char           name[16];
        uint32_t       i, instr, bake_us;
        ag_time_t      t0, t1;
        volatile float sink;
        float          acc = 0.0f;

        t0 = ag_micros();
        if (ckt_bake_chain(&s_baked, &s_ckt, CKT_FS, s, s_sine, SINE_N, 6000,
                           variant, adaa) != 0) {
            outf("  %d stages: bake failed\n", s);
            break;
        }
        t1 = ag_micros();
        bake_us = (uint32_t)(t1 - t0);

        /* No settle: a baked chain starts at its operating point. */
        t0 = ag_micros();
        for (i = 0; i < meas; i++) {
            acc += ckt_baked_chain_tick(&s_baked, s_sine[i % SINE_N]);
        }
        t1 = ag_micros();
        sink = acc;
        (void)sink;
        instr = meas ? (uint32_t)(((uint64_t)(t1 - t0) * 1000u) / meas) : 0u;

        stage_name(name, s);
        {
            const uint32_t ms100 = ms20_x100(instr);
            const uint32_t p10 = instr / 5u;
            outf("  %s   -  %2d     -   %9u %4u.%02u %4u.%u%% %4u%%\n",
                 pad13(name), s, instr, ms100 / 100u, ms100 % 100u, p10 / 10u,
                 p10 % 10u, instr * 8u / 50u);
            outf("      bake %u M instr, tables %u KB, %u lookups off the "
                 "edge\n",
                 bake_us / 1000u,
                 (unsigned)(s * (adaa ? 2 : 1) * CKT_BAKE_N1 * CKT_BAKE_N2 * 2 *
                            sizeof(float) / 1024u),
                 ckt_baked_chain_clamped(&s_baked));
        }
    }
}

/* ------------------------------------------------------------------------ */
/* fx - what the convolution and the reverb cost next to the valves          */
/* ------------------------------------------------------------------------ */

/*
 * The valve stages are not the only thing in the signal path, and the obvious
 * question - is the cabinet dearer than the reverb - has an obvious wrong
 * answer.  A reverb is a handful of delay lines and one-pole filters; a
 * convolution is two 512-point transforms per 256 samples whatever the IR is,
 * plus one complex multiply-accumulate per bin per partition.  So the reverb
 * cost is flat and small, and the convolution has a floor it can never go
 * below - the pair of transforms - with the IR length adding to it.
 *
 * Measured, not argued, in the same units as the rest of this program.
 */
#define FX_RATE   22050u
#define FX_BLOCKS 64u
#define FX_REPS   4

static ag_ir_t s_fx_ir;
static ag_fx_t s_fx;
static int16_t s_fx_mono[AG_IR_BLOCK];
static int16_t s_fx_stereo[AG_IR_BLOCK * 2];

static void fx_fill(void)
{
    uint32_t i, rng = 12345u;
    for (i = 0; i < AG_IR_BLOCK; i++) {
        rng = rng * 1664525u + 1013904223u;
        s_fx_mono[i] = (int16_t)((int32_t)(rng >> 16) - 32768) / 4;
        s_fx_stereo[i * 2] = s_fx_mono[i];
        s_fx_stereo[i * 2 + 1] = s_fx_mono[i];
    }
}

/*
 * Blocks, not samples: both engines work a block at a time, so the per-sample
 * figure is the block cost spread over the 256 samples it covered.
 */
static uint32_t fx_instr(uint32_t block_us, uint32_t blocks)
{
    return blocks ? (uint32_t)(((uint64_t)block_us * 1000u) /
                               ((uint64_t)blocks * AG_IR_BLOCK))
                  : 0u;
}

/*
 * Run the convolution until its delay line is full, then time it.
 *
 * Not optional, and the first version of this got it wrong.  A slot of X that
 * has never been written is marked AG_IR_EMPTY and skipped, so a fresh engine
 * with 87 partitions does almost no multiply-accumulate at all for its first
 * 87 blocks - and taking the best of several repetitions makes it worse, since
 * the best one is the coldest one.  Measured that way, a one-second impulse
 * cost exactly what a half-second one did, which is the giveaway: the work
 * that was supposed to double had not started yet.
 */
static uint32_t fx_time_ir(ag_ir_t *ir, uint32_t blocks)
{
    uint32_t best = 0xffffffffu, i;
    int      rep;

    for (i = 0; i < ir->parts + 2u; i++) {
        ag_ir_process_block(ir, s_fx_mono, s_fx_stereo);
    }
    for (rep = 0; rep < FX_REPS; rep++) {
        ag_time_t t0, t1;
        uint32_t  b;
        t0 = ag_micros();
        for (b = 0; b < blocks; b++) {
            ag_ir_process_block(ir, s_fx_mono, s_fx_stereo);
        }
        t1 = ag_micros();
        if ((uint32_t)(t1 - t0) < best) {
            best = (uint32_t)(t1 - t0);
        }
    }
    return fx_instr(best, blocks);
}

static void fx_head(void)
{
    out("  engine          parts  taps    KB  instr/smp  %core  %core@22k\n");
}

/*
 * KB is the two spectra, H and X: one partition of each is 512 bins of int16
 * re and im, so two kilobytes apiece.  It is the number that decides how long
 * an impulse the board will hold, and it lives in PSRAM.
 */
static void fx_line(const char *name, uint32_t parts, uint32_t taps,
                    uint32_t instr)
{
    const uint32_t p10 = instr / 5u;         /* tenths of a % at 48 kHz  */
    const uint32_t q10 = instr * 2205u / 4800u / 5u; /* the same at 22.05 */
    const uint32_t kb = parts * AG_IR_FFT * 2u * 2u * 2u / 1024u;

    outf("  %s %5u %5u %5u %9u %4u.%u%% %5u.%u%%\n", pad13(name), parts, taps,
         kb, instr, p10 / 10u, p10 % 10u, q10 / 10u, q10 % 10u);
}

static void run_fx(uint32_t blocks)
{
    static const int k_preset[3] = { 3, 0, 1 };
    static const char *const k_name[3] = { "cab 20 ms", "room", "hall 500 ms" };
    int i, rep;

    out("\nfx: the convolution and the reverb, in the same instructions per\n"
        "sample as the valve stages above.  Both run at 22.05 kHz in IRFX, so\n"
        "the last column is the honest one for those; the %core column keeps\n"
        "the 48 kHz basis the rest of this program uses.\n\n");
    fx_head();

    fx_fill();

    for (i = 0; i < 3; i++) {
        uint32_t best = 0xffffffffu, parts = 0, taps = 0;
        if (ag_ir_init(&s_fx_ir, FX_RATE) != 0) {
            outf("  %s: init failed\n", k_name[i]);
            continue;
        }
        if (ag_ir_load_preset(&s_fx_ir, k_preset[i]) != 0) {
            out("  load failed\n");
            ag_ir_free(&s_fx_ir);
            continue;
        }
        parts = s_fx_ir.parts;
        taps = s_fx_ir.ir_frames;
        best = fx_time_ir(&s_fx_ir, blocks);
        fx_line(k_name[i], parts, taps, best);
        ag_ir_free(&s_fx_ir);
    }

    /*
     * The length sweep, which is the question "how long an impulse will this
     * board hold" asked properly.  A real impulse response rather than a
     * preset, so the cap in AG_IR_MAX_MS is what is being measured.
     */
    out("\n  a loaded impulse, by length:\n");
    fx_head();
    {
        static const uint16_t k_ms[] = { 20u, 100u, 250u, 500u, 1000u };
        const uint32_t max_n = (FX_RATE * AG_IR_MAX_MS) / 1000u;
        int16_t *tail = (int16_t *)ag_malloc(sizeof(int16_t) * max_n);
        uint32_t m;

        if (tail == NULL) {
            out("  no room for the impulse itself\n");
        } else {
            uint32_t rng = 777u;
            int32_t  env = 30000;
            uint32_t j;
            tail[0] = 30000;
            for (j = 1; j < max_n; j++) {
                rng = rng * 1664525u + 1013904223u;
                env = (env * 32740) >> 15;
                tail[j] = (int16_t)(((int32_t)(int16_t)(rng >> 16) * env) >>
                                    16);
            }
            for (m = 0; m < sizeof(k_ms) / sizeof(k_ms[0]); m++) {
                const uint32_t n = (FX_RATE * k_ms[m]) / 1000u;
                char           name[16];
                if (ag_ir_init(&s_fx_ir, FX_RATE) != 0) {
                    out("  init failed\n");
                    break;
                }
                if (ag_ir_load(&s_fx_ir, tail, n, FX_RATE) != 0) {
                    outf("  %u ms: no memory for the spectra\n",
                         (unsigned)k_ms[m]);
                    ag_ir_free(&s_fx_ir);
                    continue;
                }
                snprintf(name, sizeof(name), "%u ms", (unsigned)k_ms[m]);
                fx_line(name, s_fx_ir.parts, s_fx_ir.ir_frames,
                        fx_time_ir(&s_fx_ir, blocks));
                ag_ir_free(&s_fx_ir);
            }
            ag_free(tail);
        }
    }

    /* Bypass: the block copy and nothing else, so the numbers above can be
     * read as the convolution rather than as the harness. */
    if (ag_ir_init(&s_fx_ir, FX_RATE) == 0) {
        uint32_t best = 0xffffffffu;
        ag_ir_set_bypass(&s_fx_ir, 1);
        for (rep = 0; rep < FX_REPS; rep++) {
            ag_time_t t0, t1;
            uint32_t  b;
            t0 = ag_micros();
            for (b = 0; b < blocks; b++) {
                ag_ir_process_block(&s_fx_ir, s_fx_mono, s_fx_stereo);
            }
            t1 = ag_micros();
            if ((uint32_t)(t1 - t0) < best) {
                best = (uint32_t)(t1 - t0);
            }
        }
        fx_line("ir bypass", 0, 0, fx_instr(best, blocks));
        ag_ir_free(&s_fx_ir);
    }

    /*
     * The transform on its own, so the line above can be read as "how much of
     * the convolution is the FFT".  Counted as one forward plus one inverse
     * per block, which is what one block of overlap-add costs.
     */
    {
        static int32_t re[AG_IR_FFT], im[AG_IR_FFT];
        uint32_t       best = 0xffffffffu, i;
        for (i = 0; i < AG_IR_FFT; i++) {
            re[i] = i < AG_IR_BLOCK ? ((int32_t)s_fx_mono[i] << 15) : 0;
            im[i] = 0;
        }
        for (rep = 0; rep < FX_REPS; rep++) {
            ag_time_t t0, t1;
            uint32_t  b;
            t0 = ag_micros();
            for (b = 0; b < blocks; b++) {
                (void)ag_fft_real_fwd(re, im, (int)AG_IR_FFT);
                (void)ag_fft_real_inv(re, im, (int)AG_IR_FFT);
            }
            t1 = ag_micros();
            if ((uint32_t)(t1 - t0) < best) {
                best = (uint32_t)(t1 - t0);
            }
        }
        fx_line("fft real pair", 0, AG_IR_FFT, fx_instr(best, blocks));

        best = 0xffffffffu;
        for (rep = 0; rep < FX_REPS; rep++) {
            ag_time_t t0, t1;
            uint32_t  b;
            t0 = ag_micros();
            for (b = 0; b < blocks; b++) {
                (void)ag_fft_cplx_i32(re, im, (int)AG_IR_FFT, 1);
                (void)ag_fft_cplx_i32(re, im, (int)AG_IR_FFT, 0);
            }
            t1 = ag_micros();
            if ((uint32_t)(t1 - t0) < best) {
                best = (uint32_t)(t1 - t0);
            }
        }
        fx_line("fft cplx pair", 0, AG_IR_FFT, fx_instr(best, blocks));
    }

    if (ag_fx_init(&s_fx, FX_RATE) == 0) {
        static const unsigned k_mask[2] = { AG_FX_REVERB, AG_FX_ALL };
        static const char *const k_fxname[2] = { "ag_fx reverb",
                                                 "ag_fx all three" };
        int m;
        ag_fx_set_defaults(&s_fx);
        for (m = 0; m < 2; m++) {
            uint32_t best = 0xffffffffu;
            ag_fx_set_enable(&s_fx, k_mask[m]);
            ag_fx_reset(&s_fx);
            for (rep = 0; rep < FX_REPS; rep++) {
                ag_time_t t0, t1;
                uint32_t  b;
                t0 = ag_micros();
                for (b = 0; b < blocks; b++) {
                    ag_fx_process(&s_fx, s_fx_stereo, (int32_t)AG_IR_BLOCK);
                }
                t1 = ag_micros();
                if ((uint32_t)(t1 - t0) < best) {
                    best = (uint32_t)(t1 - t0);
                }
            }
            /* Stereo: the figure is per output frame, both channels. */
            fx_line(k_fxname[m], 0, 0, fx_instr(best, blocks));
        }
        ag_fx_free(&s_fx);
    } else {
        out("  ag_fx: init failed\n");
    }
}

/* ------------------------------------------------------------------------ */

static uint32_t arg_u(int argc, char **argv, int idx, uint32_t def)
{
    uint32_t v = 0;
    int      i = 0;
    if (argc <= idx) {
        return def;
    }
    while (argv[idx][i] >= '0' && argv[idx][i] <= '9') {
        v = v * 10u + (uint32_t)(argv[idx][i] - '0');
        i++;
    }
    return i ? v : def;
}

int ag_main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "prim";
    int         rc = 0;

    /* argv: MODE [outfile|-] [settle] [meas] [stages] */
    if (argc > 2 && argv[2][0] != '-') {
        s_out = ag_open(argv[2], AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC);
        if (s_out < 0) {
            ag_printf("cannot write %s (%d); console only\n", argv[2],
                      (int)s_out);
        }
    }

    out("CKTBENCH 0.3 - cost of simulating an analogue circuit\n");

    if (mode[0] == 'c') {
        run_check();
    } else if (mode[0] == 'p' && mode[1] == 'r' && mode[2] == 'i') {
        run_prim();
    } else if (mode[0] == 'p') {
        sine_fill();
        run_parts(arg_u(argc, argv, 3, 6000u), arg_u(argc, argv, 4, 192u));
    } else if (mode[0] == 'r') {
        sine_fill();
        run_ramp((int)arg_u(argc, argv, 5, 6u), arg_u(argc, argv, 3, 6000u),
                 arg_u(argc, argv, 4, 192u));
    } else if (mode[0] == 'm') {
        sine_fill();
        run_mono((int)arg_u(argc, argv, 5, 6u), arg_u(argc, argv, 3, 6000u),
                 arg_u(argc, argv, 4, 192u));
    } else if (mode[0] == 'b') {
        sine_fill();
        run_baked((int)arg_u(argc, argv, 5, 6u), arg_u(argc, argv, 4, 192u),
                  arg_u(argc, argv, 6, 0u));
    } else if (mode[0] == 'f') {
        run_fx(arg_u(argc, argv, 3, FX_BLOCKS));
    } else {
        ag_printf("unknown mode '%s'; known: check, prim, parts, ramp, mono, "
                  "baked, fx\n",
                  mode);
        rc = 2;
    }

    if (s_out >= 0) {
        ag_close(s_out);
        s_out = AG_INVALID_HANDLE;
        ag_printf("report written to %s\n", argv[2]);
    }
    return rc;
}
