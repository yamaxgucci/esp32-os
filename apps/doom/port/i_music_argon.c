/*
 * ArgonOS Doom music: MUS → MIDI → Nuked OPL3 + IWAD GENMIDI (AdLib).
 * SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <string.h>

#include "doomgeneric_argon.h"
#include "i_sound.h"
#include "i_swap.h"
#include "memio.h"
#include "mus2mid.h"
#include "opl3.h"
#include "w_wad.h"
#include "z_zone.h"

#define MUS_RATE     11025u
#define MUS_EVMAX    4096
#define MIDI_DRUM    9
#define MUS_MIXMAX   1024
#define OPL_VOICES   18
#define OPL_CH       9

#define GENMIDI_NUM_INSTRS     128
#define GENMIDI_NUM_PERCUSSION 47
#define GENMIDI_HEADER         "#OPL_II#"
#define GENMIDI_FLAG_FIXED     0x0001
#define GENMIDI_FLAG_2VOICE    0x0004

#define OPL_REGS_TREMOLO  0x20
#define OPL_REGS_LEVEL    0x40
#define OPL_REGS_ATTACK   0x60
#define OPL_REGS_SUSTAIN  0x80
#define OPL_REGS_FREQ_1   0xA0
#define OPL_REGS_FREQ_2   0xB0
#define OPL_REGS_FEEDBACK 0xC0
#define OPL_REGS_WAVEFORM 0xE0

typedef struct {
    uint32_t tick;
    uint32_t tempo;
    uint8_t  status;
    uint8_t  a;
    uint8_t  b;
} mev_t;

typedef struct {
    mev_t   *ev;
    int      nev;
    unsigned division;
} song_t;

typedef struct __attribute__((packed)) {
    uint8_t tremolo;
    uint8_t attack;
    uint8_t sustain;
    uint8_t waveform;
    uint8_t scale;
    uint8_t level;
} genmidi_op_t;

typedef struct __attribute__((packed)) {
    genmidi_op_t modulator;
    uint8_t      feedback;
    genmidi_op_t carrier;
    uint8_t      unused;
    int16_t      base_note_offset;
} genmidi_voice_t;

typedef struct __attribute__((packed)) {
    uint16_t       flags;
    uint8_t        fine_tuning;
    uint8_t        fixed_note;
    genmidi_voice_t voices[2];
} genmidi_instr_t;

typedef struct {
    int                    used;
    int                    ch;
    int                    key;
    int                    note;
    int                    age;
    int                    index;
    int                    array;
    int                    op1;
    int                    op2;
    unsigned               instr_voice;
    const genmidi_instr_t *instr;
    unsigned               car_volume;
    unsigned               mod_volume;
} ov_t;

static snddevice_t k_mdevs[] = {
    SNDDEVICE_SB, SNDDEVICE_ADLIB, SNDDEVICE_PAS, SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

static const int k_op[2][OPL_CH] = {
    {0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12},
    {0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15},
};

static const uint16_t k_fn[12] = {
    0x157, 0x16b, 0x181, 0x198, 0x1b0, 0x1ca,
    0x1e5, 0x202, 0x220, 0x241, 0x263, 0x287,
};

/* Chocolate Doom DMX volume curve (MIDI 0–127 → 0–127). */
static const unsigned k_volmap[128] = {
    0,   1,   3,   5,   6,   8,   10,  11,  13,  14,  16,  17,  19,  20,  22,
    23,  25,  26,  27,  29,  30,  32,  33,  34,  36,  37,  39,  41,  43,  45,
    47,  49,  50,  52,  54,  55,  57,  59,  60,  61,  63,  64,  66,  67,  68,
    69,  71,  72,  73,  74,  75,  76,  77,  79,  80,  81,  82,  83,  84,  84,
    85,  86,  87,  88,  89,  90,  91,  92,  92,  93,  94,  95,  96,  96,  97,
    98,  99,  99,  100, 101, 101, 102, 103, 103, 104, 105, 105, 106, 107, 107,
    108, 109, 109, 110, 110, 111, 112, 112, 113, 113, 114, 114, 115, 115, 116,
    117, 117, 118, 118, 119, 119, 120, 120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127};

