/*
 * Host listen files for rewritten engines + tube/cabinet.
 *   gcc -O2 -o build/engine_smoke tools/engine_smoke.c ...
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "ag_dsp.h"
#include "ag_dx7.h"
#include "ag_fm.h"
#include "ag_grain.h"
#include "ag_ir.h"
#include "ag_smp.h"
#include "ag_synth.h"

#define RATE 22050u

static void wr_u16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, 2, f);
}

static void wr_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static int write_wav(const char *path, const int16_t *pcm, uint32_t frames)
{
    FILE    *f = fopen(path, "wb");
    uint32_t data = frames * 4u;
    if (f == NULL) {
        fprintf(stderr, "engine_smoke: cannot write %s\n", path);
        return -1;
    }
    fwrite("RIFF", 1, 4, f);
    wr_u32(f, 36u + data);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    wr_u32(f, 16);
    wr_u16(f, 1);
    wr_u16(f, 2);
    wr_u32(f, RATE);
    wr_u32(f, RATE * 4u);
    wr_u16(f, 4);
    wr_u16(f, 16);
    fwrite("data", 1, 4, f);
    wr_u32(f, data);
    fwrite(pcm, 2, frames * 2u, f);
    fclose(f);
    printf("wrote %s (%u ms)\n", path, (unsigned)((frames * 1000u) / RATE));
    return 0;
}

static int16_t *alloc_pcm(uint32_t frames)
{
    int16_t *p = (int16_t *)calloc((size_t)frames * 2u, sizeof(int16_t));
    return p;
}

static void render_dx7(const char *path, int preset, uint8_t note, uint32_t hold,
                       uint32_t tail)
{
    ag_dx7_t dx;
    uint32_t frames = hold + tail;
    int16_t *pcm = alloc_pcm(frames);
    uint32_t i;

    if (pcm == NULL) {
        return;
    }
    ag_dx7_init(&dx, RATE);
    ag_dx7_load_patch(&dx, ag_dx7_preset(preset));
    ag_dx7_set_audition(&dx, 1);
    ag_dx7_note_on(&dx, note, 110);
    for (i = 0; i < frames; i += 256u) {
        uint32_t n = 256u;
        if (i + n > frames) {
            n = frames - i;
        }
        if (i == hold) {
            ag_dx7_note_off(&dx, note);
        }
        ag_dx7_render(&dx, pcm + (int32_t)i * 2, (int32_t)n);
    }
    write_wav(path, pcm, frames);
    free(pcm);
}

static void fill_grain_builtin(int16_t *buf, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t p1 = (int32_t)((i * 220u * 65536u) / RATE);
        int32_t p2 = (int32_t)((i * 330u * 65536u) / RATE);
        int32_t s = ((p1 & 0x7fff) - 16384) + (((p2 & 0x7fff) - 16384) / 2);
        uint32_t env = i < 2000u ? i : (i > n - 4000u ? (n - i) : 2000u);
        s = (s * (int32_t)env) / 2000;
        buf[i] = ag_sat16(s);
    }
}

static void render_grain(const char *path)
{
    enum { BUF_N = 22050 };
    ag_grain_t g;
    int16_t   *sample = (int16_t *)calloc(BUF_N, sizeof(int16_t));
    uint32_t   hold = RATE;
    uint32_t   tail = RATE / 2u;
    uint32_t   frames = hold + tail;
    int16_t   *pcm = alloc_pcm(frames);
    uint32_t   i;

    if (sample == NULL || pcm == NULL) {
        free(sample);
        free(pcm);
        return;
    }
    fill_grain_builtin(sample, BUF_N);
    ag_grain_init(&g, RATE);
    ag_grain_set_defaults(&g);
    ag_grain_buf_set(&g, sample, BUF_N, RATE, 0);
    ag_grain_note_on(&g, 60, 110);
    for (i = 0; i < frames; i += 256u) {
        uint32_t n = 256u;
        if (i + n > frames) {
            n = frames - i;
        }
        if (i == hold) {
            ag_grain_note_off(&g, 60);
        }
        ag_grain_render(&g, pcm + (int32_t)i * 2, (int32_t)n);
    }
    write_wav(path, pcm, frames);
    free(pcm);
    free(sample);
}

static void note_to_fnum(int note, uint16_t *fnum, uint8_t *block)
{
    int     n = note;
    int     b = 4;
    int32_t hz, ref;
    uint32_t f;

    while (n >= 72) {
        b++;
        n -= 12;
    }
    while (n < 60) {
        b--;
        n += 12;
    }
    if (b < 0) {
        b = 0;
    }
    if (b > 7) {
        b = 7;
    }
    hz = ag_dsp_note_hz_x100(n);
    ref = ag_dsp_note_hz_x100(60);
    if (ref < 1) {
        ref = 1;
    }
    f = (uint32_t)((172 * hz) / ref);
    if (f < 1u) {
        f = 1u;
    }
    if (f > 511u) {
        f = 511u;
    }
    *fnum = (uint16_t)f;
    *block = (uint8_t)b;
}

static void render_fm(const char *path)
{
    ag_fm_t  fm;
    uint32_t hold = (RATE * 3u) / 4u;
    uint32_t tail = RATE / 4u;
    uint32_t frames = hold + tail;
    int16_t *pcm = alloc_pcm(frames);
    int16_t *left = (int16_t *)calloc(256, sizeof(int16_t));
    int16_t *right = (int16_t *)calloc(256, sizeof(int16_t));
    uint16_t fnum;
    uint8_t  block;
    uint32_t i, k;

    if (pcm == NULL || left == NULL || right == NULL) {
        free(pcm);
        free(left);
        free(right);
        return;
    }
    ag_fm_init(&fm, 3579545u, RATE);
    ag_fm_set_inst_vol(&fm, 0, 2, 1); /* violin-ish OPLL preset, loud */
    note_to_fnum(64, &fnum, &block);
    ag_fm_set_fnum(&fm, 0, fnum, block);
    ag_fm_set_key(&fm, 0, 1, 0);
    for (i = 0; i < frames; i += 256u) {
        uint32_t n = 256u;
        if (i + n > frames) {
            n = frames - i;
        }
        if (i == hold) {
            ag_fm_set_key(&fm, 0, 0, 0);
        }
        ag_fm_update(&fm, left, right, (int32_t)n);
        for (k = 0; k < n; k++) {
            pcm[(i + k) * 2u] = left[k];
            pcm[(i + k) * 2u + 1u] = right[k];
        }
    }
    write_wav(path, pcm, frames);
    free(pcm);
    free(left);
    free(right);
}

