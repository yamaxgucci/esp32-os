/*
 * ArgonOS Doom music: MUS → MIDI → ag_fm (9-voice OPLL-style), mixed in SFX.
 * Not Nuked/AdLib OPL; tunes play, timbre is the existing FM synth.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>
#include <string.h>

#include "doomgeneric_argon.h"
#include "fm/ag_fm.h"
#include "i_sound.h"
#include "memio.h"
#include "mus2mid.h"
#include "z_zone.h"

#define MUS_RATE   11025u
#define MUS_VOICES 9
#define MUS_EVMAX  4096
#define MIDI_DRUM  9
#define MUS_MIXMAX 1024

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

typedef struct {
    int used;
    int ch;
    int note;
    int age;
} voice_t;

static snddevice_t k_mdevs[] = {
    SNDDEVICE_SB, SNDDEVICE_ADLIB, SNDDEVICE_PAS, SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_GENMIDI,
    SNDDEVICE_AWE32,
};

static ag_fm_t   s_fm;
static int       s_fm_on;
static song_t   *s_song;
static int       s_playing;
static int       s_looping;
static int       s_paused;
static int       s_evi;
static uint32_t  s_tick;
static uint32_t  s_tempo = 500000u;
static uint64_t  s_acc;
static int       s_vol = 64;
static uint8_t   s_prog[16];
static uint8_t   s_cc7[16];
static voice_t   s_vc[MUS_VOICES];
static uint32_t  s_age;
static int16_t   s_l[MUS_MIXMAX];
static int16_t   s_r[MUS_MIXMAX];

static const uint16_t k_fn[12] = {
    173, 183, 194, 206, 218, 231, 245, 259, 275, 291, 308, 327,
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
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

static void note_fnum(int note, uint16_t *fn, uint8_t *blk)
{
    int block;
    if (note < 0) {
        note = 0;
    }
    if (note > 127) {
        note = 127;
    }
    block = note / 12 - 1;
    if (block < 0) {
        block = 0;
    }
    if (block > 7) {
        block = 7;
    }
    *blk = (uint8_t)block;
    *fn = k_fn[note % 12];
}

static int inst_for(int ch, int note)
{
    if (ch == MIDI_DRUM) {
        return 8 + (note % 7);
    }
    return 1 + (s_prog[ch] % 15);
}

static int vol_for(int ch, int vel)
{
    int v = (vel * (int)s_cc7[ch] * (15 - 0)) / (127 * 127);
    if (v < 0) {
        v = 0;
    }
    if (v > 15) {
        v = 15;
    }
    return 15 - v;
}

static int steal_voice(int ch, int note)
{
    int i;
    int best = 0;
    uint32_t oldest = 0xffffffffu;
    for (i = 0; i < MUS_VOICES; i++) {
        if (s_vc[i].used && s_vc[i].ch == ch && s_vc[i].note == note) {
            return i;
        }
    }
    for (i = 0; i < MUS_VOICES; i++) {
        if (!s_vc[i].used) {
            return i;
        }
    }
    for (i = 0; i < MUS_VOICES; i++) {
        if (s_vc[i].age < oldest) {
            oldest = (uint32_t)s_vc[i].age;
            best = i;
        }
    }
    return best;
}

static void voice_off(int ch, int note)
{
    int i;
    for (i = 0; i < MUS_VOICES; i++) {
        if (s_vc[i].used && s_vc[i].ch == ch && s_vc[i].note == note) {
            ag_fm_set_key(&s_fm, i, 0, 0);
            s_vc[i].used = 0;
        }
    }
}

static void voice_on(int ch, int note, int vel)
{
    int      v;
    uint16_t fn;
    uint8_t  blk;
    if (vel <= 0) {
        voice_off(ch, note);
        return;
    }
    v = steal_voice(ch, note);
    if (s_vc[v].used) {
        ag_fm_set_key(&s_fm, v, 0, 0);
    }
    note_fnum(note, &fn, &blk);
    ag_fm_set_inst_vol(&s_fm, v, (uint8_t)inst_for(ch, note),
                       (uint8_t)vol_for(ch, vel));
    ag_fm_set_fnum(&s_fm, v, fn, blk);
    ag_fm_set_key(&s_fm, v, 1, 0);
    s_vc[v].used = 1;
    s_vc[v].ch = ch;
    s_vc[v].note = note;
    s_vc[v].age = (int)++s_age;
}

static void all_off(void)
{
    int i;
    for (i = 0; i < MUS_VOICES; i++) {
        ag_fm_set_key(&s_fm, i, 0, 0);
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
    int i;
    ag_fm_init(&s_fm, 3579545u, MUS_RATE);
    ag_fm_set_clk_div(&s_fm, AG_FM_CLKDIV_OPLL);
    s_fm_on = 1;
    for (i = 0; i < 16; i++) {
        s_cc7[i] = 100;
        s_prog[i] = 0;
    }
    ag_printf("doom: music ag_fm @ %u Hz\n", (unsigned)MUS_RATE);
    return true;
}

static void mus_Shutdown(void)
{
    all_off();
    s_playing = 0;
    s_song = NULL;
    s_fm_on = 0;
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

    if (!s_fm_on || !s_playing || s_paused || s_song == NULL || stereo == NULL ||
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
    ag_fm_update(&s_fm, s_l, s_r, n);
    for (i = 0; i < n; i++) {
        int sl = stereo[i * 2] + (s_l[i] * s_vol) / 127;
        int sr = stereo[i * 2 + 1] + (s_r[i] * s_vol) / 127;
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