static opl3_chip        s_opl;
static int              s_opl_on;
static genmidi_instr_t *s_melodic;
static genmidi_instr_t *s_drum;
static song_t          *s_song;
static int              s_playing;
static int              s_looping;
static int              s_paused;
static int              s_evi;
static uint32_t         s_tick;
static uint32_t         s_tempo = 500000u;
static uint64_t         s_acc;
static int              s_vol = 64;
static uint8_t          s_prog[16];
static uint8_t          s_cc7[16];
static ov_t             s_vc[OPL_VOICES];
static uint32_t         s_age;
static int16_t          s_mix[MUS_MIXMAX * 2];

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr(uint16_t reg, uint8_t v)
{
    OPL3_WriteReg(&s_opl, reg, v);
}

static void load_op(int op, const genmidi_op_t *data, int max_level,
                    unsigned *volume)
{
    int level = data->scale;
    if (max_level) {
        level |= 0x3f;
    } else {
        level |= data->level;
    }
    *volume = (unsigned)level;
    wr((uint16_t)(OPL_REGS_LEVEL + op), (uint8_t)level);
    wr((uint16_t)(OPL_REGS_TREMOLO + op), data->tremolo);
    wr((uint16_t)(OPL_REGS_ATTACK + op), data->attack);
    wr((uint16_t)(OPL_REGS_SUSTAIN + op), data->sustain);
    wr((uint16_t)(OPL_REGS_WAVEFORM + op), data->waveform);
}

static void set_instrument(ov_t *v, const genmidi_instr_t *instr,
                           unsigned instr_voice)
{
    const genmidi_voice_t *data;
    int                    modulating;

    v->instr = instr;
    v->instr_voice = instr_voice;
    data = &instr->voices[instr_voice];
    modulating = (data->feedback & 0x01) == 0;
    load_op(v->op2 | v->array, &data->carrier, 1, &v->car_volume);
    load_op(v->op1 | v->array, &data->modulator, !modulating, &v->mod_volume);
    wr((uint16_t)((OPL_REGS_FEEDBACK + v->index) | v->array),
       (uint8_t)(data->feedback | 0x30));
}

static unsigned car_tl(int ch, int vel)
{
    unsigned note_v = (unsigned)vel;
    unsigned ch_v = s_cc7[ch];
    unsigned midi_volume;
    unsigned full;

    if (note_v > 127u) {
        note_v = 127u;
    }
    if (ch_v > 127u) {
        ch_v = 127u;
    }
    midi_volume = 2u * (k_volmap[ch_v] + 1u);
    full = (k_volmap[note_v] * midi_volume) >> 9;
    full = (full * (unsigned)s_vol) / 127u;
    if (full > 0x3fu) {
        full = 0x3fu;
    }
    return 0x3fu - full;
}

static void set_voice_volume(ov_t *v, int vel)
{
    const genmidi_voice_t *data;
    unsigned               car;
    unsigned               mod;

    if (v->instr == NULL) {
        return;
    }
    data = &v->instr->voices[v->instr_voice];
    car = car_tl(v->ch, vel);
    v->car_volume = car | (v->car_volume & 0xc0u);
    wr((uint16_t)((OPL_REGS_LEVEL + v->op2) | v->array), (uint8_t)v->car_volume);
    if ((data->feedback & 0x01) != 0 && data->modulator.level != 0x3f) {
        mod = data->modulator.level;
        if (mod < car) {
            mod = car;
        }
        mod |= v->mod_volume & 0xc0u;
        v->mod_volume = mod;
        wr((uint16_t)((OPL_REGS_LEVEL + v->op1) | v->array),
           (uint8_t)(mod | (data->modulator.scale & 0xc0u)));
    }
}