static void apply_ir_blocks(ag_ir_t *ir, int16_t *pcm, uint32_t frames)
{
    int16_t  mono[AG_IR_BLOCK];
    int16_t  st[AG_IR_BLOCK * 2];
    uint32_t i, k;

    for (i = 0; i < frames; i += AG_IR_BLOCK) {
        uint32_t n = AG_IR_BLOCK;
        if (i + n > frames) {
            n = frames - i;
        }
        memset(mono, 0, sizeof(mono));
        for (k = 0; k < n; k++) {
            int32_t l = pcm[(i + k) * 2u];
            int32_t r = pcm[(i + k) * 2u + 1u];
            mono[k] = ag_sat16((l + r) >> 1);
        }
        ag_ir_process_block(ir, mono, st);
        for (k = 0; k < n; k++) {
            pcm[(i + k) * 2u] = st[k * 2u];
            pcm[(i + k) * 2u + 1u] = st[k * 2u + 1u];
        }
    }
}

static void render_tube_cab(const char *path)
{
    ag_synth_t s;
    ag_ir_t    ir;
    uint32_t   hold = RATE;
    uint32_t   tail = RATE / 2u;
    uint32_t   frames = hold + tail;
    int16_t   *pcm = alloc_pcm(frames);
    uint32_t   i;

    if (pcm == NULL) {
        return;
    }
    ag_synth_init(&s, RATE);
    ag_synth_set(&s, AG_SYNTH_P_WAVE1, AG_OSC_SAW);
    ag_synth_set(&s, AG_SYNTH_P_CUTOFF, 86);
    ag_synth_set(&s, AG_SYNTH_P_RESO, 28);
    ag_synth_set(&s, AG_SYNTH_P_DRIVE, 56);
    ag_synth_set(&s, AG_SYNTH_P_BIAS, 68);
    ag_synth_set(&s, AG_SYNTH_P_SAG, 18);
    ag_synth_set(&s, AG_SYNTH_P_DIST_MODEL, AG_DIST_TUBE);
    ag_synth_note_on(&s, 64, 110);
    for (i = 0; i < frames; i += 256u) {
        uint32_t n = 256u;
        if (i + n > frames) {
            n = frames - i;
        }
        if (i == hold) {
            ag_synth_note_off(&s, 64);
        }
        ag_synth_render(&s, pcm + (int32_t)i * 2, (int32_t)n);
    }
    if (ag_ir_init(&ir, RATE) == 0 && ag_ir_load_preset(&ir, 3) == 0) {
        ag_ir_set_wet(&ir, 110);
        ag_ir_set_gain(&ir, 80);
        apply_ir_blocks(&ir, pcm, frames);
        ag_ir_free(&ir);
    }
    write_wav(path, pcm, frames);
    free(pcm);
}

