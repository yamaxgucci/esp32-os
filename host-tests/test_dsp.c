/*
 * ArgonOS - host tests for the shared audio DSP layer (apps/common).
 *
 * These are not "does it compile" tests.  Every check here corresponds to a
 * defect that shipped because nothing outside the kernel was ever executed on
 * the host: an oscillator with a full-scale DC step, an FM matrix whose
 * modulation index was off by 2^16, an envelope driven at two different tick
 * rates by two different callers, and an LFO triangle running four times too
 * fast.  All four measure as numbers, so they belong here rather than in a
 * listening file.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

#include <stdlib.h>

#include "ag_dsp.h"
#include "ag_fmx.h"
#include "ag_ir.h"
#include "ag_osc.h"
#include "ag_smp.h"
#include "ag_synth.h"

#define RATE 22050u

static void osc_stats(uint8_t wave, int32_t *mn, int32_t *mx, int32_t *mean)
{
    ag_osc_t o;
    int64_t  sum = 0;
    int      i;
    const int n = (int)RATE;

    memset(&o, 0, sizeof(o));
    o.wave = wave;
    o.pwm = 64;
    ag_osc_set_hz_x100(&o, 26200, RATE); /* ~262 Hz, whole cycles either way */
    *mn = 1 << 30;
    *mx = -(1 << 30);
    for (i = 0; i < n; i++) {
        const int32_t s = ag_osc_tick(&o);
        if (s < *mn) {
            *mn = s;
        }
        if (s > *mx) {
            *mx = s;
        }
        sum += s;
    }
    *mean = (int32_t)(sum / n);
}

/* Every VA wave must sit inside int16 and be centred: a wave with a DC
 * pedestal eats the headroom of everything downstream and thumps on note-on. */
static void test_osc_range(void)
{
    static const uint8_t waves[] = { AG_OSC_SAW, AG_OSC_SQR, AG_OSC_TRI,
                                     AG_OSC_SIN };
    unsigned k;

    for (k = 0; k < sizeof(waves) / sizeof(waves[0]); k++) {
        int32_t mn = 0, mx = 0, mean = 0;
        osc_stats(waves[k], &mn, &mx, &mean);
        AG_CHECK(mn >= -32768 && mn < -30000);
        AG_CHECK(mx <= 32768 && mx > 30000);
        AG_CHECK(mean > -600 && mean < 600);
    }
}

/* The saw is the default wave, so its scaling is the one that matters most:
 * one unit of the phase ramp is one unit of output, not two. */
static void test_osc_saw_scaling(void)
{
    int32_t mn = 0, mx = 0, mean = 0;
    osc_stats(AG_OSC_SAW, &mn, &mx, &mean);
    AG_CHECK(mx - mn > 60000);   /* full swing */
    AG_CHECK(mx - mn <= 66000);  /* and not double it */
    AG_CHECK(mean > -400 && mean < 400);
}

/*
 * Rising zero crossings over two seconds, so the count is the frequency
 * doubled.  Whether the crossing exactly at phase 0 falls inside the window
 * depends on the wave, hence the +-1 below; a wave running at the wrong
 * multiple of the requested rate is off by a factor, not by one.
 */
static int lfo_crossings_2s(uint8_t wave, int32_t hz_x100)
{
    ag_dsp_lfo_t l;
    int          i, zc = 0, prev = 1;

    memset(&l, 0, sizeof(l));
    l.wave = wave;
    ag_dsp_lfo_set_hz_x100(&l, hz_x100, RATE);
    for (i = 0; i < (int)RATE * 2; i++) {
        const int cur = ag_dsp_lfo_tick(&l) >= 0;
        if (cur && !prev) {
            zc++;
        }
        prev = cur;
    }
    return zc;
}

static void check_lfo_hz(uint8_t wave, int32_t hz_x100, int want_hz)
{
    const int got = lfo_crossings_2s(wave, hz_x100);
    const int want = want_hz * 2;
    ag_test_checks++;
    if (got < want - 1 || got > want + 1) {
        ag_test_failures++;
        printf("FAIL %s:%d  lfo wave %u: %d crossings in 2 s, want %d\n",
               __FILE__, __LINE__, (unsigned)wave, got, want);
    }
}

static void test_lfo_rate(void)
{
    /* All five waves have to agree on what 4 Hz means. */
    check_lfo_hz(AG_DSP_LFO_TRI, 400, 4);
    check_lfo_hz(AG_DSP_LFO_SAWDN, 400, 4);
    check_lfo_hz(AG_DSP_LFO_SAWUP, 400, 4);
    check_lfo_hz(AG_DSP_LFO_SQR, 400, 4);
    check_lfo_hz(AG_DSP_LFO_SIN, 400, 4);
    check_lfo_hz(AG_DSP_LFO_TRI, 1000, 10);
}