static uint16_t fnum_for(const ov_t *v)
{
    int      note = v->note;
    int      block;
    uint16_t fn;
    const genmidi_voice_t *gv;

    if (v->instr != NULL) {
        gv = &v->instr->voices[v->instr_voice];
        if ((SHORT(v->instr->flags) & GENMIDI_FLAG_FIXED) == 0) {
            note += (int)SHORT(gv->base_note_offset);
        }
        if (v->instr_voice != 0) {
            note += ((int)v->instr->fine_tuning / 32) - 4;
        }
    }
    if (note < 0) {
        note = 0;
    }
    if (note > 95) {
        note = 95;
    }
    block = note / 12 - 1;
    if (block < 0) {
        block = 0;
    }
    if (block > 7) {
        block = 7;
    }
    fn = k_fn[note % 12];
    return (uint16_t)(fn | ((unsigned)block << 10));
}

static void key_off_voice(ov_t *v)
{
    uint16_t freq = fnum_for(v);
    wr((uint16_t)((OPL_REGS_FREQ_2 + v->index) | v->array),
       (uint8_t)(freq >> 8));
    v->used = 0;
}

static void key_on_voice(ov_t *v)
{
    uint16_t freq = fnum_for(v);
    wr((uint16_t)((OPL_REGS_FREQ_1 + v->index) | v->array),
       (uint8_t)(freq & 0xffu));
    wr((uint16_t)((OPL_REGS_FREQ_2 + v->index) | v->array),
       (uint8_t)((freq >> 8) | 0x20u));
}

static int steal_voice(void)
{
    int      i;
    int      best = 0;
    uint32_t oldest = 0xffffffffu;

    for (i = 0; i < OPL_VOICES; i++) {
        if (!s_vc[i].used) {
            return i;
        }
    }
    for (i = 0; i < OPL_VOICES; i++) {
        if ((uint32_t)s_vc[i].age < oldest) {
            oldest = (uint32_t)s_vc[i].age;
            best = i;
        }
    }
    key_off_voice(&s_vc[best]);
    return best;
}

static const genmidi_instr_t *instr_for(int ch, int note)
{
    if (s_melodic == NULL) {
        return NULL;
    }
    if (ch == MIDI_DRUM) {
        if (note < 35 || note > 81) {
            return NULL;
        }
        return &s_drum[note - 35];
    }
    return &s_melodic[s_prog[ch] & 127];
}

static void voice_off(int ch, int note)
{
    int i;
    for (i = 0; i < OPL_VOICES; i++) {
        if (s_vc[i].used && s_vc[i].ch == ch && s_vc[i].key == note) {
            key_off_voice(&s_vc[i]);
        }
    }
}

static void voice_on_one(int ch, int key, int note, int vel,
                         const genmidi_instr_t *instr, unsigned instr_voice)
{
    ov_t *v;
    int   slot;

    slot = steal_voice();
    v = &s_vc[slot];
    v->used = 1;
    v->ch = ch;
    v->key = key;
    v->note = note;
    v->age = (int)++s_age;
    set_instrument(v, instr, instr_voice);
    set_voice_volume(v, vel);
    key_on_voice(v);
}

static void voice_on(int ch, int note, int vel)
{
    const genmidi_instr_t *instr;
    int                    play_note;

    if (vel <= 0) {
        voice_off(ch, note);
        return;
    }
    instr = instr_for(ch, note);
    if (instr == NULL) {
        return;
    }
    if (ch == MIDI_DRUM) {
        play_note = (SHORT(instr->flags) & GENMIDI_FLAG_FIXED)
                        ? instr->fixed_note
                        : 60;
    } else if (SHORT(instr->flags) & GENMIDI_FLAG_FIXED) {
        play_note = instr->fixed_note;
    } else {
        play_note = note;
    }
    voice_on_one(ch, note, play_note, vel, instr, 0);
    if (SHORT(instr->flags) & GENMIDI_FLAG_2VOICE) {
        voice_on_one(ch, note, play_note, vel, instr, 1);
    }
}