static void render_ir_hall(const char *path)
{
    ag_ir_t  ir;
    uint32_t frames = RATE * 2u;
    int16_t *pcm = alloc_pcm(frames);
    uint32_t i;

    if (pcm == NULL) {
        return;
    }
    /* Dry click + short burst so the hall IR is obvious. */
    pcm[0] = 28000;
    pcm[1] = 28000;
    for (i = 1; i < 40u; i++) {
        int16_t v = (int16_t)(20000 - (int)i * 400);
        pcm[i * 2u] = v;
        pcm[i * 2u + 1u] = v;
    }
    if (ag_ir_init(&ir, RATE) == 0 && ag_ir_load_preset(&ir, 1) == 0) {
        ag_ir_set_wet(&ir, 120);
        apply_ir_blocks(&ir, pcm, frames);
        ag_ir_free(&ir);
    }
    write_wav(path, pcm, frames);
    free(pcm);
}

static void render_smp(const char *path, int preset, uint8_t note)
{
    ag_smp_t     s;
    ag_smp_zone_t z;
    uint32_t     n = ag_smp_preset_frames(preset, RATE);
    int16_t     *rom = (int16_t *)calloc(n, sizeof(int16_t));
    uint32_t     hold = RATE;
    uint32_t     tail = RATE / 2u;
    uint32_t     frames = hold + tail;
    int16_t     *pcm = alloc_pcm(frames);
    uint32_t     i;

    if (rom == NULL || pcm == NULL) {
        free(rom);
        free(pcm);
        return;
    }
    if (ag_smp_fill_preset(preset, rom, n, RATE, &z) != 0) {
        free(rom);
        free(pcm);
        return;
    }
    ag_smp_init(&s, RATE);
    ag_smp_set_zone(&s, &z);
    if (preset == AG_SMP_ORGAN) {
        ag_smp_set_adsr(&s, 8, 20, 120, 30);
    } else if (preset == AG_SMP_BASS) {
        ag_smp_set_adsr(&s, 2, 50, 40, 40);
    } else {
        ag_smp_set_adsr(&s, 2, 50, 80, 40);
    }
    ag_smp_note_on(&s, note, 110);
    for (i = 0; i < frames; i += 256u) {
        uint32_t nfr = 256u;
        if (i + nfr > frames) {
            nfr = frames - i;
        }
        if (i == hold) {
            ag_smp_note_off(&s, note);
        }
        ag_smp_render(&s, pcm + (int32_t)i * 2, (int32_t)nfr);
    }
    write_wav(path, pcm, frames);
    free(pcm);
    free(rom);
}