/* Milliseconds from note-on until the envelope reaches full level. */
static int adsr_attack_ms(uint8_t a, uint32_t rate, uint32_t n)
{
    ag_dsp_adsr_t e;
    uint32_t      ticks = 0;

    memset(&e, 0, sizeof(e));
    ag_dsp_adsr_set_rate(&e, rate);
    ag_dsp_adsr_on(&e);
    while (e.stage == 0 && ticks < rate * 20u) {
        ag_dsp_adsr_tick_n(&e, a, 127, 127, 127, 1, n);
        ticks++;
    }
    return (int)(((uint64_t)ticks * n * 1000u) / rate);
}

static int adsr_release_ms(uint8_t r, uint32_t rate, uint32_t n)
{
    ag_dsp_adsr_t e;
    uint32_t      ticks = 0;

    memset(&e, 0, sizeof(e));
    ag_dsp_adsr_set_rate(&e, rate);
    ag_dsp_adsr_on(&e);
    while (e.stage < 2u && ticks < rate * 20u) {
        ag_dsp_adsr_tick_n(&e, 0, 0, 127, r, 1, n);
        ticks++;
    }
    ag_dsp_adsr_off(&e);
    ticks = 0;
    while (ag_dsp_adsr_tick_n(&e, 0, 0, 127, r, 0, n) && ticks < rate * 20u) {
        ticks++;
    }
    return (int)(((uint64_t)ticks * n * 1000u) / rate);
}

static void test_adsr_times(void)
{
    /* A knob at the top has to give a usable envelope, not a click. */
    AG_CHECK(adsr_attack_ms(127, RATE, 1) > 2000);
    AG_CHECK(adsr_attack_ms(127, RATE, 1) < 4000);
    AG_CHECK(adsr_release_ms(127, RATE, 1) > 3000);
    AG_CHECK(adsr_release_ms(127, RATE, 1) < 7000);
    /* And a knob at the bottom still has to be quick. */
    AG_CHECK(adsr_attack_ms(0, RATE, 1) < 20);
    /* Monotonic in the knob. */
    AG_CHECK(adsr_attack_ms(100, RATE, 1) > adsr_attack_ms(50, RATE, 1));
    AG_CHECK(adsr_attack_ms(50, RATE, 1) > adsr_attack_ms(10, RATE, 1));
}

/*
 * The same envelope is ticked once per sample by the synths and once per
 * block by ag_grain.  Both must take the same wall-clock time, or the knob
 * means two different things depending on who is holding it.
 */
static void test_adsr_tick_rate_independent(void)
{
    const int per_sample = adsr_attack_ms(100, RATE, 1);
    const int per_block = adsr_attack_ms(100, RATE, 256);
    const int per_big_block = adsr_attack_ms(100, RATE, 1024);
    const int at_44k = adsr_attack_ms(100, 44100u, 1);

    AG_CHECK(abs(per_sample - per_block) < per_sample / 8 + 5);
    AG_CHECK(abs(per_sample - per_big_block) < per_sample / 8 + 20);
    AG_CHECK(abs(per_sample - at_44k) < per_sample / 8 + 5);
}

/* Sustain is a level, and the decay has to actually reach it. */
static void test_adsr_sustain(void)
{
    ag_dsp_adsr_t e;
    int           i;

    memset(&e, 0, sizeof(e));
    ag_dsp_adsr_set_rate(&e, RATE);
    ag_dsp_adsr_on(&e);
    for (i = 0; i < (int)RATE * 8; i++) {
        if (!ag_dsp_adsr_tick(&e, 0, 0, 64, 64, 1)) {
            break;
        }
        if (e.stage == 2u) {
            break;
        }
    }
    AG_CHECK_INT(e.stage, 2);
    AG_CHECK(e.level >= 125 && e.level <= 132); /* 64/127 of 256 */
}

/* Voice stealing takes the oldest note, never the one just played. */
static void test_voice_steal(void)
{
    uint32_t born[4] = { 10u, 40u, 20u, 30u };
    uint8_t  active[4] = { 1u, 1u, 1u, 1u };
    uint8_t  one_free[4] = { 1u, 1u, 0u, 1u };

    AG_CHECK_INT(ag_dsp_voice_steal(born, active, 4), 0);
    AG_CHECK_INT(ag_dsp_voice_steal(born, one_free, 4), 2);
}