static void all_off(void)
{
    int i;
    for (i = 0; i < OPL_VOICES; i++) {
        if (s_vc[i].used) {
            key_off_voice(&s_vc[i]);
        }
        s_vc[i].used = 0;
    }
}

static void apply_ev(const mev_t *e)
{
    uint8_t cmd;
    int     ch;
    if (e->status == 0xffu && e->a == 0x51u) {
        s_tempo = e->tempo ? e->tempo : 500000u;
        return;
    }
    cmd = (uint8_t)(e->status & 0xf0u);
    ch = e->status & 0x0f;
    if (cmd == 0x90u) {
        voice_on(ch, e->a, e->b);
    } else if (cmd == 0x80u) {
        voice_off(ch, e->a);
    } else if (cmd == 0xc0u) {
        s_prog[ch] = e->a;
    } else if (cmd == 0xb0u && e->a == 7) {
        s_cc7[ch] = e->b;
    } else if (cmd == 0xb0u && e->a == 0x7bu) {
        all_off();
    }
}

static void dispatch_to(uint32_t tick)
{
    if (s_song == NULL) {
        return;
    }
    while (s_evi < s_song->nev && s_song->ev[s_evi].tick <= tick) {
        apply_ev(&s_song->ev[s_evi]);
        s_evi++;
    }
    if (s_evi >= s_song->nev && s_looping) {
        all_off();
        s_evi = 0;
        s_tick = 0;
        s_acc = 0;
        s_tempo = 500000u;
    } else if (s_evi >= s_song->nev) {
        s_playing = 0;
        all_off();
    }
}

static int read_vlq(const uint8_t *p, int *i, int end, uint32_t *out)
{
    uint32_t v = 0;
    int      n = 0;
    while (*i < end && n < 4) {
        uint8_t b = p[(*i)++];
        v = (v << 7) | (uint32_t)(b & 0x7fu);
        n++;
        if ((b & 0x80u) == 0) {
            *out = v;
            return 1;
        }
    }
    *out = v;
    return 0;
}

static int parse_midi(const uint8_t *p, int len, song_t *out)
{
    int      i;
    int      track_end;
    uint32_t trlen;
    uint32_t tick = 0;
    uint8_t  run = 0;
    int      nev = 0;
    mev_t   *ev;

    if (len < 22 || memcmp(p, "MThd", 4) != 0) {
        return 0;
    }
    out->division = be16(p + 12);
    if (out->division == 0 || (out->division & 0x8000u)) {
        out->division = 70;
    }
    i = 8 + (int)be32(p + 4);
    if (i + 8 > len || memcmp(p + i, "MTrk", 4) != 0) {
        return 0;
    }
    trlen = be32(p + i + 4);
    i += 8;
    track_end = i + (int)trlen;
    if (track_end > len) {
        track_end = len;
    }
    ev = (mev_t *)Z_Malloc(MUS_EVMAX * (int)sizeof(mev_t), PU_STATIC, NULL);
    if (ev == NULL) {
        return 0;
    }
    while (i < track_end && nev < MUS_EVMAX) {
        uint32_t delta = 0;
        uint8_t  st;
        if (!read_vlq(p, &i, track_end, &delta)) {
            break;
        }
        tick += delta;
        if (i >= track_end) {
            break;
        }
        if (p[i] & 0x80u) {
            st = p[i++];
            run = st;
        } else {
            st = run;
        }
        if (st == 0xffu) {
            uint8_t  type;
            uint32_t ln = 0;
            if (i >= track_end) {
                break;
            }
            type = p[i++];
            if (!read_vlq(p, &i, track_end, &ln)) {
                break;
            }
            if (type == 0x51u && ln == 3u && i + 3 <= track_end) {
                ev[nev].tick = tick;
                ev[nev].status = 0xffu;
                ev[nev].a = 0x51u;
                ev[nev].b = 0;
                ev[nev].tempo = ((uint32_t)p[i] << 16) |
                                ((uint32_t)p[i + 1] << 8) | p[i + 2];
                nev++;
            }
            i += (int)ln;
            if (type == 0x2fu) {
                break;
            }
            continue;
        }
        if (st == 0xf0u || st == 0xf7u) {
            uint32_t ln = 0;
            if (!read_vlq(p, &i, track_end, &ln)) {
                break;
            }
            i += (int)ln;
            continue;
        }
        {
            uint8_t cmd = (uint8_t)(st & 0xf0u);
            uint8_t a = 0;
            uint8_t b = 0;
            if (i >= track_end) {
                break;
            }
            a = p[i++];
            if (cmd != 0xc0u && cmd != 0xd0u) {
                if (i >= track_end) {
                    break;
                }
                b = p[i++];
            }
            if (cmd == 0x90u || cmd == 0x80u || cmd == 0xc0u ||
                (cmd == 0xb0u && (a == 7 || a == 0x7bu))) {
                ev[nev].tick = tick;
                ev[nev].status = st;
                ev[nev].a = a;
                ev[nev].b = b;
                ev[nev].tempo = 0;
                nev++;
            }
        }
    }
    out->ev = ev;
    out->nev = nev;
    return nev > 0;
}