static void render_wavetable(const char *path)
{
    /* Pretend a recorded WAV note, then cut one cycle — same path as a file. */
    enum { PCM_N = 22050 };
    int16_t   pcm_note[PCM_N];
    int16_t   cycle[1024];
    ag_synth_t s;
    uint32_t   hold = RATE;
    uint32_t   tail = RATE / 2u;
    uint32_t   frames = hold + tail;
    int16_t   *out = alloc_pcm(frames);
    uint32_t   i, n;

    if (out == NULL) {
        return;
    }
    for (i = 0; i < PCM_N; i++) {
        uint32_t ph = (uint32_t)(((uint64_t)i << 32) / (uint64_t)(RATE / 262u));
        int32_t  acc = (int32_t)ag_dsp_sin(ph) +
                      ((int32_t)ag_dsp_sin(ph * 2u) * 3) / 4 +
                      ((int32_t)ag_dsp_sin(ph * 3u) / 2) +
                      ((int32_t)ag_dsp_sin(ph * 5u) / 3);
        pcm_note[i] = ag_sat16(acc);
    }
    n = ag_osc_cycle_from_pcm(cycle, 1024u, pcm_note, PCM_N, RATE, 60);
    ag_synth_init(&s, RATE);
    ag_synth_set_wavetable(&s, cycle, n);
    ag_synth_set(&s, AG_SYNTH_P_WAVE1, AG_OSC_WT);
    ag_synth_set(&s, AG_SYNTH_P_WAVE2, AG_OSC_WT);
    ag_synth_set(&s, AG_SYNTH_P_CUTOFF, 100);
    ag_synth_set(&s, AG_SYNTH_P_DRIVE, 0);
    ag_synth_note_on(&s, 60, 110);
    for (i = 0; i < frames; i += 256u) {
        uint32_t nfr = 256u;
        if (i + nfr > frames) {
            nfr = frames - i;
        }
        if (i == hold) {
            ag_synth_note_off(&s, 60);
        }
        ag_synth_render(&s, out + (int32_t)i * 2, (int32_t)nfr);
    }
    write_wav(path, out, frames);
    free(out);
}

int main(int argc, char **argv)
{
    const char *dir = "build/listen";
    char        path[256];

    (void)argc;
    (void)argv;
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0777);
#endif

    snprintf(path, sizeof(path), "%s/dx7_epiano.wav", dir);
    render_dx7(path, 1, 64, RATE, RATE / 2u);
    snprintf(path, sizeof(path), "%s/dx7_brass.wav", dir);
    render_dx7(path, 2, 60, RATE, RATE / 2u);
    snprintf(path, sizeof(path), "%s/dx7_bell.wav", dir);
    render_dx7(path, 3, 72, (RATE * 3u) / 4u, RATE);
    snprintf(path, sizeof(path), "%s/grain.wav", dir);
    render_grain(path);
    snprintf(path, sizeof(path), "%s/fm_opll.wav", dir);
    render_fm(path);
    snprintf(path, sizeof(path), "%s/tube_cab.wav", dir);
    render_tube_cab(path);
    snprintf(path, sizeof(path), "%s/ir_hall.wav", dir);
    render_ir_hall(path);
    snprintf(path, sizeof(path), "%s/smp_organ.wav", dir);
    render_smp(path, AG_SMP_ORGAN, 60);
    snprintf(path, sizeof(path), "%s/smp_piano.wav", dir);
    render_smp(path, AG_SMP_PIANO, 67);
    snprintf(path, sizeof(path), "%s/smp_bass.wav", dir);
    render_smp(path, AG_SMP_BASS, 52);
    snprintf(path, sizeof(path), "%s/osc_wavetable.wav", dir);
    render_wavetable(path);
    return 0;
}