static double fmx_rms(uint8_t depth)
{
    ag_fmx_t f;
    int64_t  e2 = 0;
    int      i;
    const int n = 4410;

    ag_fmx_reset(&f);
    ag_fmx_set_n(&f, 2);
    ag_fmx_algo_stack(&f);
    f.fb = 0;
    f.route[0][1] = depth;
    ag_fmx_set_hz(&f, 26200, RATE);
    ag_fmx_note_on(&f);
    for (i = 0; i < n; i++) {
        const int32_t s = ag_fmx_tick(&f, 0, 127, 127, 127, 1);
        e2 += (int64_t)s * s;
    }
    return (double)e2 / (double)n;
}

/*
 * With the carrier held at a fixed level, turning the routing depth up must
 * change the waveform.  It did not: mod was added to a 32-bit phase without
 * being scaled into it, so the deepest possible modulation moved the phase by
 * about a millionth of a cycle and every "FM" patch was a plain sine.
 */
static void test_fmx_index_changes_timbre(void)
{
    ag_fmx_t f;
    int64_t  diff = 0;
    int32_t  a[512], b[512];
    int      i;

    /* Same carrier, two depths, sample by sample. */
    for (i = 0; i < 2; i++) {
        int32_t *dst = i == 0 ? a : b;
        int      k;
        ag_fmx_reset(&f);
        ag_fmx_set_n(&f, 2);
        ag_fmx_algo_stack(&f);
        f.fb = 0;
        f.route[0][1] = i == 0 ? 0u : 90u;
        ag_fmx_set_hz(&f, 26200, RATE);
        ag_fmx_note_on(&f);
        for (k = 0; k < 512; k++) {
            dst[k] = ag_fmx_tick(&f, 0, 127, 127, 127, 1);
        }
    }
    for (i = 0; i < 512; i++) {
        const int32_t d = a[i] - b[i];
        diff += (int64_t)(d < 0 ? -d : d);
    }
    /* Deep FM against none: the two must be nothing like each other. */
    AG_CHECK(diff / 512 > 2000);

    /* And the modulation must not be so strong that it is pure noise. */
    AG_CHECK(fmx_rms(0) > 1000.0);
    AG_CHECK(fmx_rms(90) > 1000.0);
}

/* A rendered note must be centred and must fit: DC or clipping here means an
 * oscillator or a gain stage is wrong somewhere upstream. */
static void test_synth_render_sane(void)
{
    ag_synth_t *s = (ag_synth_t *)calloc(1, sizeof(ag_synth_t));
    int16_t    *pcm = (int16_t *)calloc(RATE * 2u, sizeof(int16_t));
    int64_t     sum = 0;
    int32_t     peak = 0;
    unsigned    i, clipped = 0;

    if (s == NULL || pcm == NULL) {
        free(s);
        free(pcm);
        AG_CHECK(0);
        return;
    }
    ag_synth_init(s, RATE);
    ag_synth_set(s, AG_SYNTH_P_WAVE1, AG_OSC_SAW);
    ag_synth_note_on(s, 60, 110);
    for (i = 0; i < RATE; i += 256u) {
        const uint32_t n = (i + 256u > RATE) ? (RATE - i) : 256u;
        ag_synth_render(s, pcm + (size_t)i * 2u, (int32_t)n);
    }
    for (i = 0; i < RATE; i++) {
        const int32_t v = pcm[i * 2u];
        sum += v;
        if (v > peak) {
            peak = v;
        }
        if (v >= 32700 || v <= -32700) {
            clipped++;
        }
    }
    AG_CHECK(peak > 500);                          /* it makes a sound */
    AG_CHECK(clipped * 100u < RATE);               /* under 1% at the rail */
    AG_CHECK(sum / (int64_t)RATE > -700);          /* and it is centred */
    AG_CHECK(sum / (int64_t)RATE < 700);
    free(s);
    free(pcm);
}

/* The sampler holds a note for as long as the envelope says, not one tick. */
static void test_smp_holds_note(void)
{
    ag_smp_t     *s = (ag_smp_t *)calloc(1, sizeof(ag_smp_t));
    ag_smp_zone_t z;
    uint32_t      n;
    int16_t      *rom;
    int16_t      *pcm;
    unsigned      i, loud_after_half_a_second = 0;

    if (s == NULL) {
        AG_CHECK(0);
        return;
    }
    n = ag_smp_preset_frames(AG_SMP_ORGAN, RATE);
    rom = (int16_t *)calloc(n, sizeof(int16_t));
    pcm = (int16_t *)calloc(RATE * 2u, sizeof(int16_t));
    if (rom == NULL || pcm == NULL ||
        ag_smp_fill_preset(AG_SMP_ORGAN, rom, n, RATE, &z) != 0) {
        free(s);
        free(rom);
        free(pcm);
        AG_CHECK(0);
        return;
    }
    ag_smp_init(s, RATE);
    ag_smp_set_zone(s, &z);
    ag_smp_set_adsr(s, 8, 20, 120, 30);
    ag_smp_note_on(s, 60, 110);
    for (i = 0; i < RATE; i += 256u) {
        const uint32_t blk = (i + 256u > RATE) ? (RATE - i) : 256u;
        ag_smp_render(s, pcm + (size_t)i * 2u, (int32_t)blk);
    }
    for (i = RATE / 2u; i < RATE; i++) {
        if (pcm[i * 2u] > 500 || pcm[i * 2u] < -500) {
            loud_after_half_a_second++;
        }
    }
    AG_CHECK(loud_after_half_a_second > RATE / 8u);
    free(s);
    free(rom);
    free(pcm);
}