static boolean mus_Init(void)
{
    int   i;
    byte *lump;

    OPL3_Reset(&s_opl, MUS_RATE);
    wr(0x01, 0x20);
    wr(0x105, 0x01);
    wr(0x104, 0x00);
    wr(0xBD, 0x00);
    for (i = 0; i < OPL_VOICES; i++) {
        s_vc[i].index = i % OPL_CH;
        s_vc[i].op1 = k_op[0][i % OPL_CH];
        s_vc[i].op2 = k_op[1][i % OPL_CH];
        s_vc[i].array = (i / OPL_CH) << 8;
        s_vc[i].used = 0;
        wr((uint16_t)((OPL_REGS_FEEDBACK + s_vc[i].index) | s_vc[i].array),
           0x30);
    }
    for (i = 0; i < 16; i++) {
        s_cc7[i] = 100;
        s_prog[i] = 0;
    }
    s_melodic = NULL;
    s_drum = NULL;
    if (W_CheckNumForName("GENMIDI") >= 0) {
        lump = (byte *)W_CacheLumpName("GENMIDI", PU_STATIC);
        if (lump != NULL) {
            s_melodic = (genmidi_instr_t *)(lump + 8);
            s_drum = s_melodic + GENMIDI_NUM_INSTRS;
            (void)GENMIDI_HEADER;
            (void)GENMIDI_NUM_PERCUSSION;
        }
    }
    s_opl_on = 1;
    ag_printf("doom: music OPL3+GENMIDI @ %u Hz%s\n", (unsigned)MUS_RATE,
              s_melodic ? "" : " (no GENMIDI lump)");
    return true;
}

static void mus_Shutdown(void)
{
    all_off();
    s_playing = 0;
    s_song = NULL;
    s_opl_on = 0;
}

static void mus_SetVol(int volume)
{
    if (volume < 0) {
        volume = 0;
    }
    if (volume > 127) {
        volume = 127;
    }
    s_vol = volume;
}

static void mus_Pause(void)
{
    s_paused = 1;
    all_off();
}

static void mus_Resume(void) { s_paused = 0; }

