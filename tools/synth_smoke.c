/*
 * Host smoke: render ag_synth to a WAV (no Argon runtime).
 *   gcc -O2 -o build/synth_smoke tools/synth_smoke.c \
 *       apps/common/dsp/ag_dsp.c apps/common/synth/ag_osc.c \
 *       apps/common/synth/ag_filt.c apps/common/synth/ag_dist.c \
 *       apps/common/synth/ag_fmx.c apps/common/synth/ag_synth.c \
 *       -I apps/common/dsp -I apps/common/synth
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ag_synth.h"

#define RATE   22050u
#define SECS   2u
#define FRAMES (RATE * SECS)

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
    return 0;
}

int main(int argc, char **argv)
{
    static int16_t pcm[FRAMES * 2];
    ag_synth_t     s;
    const char    *out = "build/synth_smoke.wav";
    uint32_t       i;

    if (argc > 1) {
        out = argv[1];
    }
    ag_synth_init(&s, RATE);
    ag_synth_set(&s, AG_SYNTH_P_WAVE1, AG_OSC_SAW);
    ag_synth_set(&s, AG_SYNTH_P_CUTOFF, 72);
    ag_synth_set(&s, AG_SYNTH_P_RESO, 50);
    ag_synth_set(&s, AG_SYNTH_P_DRIVE, 48);
    ag_synth_set(&s, AG_SYNTH_P_DIST_MODEL, AG_DIST_TUBE);
    ag_synth_mod_bind(&s, AG_SYNTH_SRC_LFO1, AG_SYNTH_P_CUTOFF, 60);
    ag_synth_note_on(&s, 60, 110);
    for (i = 0; i < FRAMES; i += 256u) {
        uint32_t n = 256u;
        if (i + n > FRAMES) {
            n = FRAMES - i;
        }
        if (i == RATE) {
            ag_synth_note_off(&s, 60);
            ag_synth_set(&s, AG_SYNTH_P_ENGINE, AG_SYNTH_FM);
            ag_synth_note_on(&s, 64, 110);
        }
        ag_synth_render(&s, pcm + (int32_t)i * 2, (int32_t)n);
    }
    if (write_wav(out, pcm, FRAMES) != 0) {
        fprintf(stderr, "synth_smoke: cannot write %s\n", out);
        return 1;
    }
    printf("wrote %s (%u frames, VA then FM)\n", out, (unsigned)FRAMES);
    return 0;
}