/* Sine through the IR at a given input level: wet only, so what is measured
 * is the convolution and nothing else. */
static void ir_measure(int preset, int div, double *rms, int *jumps)
{
    static int16_t mono[AG_IR_BLOCK];
    static int16_t st[AG_IR_BLOCK * 2];
    ag_ir_t  ir;
    uint32_t phase = 0;
    const uint32_t step = ag_dsp_hz_to_step(33000, RATE);
    int      b, k;
    int32_t  prev = 0;
    int64_t  e2 = 0;
    long     n = 0;

    *rms = 0.0;
    *jumps = 0;
    if (ag_ir_init(&ir, RATE) != 0 || ag_ir_load_preset(&ir, preset) != 0) {
        return;
    }
    ag_ir_set_wet(&ir, 127);
    for (b = 0; b < 40; b++) {
        for (k = 0; k < (int)AG_IR_BLOCK; k++) {
            mono[k] = (int16_t)(ag_dsp_sin(phase) / div);
            phase += step;
        }
        ag_ir_process_block(&ir, mono, st);
        if (b < 8) {
            continue; /* let the tail fill */
        }
        for (k = 0; k < (int)AG_IR_BLOCK; k++) {
            const int32_t v = st[k * 2];
            const int32_t d = v - prev;
            if (d > 32767 || d < -32767) {
                (*jumps)++;
            }
            prev = v;
            e2 += (int64_t)v * v;
            n++;
        }
    }
    if (n > 0) {
        double mean = (double)e2 / (double)n;
        double r = mean, prev_r = 0.0;
        int    it;
        for (it = 0; it < 40 && r != prev_r; it++) { /* isqrt, no libm here */
            prev_r = r;
            r = 0.5 * (r + mean / (r > 1.0 ? r : 1.0));
        }
        *rms = r;
    }
    ag_ir_free(&ir);
}

/*
 * Convolution is a linear filter: halve the input and the output halves.  It
 * did not - the wet signal was pinned to the rail whatever went in, because
 * the IR was normalised by its peak instead of its energy and the spectra
 * were stored with a fixed shift that a 500 ms tail cannot fit.
 */
static void test_ir_linear(void)
{
    static const int presets[] = { 0, 1, 3 };
    unsigned p;

    for (p = 0; p < sizeof(presets) / sizeof(presets[0]); p++) {
        double loud = 0.0, quiet = 0.0;
        int    j_loud = 0, j_quiet = 0;

        ir_measure(presets[p], 1, &loud, &j_loud);
        ir_measure(presets[p], 4, &quiet, &j_quiet);

        AG_CHECK(loud > 200.0);       /* it does something */
        AG_CHECK(loud < 26000.0);     /* and does not sit on the rail */
        AG_CHECK_INT(j_loud, 0);      /* no full-scale steps between samples */
        AG_CHECK_INT(j_quiet, 0);
        /* Four times less in, about four times less out. */
        AG_CHECK(quiet * 3.0 < loud);
        AG_CHECK(quiet * 6.0 > loud);
    }
}

/* Loading the same preset twice must give the same room. */
static void test_ir_repeatable(void)
{
    double a = 0.0, b = 0.0;
    int    ja = 0, jb = 0;

    ir_measure(1, 1, &a, &ja);
    ir_measure(1, 1, &b, &jb);
    AG_CHECK(a > 0.0);
    AG_CHECK(a == b);
}

void run_dsp_tests(void)
{
    test_osc_range();
    test_osc_saw_scaling();
    test_lfo_rate();
    test_adsr_times();
    test_adsr_tick_rate_independent();
    test_adsr_sustain();
    test_voice_steal();
    test_fmx_index_changes_timbre();
    test_synth_render_sane();
    test_smp_holds_note();
    test_ir_linear();
    test_ir_repeatable();
}