static void *mus_Register(void *data, int len)
{
    MEMFILE *in;
    MEMFILE *out;
    void    *mid = NULL;
    size_t   midlen = 0;
    song_t  *song;
    const uint8_t *src = (const uint8_t *)data;
    int ok;

    if (data == NULL || len < 4) {
        return NULL;
    }
    song = (song_t *)Z_Malloc((int)sizeof(song_t), PU_STATIC, NULL);
    if (song == NULL) {
        return NULL;
    }
    memset(song, 0, sizeof(*song));
    if (len >= 4 && memcmp(src, "MThd", 4) == 0) {
        ok = parse_midi(src, len, song);
    } else {
        in = mem_fopen_read(data, (size_t)len);
        out = mem_fopen_write();
        if (in == NULL || out == NULL || mus2mid(in, out)) {
            if (in) {
                mem_fclose(in);
            }
            if (out) {
                mem_fclose(out);
            }
            Z_Free(song);
            return NULL;
        }
        mem_get_buf(out, &mid, &midlen);
        ok = parse_midi((const uint8_t *)mid, (int)midlen, song);
        mem_fclose(in);
        mem_fclose(out);
    }
    if (!ok) {
        if (song->ev) {
            Z_Free(song->ev);
        }
        Z_Free(song);
        return NULL;
    }
    return song;
}

static void mus_UnRegister(void *handle)
{
    song_t *song = (song_t *)handle;
    if (s_song == song) {
        s_song = NULL;
        s_playing = 0;
        all_off();
    }
    if (song == NULL) {
        return;
    }
    if (song->ev) {
        Z_Free(song->ev);
    }
    Z_Free(song);
}

static void mus_Play(void *handle, boolean looping)
{
    int i;
    s_song = (song_t *)handle;
    s_looping = looping ? 1 : 0;
    s_playing = s_song != NULL;
    s_paused = 0;
    s_evi = 0;
    s_tick = 0;
    s_acc = 0;
    s_tempo = 500000u;
    s_age = 0;
    for (i = 0; i < 16; i++) {
        s_cc7[i] = 100;
        s_prog[i] = 0;
    }
    all_off();
}

static void mus_Stop(void)
{
    s_playing = 0;
    all_off();
}

static boolean mus_IsPlaying(void) { return s_playing ? true : false; }

static void mus_Poll(void) {}

void doom_argon_music_mix(int16_t *stereo, int n)
{
    int      i;
    uint32_t den;
    unsigned div;

    if (!s_opl_on || !s_playing || s_paused || s_song == NULL || stereo == NULL ||
        n < 1) {
        return;
    }
    if (n > MUS_MIXMAX) {
        n = MUS_MIXMAX;
    }
    div = s_song->division ? s_song->division : 70u;
    den = MUS_RATE * (s_tempo ? s_tempo : 500000u);
    s_acc += (uint64_t)n * 1000000ull * (uint64_t)div;
    while (den != 0u && s_acc >= (uint64_t)den) {
        s_acc -= (uint64_t)den;
        s_tick++;
        dispatch_to(s_tick);
    }
    OPL3_GenerateStream(&s_opl, s_mix, (uint32_t)n);
    for (i = 0; i < n; i++) {
        int sl = stereo[i * 2] + (s_mix[i * 2] * s_vol) / 160;
        int sr = stereo[i * 2 + 1] + (s_mix[i * 2 + 1] * s_vol) / 160;
        if (sl > 32767) {
            sl = 32767;
        }
        if (sl < -32768) {
            sl = -32768;
        }
        if (sr > 32767) {
            sr = 32767;
        }
        if (sr < -32768) {
            sr = -32768;
        }
        stereo[i * 2] = (int16_t)sl;
        stereo[i * 2 + 1] = (int16_t)sr;
    }
}

music_module_t DG_music_module = {
    k_mdevs,
    (int)(sizeof k_mdevs / sizeof k_mdevs[0]),
    mus_Init,
    mus_Shutdown,
    mus_SetVol,
    mus_Pause,
    mus_Resume,
    mus_Register,
    mus_UnRegister,
    mus_Play,
    mus_Stop,
    mus_IsPlaying,
    mus_Poll,
};
