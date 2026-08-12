/*
 * dx7nofx — structural DX7-like FM for Argon CC (nofx, single thread).
 *
 * 6 operators, 32 algorithms, 8 voices, sine ops, LFO shapes.
 * Compiles on the guest with CC.AXE — not the host mkaxe DX7.AXE.
 *
 * Sound/input like host DX7.AXE: /dev/pcmvirt + /dev/midivirt (not mute
 * ag_audio_* → pcmnull).
 *
 *   drv install h:\pcmvirt.sys
 *   drv install h:\midivirt.sys
 *   run h:\cc.axe h:\dx7nofx.c h:\dx7nofx.axe
 *   run h:\dx7nofx.axe
 *
 * Host (same as apps/dx7/README.md):
 *   pcmplay.py --reconnect
 *   midikbd.py --reconnect   (focus that window; Z..M / Q..I)
 *
 * Fallback console keys (sticky ag_key): Z..M / Q..I, [] alg, ,. preset,
 * -= feedback, Space panic, Esc quit.
 */

#define OPS       6
#define ALGS      32
#define VOICES    8
#define SIN_BITS  10
#define SIN_LEN   1024
#define SIN_MASK  1023
#define PHASE_SH  22
#define RATE_HZ   22050
/* 2^32 / 22050 ≈ 194756 — phase step per Hertz at this rate */
#define STEP_HZ   194756
#define N_KEYS    25
#define CHUNK     128
/* ~5805 us @ 22050 — same pacing idea as host DX7 CHUNK_US */
#define CHUNK_US  5805
#define UI_MS     250
/* AG_IOC(AG_DEV_AUDIO=9, 2) = SETFMT */
#define IOC_SETFMT 589826

/* HID usage page 0x07 — same numbers as sdk/include/argon/keys.h */
#define KEY_A     4
#define KEY_B     5
#define KEY_C     6
#define KEY_D     7
#define KEY_E     8
#define KEY_G     10
#define KEY_H     11
#define KEY_I     12
#define KEY_J     13
#define KEY_M     16
#define KEY_N     17
#define KEY_Q     20
#define KEY_R     21
#define KEY_S     22
#define KEY_T     23
#define KEY_U     24
#define KEY_V     25
#define KEY_W     26
#define KEY_X     27
#define KEY_Y     28
#define KEY_Z     29
#define KEY_2     31
#define KEY_3     32
#define KEY_5     34
#define KEY_6     35
#define KEY_7     36
#define KEY_ESC   41
#define KEY_SPACE 44
#define KEY_MINUS 45
#define KEY_EQUAL 46
#define KEY_LBRK  47
#define KEY_RBRK  48
#define KEY_COMMA 54
#define KEY_DOT   55

/* ag_event_t type field @0 (ILP32 layout; buffer ≥ real event). */
#define AG_EV_FOCUS_GAINED 12
#define AG_EV_FOCUS_LOST   13
#define AG_EV_QUIT         14

struct ag_ev {
    int type;
    int pad;
    int ts0;
    int ts1;
    int u0;
    int u1;
    int u2;
    int u3;
    int u4;
    int u5;
};

struct op_patch {
    int rate[4];
    int level[4];
    int out_level;
    int coarse;
    int fine;
    int detune;
};

struct patch {
    struct op_patch op[6];
    int algorithm;
    int feedback;
    int lfo_speed;
    int lfo_delay;
    int lfo_pmd;
    int lfo_amd;
    int lfo_wave;
    int lfo_pms;
};

struct op_state {
    int phase;
    int step;
    int eg_level;
    int eg_target;
    int eg_rate;
    int eg_stage;
    int out;
};

struct voice {
    struct op_state op[6];
    int active;
    int gate;
    int note;
    int out_gain;
    int fb0;
    int fb1;
    int base_hz;
};

/* Layout matches ag_audio_fmt_t (8 bytes). */
struct audio_fmt {
    int rate;
    char channels;
    char bits;
    char pad0;
    char pad1;
};

int sin_tab[1024];
int amp_lut[100];
int alg_dest[192];
struct patch g_patch;
struct voice g_voice[8];
int g_preset;
int g_lfo_phase;
int g_lfo_step;
int g_lfo_delay_left;
int pcm[128];
char pcm_out[512];
char midi_buf[64];
int audio_fd;
int midi_fd;
int k_code[25];
int k_note[25];
int k_held[25];
int k_prev[25];
int prev_lbrk;
int prev_rbrk;
int prev_comma;
int prev_dot;
int prev_minus;
int prev_equal;
int prev_space;
int g_render_us;
int g_send_us;
int g_loop_us;
int g_load_pct;
int g_late;
int g_drop;
int g_resync;
int g_midi_ev;
int g_ui_ms;
int g_dirty;
int g_next_due;

int clampi(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

int fill_sin(void)
{
    int i;
    int x;
    int neg;
    int u;
    int u2;
    int u3;
    int s;
    i = 0;
    while (i < SIN_LEN) {
        x = i & SIN_MASK;
        neg = 0;
        if (x >= 512) {
            x = x - 512;
            neg = 1;
        }
        if (x >= 256) {
            x = 512 - x;
        }
        u = (x * 32767) / 256;
        if (u > 32767) {
            u = 32767;
        }
        u2 = (u * u) / 32768;
        u3 = (u2 * u) / 32768;
        s = (3 * u - u3) / 2;
        if (s > 32767) {
            s = 32767;
        }
        if (s < 0) {
            s = 0;
        }
        if (neg) {
            sin_tab[i] = 0 - s;
        } else {
            sin_tab[i] = s;
        }
        i = i + 1;
    }
    return 0;
}

int isin(int phase)
{
    return sin_tab[(phase >> PHASE_SH) & SIN_MASK];
}

/*
 * Phase step for hz_x100 (Hertz * 100) at RATE_HZ without a 64-bit multiply.
 * step = (hz_x100/100)*STEP_HZ + (hz_x100%100)*STEP_HZ/100
 */
int freq_to_step(int hz_x100)
{
    int hi;
    int lo;
    int step;
    if (hz_x100 < 1) {
        hz_x100 = 1;
    }
    hi = hz_x100 / 100;
    lo = hz_x100 - hi * 100;
    step = hi * STEP_HZ + (lo * STEP_HZ) / 100;
    return step;
}

int note_hz_x100(int note)
{
    int k_ratio[12];
    int n;
    int d;
    int octs;
    int rem;
    int hz;
    k_ratio[0] = 10000;
    k_ratio[1] = 10595;
    k_ratio[2] = 11225;
    k_ratio[3] = 11892;
    k_ratio[4] = 12599;
    k_ratio[5] = 13348;
    k_ratio[6] = 14142;
    k_ratio[7] = 14983;
    k_ratio[8] = 15874;
    k_ratio[9] = 16818;
    k_ratio[10] = 17817;
    k_ratio[11] = 18877;
    n = clampi(note, 0, 127);
    d = n - 69;
    octs = d / 12;
    rem = d - octs * 12;
    if (rem < 0) {
        rem = rem + 12;
        octs = octs - 1;
    }
    hz = (44000 * k_ratio[rem]) / 10000;
    while (octs > 0) {
        hz = hz * 2;
        octs = octs - 1;
    }
    while (octs < 0) {
        hz = hz / 2;
        octs = octs + 1;
    }
    if (hz < 100) {
        hz = 100;
    }
    return hz;
}

int rate_to_delta(int rate)
{
    int r;
    int d;
    r = clampi(rate, 0, 99);
    if (r == 0) {
        return 0;
    }
    d = (r * r * (64 + r)) / 6400;
    if (d < 1) {
        d = 1;
    }
    return d;
}

int level_to_amp256(int level)
{
    int n;
    int amp;
    n = 99 - clampi(level, 0, 99);
    amp = 256;
    while (n >= 8) {
        amp = amp / 2;
        n = n - 8;
    }
    if (n > 0) {
        amp = (amp * (8 - n) + (amp / 2) * n) / 8;
    }
    return amp;
}

int fill_amp_lut(void)
{
    int i;
    i = 0;
    while (i < 100) {
        amp_lut[i] = level_to_amp256(i);
        i = i + 1;
    }
    return 0;
}

int init_algs(void)
{
    /* Flattened k_alg_dest[32][6] from host ag_dx7.c (dest 6 = carrier). */
    int a;
    int row[6];
    int i;
    a = 0;
    while (a < ALGS) {
        if (a == 0) {
            row[0] = 1;
            row[1] = 2;
            row[2] = 3;
            row[3] = 4;
            row[4] = 5;
            row[5] = 6;
        }
        if (a == 1) {
            row[0] = 1;
            row[1] = 2;
            row[2] = 3;
            row[3] = 4;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 2 || a == 3) {
            row[0] = 1;
            row[1] = 2;
            row[2] = 3;
            row[3] = 6;
            row[4] = 5;
            row[5] = 6;
        }
        if (a == 4 || a == 5) {
            row[0] = 1;
            row[1] = 2;
            row[2] = 6;
            row[3] = 4;
            row[4] = 5;
            row[5] = 6;
        }
        if (a == 6) {
            row[0] = 1;
            row[1] = 2;
            row[2] = 6;
            row[3] = 4;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 7 || a == 8) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 3;
            row[3] = 4;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 9 || a == 10) {
            row[0] = 1;
            row[1] = 2;
            row[2] = 6;
            row[3] = 6;
            row[4] = 5;
            row[5] = 6;
        }
        if (a == 11 || a == 12) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 6;
            row[3] = 4;
            row[4] = 5;
            row[5] = 6;
        }
        if (a == 13 || a == 14) {
            row[0] = 1;
            row[1] = 2;
            row[2] = 6;
            row[3] = 4;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 15 || a == 16 || a == 23 || a == 26) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 3;
            row[3] = 6;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 17 || a == 27) {
            row[0] = 1;
            row[1] = 2;
            row[2] = 6;
            row[3] = 6;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 18) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 3;
            row[3] = 4;
            row[4] = 5;
            row[5] = 6;
        }
        if (a == 19) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 3;
            row[3] = 6;
            row[4] = 5;
            row[5] = 6;
        }
        if (a == 20) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 6;
            row[3] = 4;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 21) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 6;
            row[3] = 6;
            row[4] = 5;
            row[5] = 6;
        }
        if (a == 22 || a == 24 || a == 28 || a == 30) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 6;
            row[3] = 6;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 25) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 3;
            row[3] = 4;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 29) {
            row[0] = 1;
            row[1] = 6;
            row[2] = 6;
            row[3] = 4;
            row[4] = 6;
            row[5] = 6;
        }
        if (a == 31) {
            row[0] = 6;
            row[1] = 6;
            row[2] = 6;
            row[3] = 6;
            row[4] = 6;
            row[5] = 6;
        }
        i = 0;
        while (i < OPS) {
            alg_dest[a * OPS + i] = row[i];
            i = i + 1;
        }
        a = a + 1;
    }
    return 0;
}

int op_init(struct op_patch *p, int out_level, int coarse, int fine)
{
    p->rate[0] = 90;
    p->rate[1] = 70;
    p->rate[2] = 70;
    p->rate[3] = 55;
    p->level[0] = 99;
    p->level[1] = 99;
    p->level[2] = 99;
    p->level[3] = 0;
    p->out_level = out_level;
    p->coarse = coarse;
    p->fine = fine;
    p->detune = 7;
    return 0;
}

int patch_init_sine(void)
{
    int i;
    i = 0;
    while (i < OPS) {
        op_init(&g_patch.op[i], 0, 1, 0);
        i = i + 1;
    }
    op_init(&g_patch.op[5], 99, 1, 0);
    g_patch.op[5].rate[0] = 90;
    g_patch.op[5].rate[3] = 55;
    g_patch.algorithm = 31;
    g_patch.feedback = 0;
    g_patch.lfo_speed = 35;
    g_patch.lfo_delay = 0;
    g_patch.lfo_pmd = 0;
    g_patch.lfo_amd = 0;
    g_patch.lfo_wave = 0;
    g_patch.lfo_pms = 0;
    return 0;
}

int patch_epiano(void)
{
    patch_init_sine();
    g_patch.algorithm = 4;
    g_patch.feedback = 4;
    op_init(&g_patch.op[0], 65, 1, 0);
    g_patch.op[0].rate[3] = 60;
    op_init(&g_patch.op[1], 80, 14, 0);
    g_patch.op[1].rate[3] = 70;
    g_patch.op[1].level[2] = 0;
    op_init(&g_patch.op[2], 70, 1, 0);
    op_init(&g_patch.op[3], 99, 1, 0);
    g_patch.op[3].level[2] = 70;
    g_patch.op[3].rate[3] = 45;
    op_init(&g_patch.op[4], 60, 1, 0);
    op_init(&g_patch.op[5], 95, 1, 0);
    g_patch.op[5].level[2] = 65;
    g_patch.op[5].rate[3] = 45;
    return 0;
}

int patch_brass(void)
{
    patch_init_sine();
    g_patch.algorithm = 18;
    g_patch.feedback = 3;
    op_init(&g_patch.op[0], 75, 1, 0);
    g_patch.op[0].rate[0] = 55;
    op_init(&g_patch.op[1], 85, 1, 0);
    op_init(&g_patch.op[2], 99, 1, 0);
    g_patch.op[2].rate[0] = 60;
    g_patch.op[2].level[2] = 80;
    op_init(&g_patch.op[3], 0, 1, 0);
    op_init(&g_patch.op[4], 0, 1, 0);
    op_init(&g_patch.op[5], 0, 1, 0);
    g_patch.lfo_pmd = 15;
    g_patch.lfo_pms = 2;
    g_patch.lfo_speed = 25;
    g_patch.lfo_wave = 0;
    return 0;
}

int patch_bell(void)
{
    int i;
    patch_init_sine();
    g_patch.algorithm = 0;
    g_patch.feedback = 0;
    op_init(&g_patch.op[0], 90, 3, 0);
    op_init(&g_patch.op[1], 80, 7, 0);
    op_init(&g_patch.op[2], 70, 11, 0);
    op_init(&g_patch.op[3], 60, 14, 0);
    op_init(&g_patch.op[4], 50, 1, 0);
    op_init(&g_patch.op[5], 99, 1, 0);
    i = 0;
    while (i < OPS) {
        g_patch.op[i].rate[0] = 99;
        g_patch.op[i].rate[1] = 35;
        g_patch.op[i].rate[2] = 25;
        g_patch.op[i].rate[3] = 30;
        g_patch.op[i].level[1] = 40;
        g_patch.op[i].level[2] = 0;
        i = i + 1;
    }
    return 0;
}

int apply_preset(int n)
{
    g_preset = n;
    if (n < 0) {
        g_preset = 0;
    }
    if (g_preset > 3) {
        g_preset = 3;
    }
    if (g_preset == 0) {
        patch_init_sine();
    }
    if (g_preset == 1) {
        patch_epiano();
    }
    if (g_preset == 2) {
        patch_brass();
    }
    if (g_preset == 3) {
        patch_bell();
    }
    return 0;
}

int eg_set_stage(struct op_state *o, struct op_patch *p, int stage)
{
    if (stage < 0 || stage > 3) {
        o->eg_stage = 4;
        o->eg_target = 0;
        o->eg_rate = 0;
        return 0;
    }
    o->eg_stage = stage;
    o->eg_target = p->level[stage] * 256;
    o->eg_rate = rate_to_delta(p->rate[stage]);
    if (o->eg_rate < 1 && p->rate[stage] > 0) {
        o->eg_rate = 1;
    }
    return 0;
}

/* Advance EG by `n` samples (coarser than host; big win on CC). */
int eg_advance(struct op_state *o, struct op_patch *p, int n)
{
    int d;
    if (o->eg_stage >= 4) {
        o->eg_level = 0;
        return 0;
    }
    if (o->eg_stage == 2 && o->eg_level == o->eg_target) {
        return 0;
    }
    if (n < 1) {
        n = 1;
    }
    d = o->eg_rate * n;
    if (d <= 0) {
        o->eg_level = o->eg_target;
        if (o->eg_stage < 2) {
            eg_set_stage(o, p, o->eg_stage + 1);
        }
        return 0;
    }
    if (o->eg_level < o->eg_target) {
        o->eg_level = o->eg_level + d;
        if (o->eg_level >= o->eg_target) {
            o->eg_level = o->eg_target;
            if (o->eg_stage < 2) {
                eg_set_stage(o, p, o->eg_stage + 1);
            }
        }
    } else {
        if (o->eg_level > o->eg_target) {
            o->eg_level = o->eg_level - d;
            if (o->eg_level <= o->eg_target) {
                o->eg_level = o->eg_target;
                if (o->eg_stage < 2) {
                    eg_set_stage(o, p, o->eg_stage + 1);
                } else {
                    if (o->eg_stage == 3 && o->eg_level <= 0) {
                        o->eg_stage = 4;
                        o->eg_level = 0;
                    }
                }
            }
        } else {
            if (o->eg_stage < 2) {
                eg_set_stage(o, p, o->eg_stage + 1);
            } else {
                if (o->eg_stage == 3 && o->eg_level <= 0) {
                    o->eg_stage = 4;
                    o->eg_level = 0;
                }
            }
        }
    }
    return 0;
}

int op_freq_hz_x100(struct voice *v, int oi)
{
    struct op_patch *p;
    int coarse;
    int fine;
    int ratio;
    int hz;
    int det;
    p = &g_patch.op[oi];
    coarse = p->coarse;
    fine = p->fine;
    det = p->detune - 7;
    if (coarse == 0) {
        ratio = 50 + fine;
    } else {
        ratio = coarse * 100 + fine;
    }
    hz = (v->base_hz * ratio) / 100;
    if (det != 0) {
        hz = hz + (hz * det) / 400;
    }
    if (hz < 100) {
        hz = 100;
    }
    return hz;
}

int voice_recompute_steps(struct voice *v, int pitch_mod)
{
    int i;
    int hz;
    int base;
    base = v->base_hz;
    if (pitch_mod != 0) {
        base = base + (base * pitch_mod) / 200;
        if (base < 100) {
            base = 100;
        }
    }
    i = 0;
    while (i < OPS) {
        hz = op_freq_hz_x100(v, i);
        if (pitch_mod != 0) {
            hz = hz + (hz * pitch_mod) / 200;
        }
        v->op[i].step = freq_to_step(hz);
        i = i + 1;
    }
    return 0;
}

int voice_note_on(struct voice *v, int note)
{
    int i;
    v->active = 1;
    v->gate = 1;
    v->note = note;
    v->base_hz = note_hz_x100(note);
    v->out_gain = 0;
    v->fb0 = 0;
    v->fb1 = 0;
    i = 0;
    while (i < OPS) {
        v->op[i].phase = 0;
        v->op[i].out = 0;
        v->op[i].eg_level = 0;
        eg_set_stage(&v->op[i], &g_patch.op[i], 0);
        i = i + 1;
    }
    voice_recompute_steps(v, 0);
    return 0;
}

int voice_note_off(struct voice *v)
{
    int i;
    if (v->active == 0) {
        return 0;
    }
    v->gate = 0;
    i = 0;
    while (i < OPS) {
        eg_set_stage(&v->op[i], &g_patch.op[i], 3);
        i = i + 1;
    }
    return 0;
}

int voice_dead(struct voice *v)
{
    int i;
    if (v->gate) {
        return 0;
    }
    i = 0;
    while (i < OPS) {
        if (v->op[i].eg_stage < 4 && v->op[i].eg_level > 0) {
            return 0;
        }
        i = i + 1;
    }
    return 1;
}

int op_amp_fast(struct voice *v, int oi)
{
    int lvl;
    int eg;
    lvl = g_patch.op[oi].out_level;
    if (lvl < 0) {
        lvl = 0;
    }
    if (lvl > 99) {
        lvl = 99;
    }
    eg = v->op[oi].eg_level >> 8;
    if (eg < 0) {
        eg = 0;
    }
    if (eg > 99) {
        eg = 99;
    }
    return (amp_lut[lvl] * amp_lut[eg]) >> 8;
}

int lfo_value(void)
{
    int ph;
    int w;
    int t;
    ph = (g_lfo_phase >> PHASE_SH) & SIN_MASK;
    w = g_patch.lfo_wave;
    if (w == 4) {
        return isin(g_lfo_phase) / 128;
    }
    if (w == 3) {
        if (ph < 512) {
            return 256;
        }
        return 0 - 256;
    }
    if (w == 1) {
        return 256 - (ph * 512) / SIN_LEN;
    }
    if (w == 2) {
        return (ph * 512) / SIN_LEN - 256;
    }
    /* triangle */
    if (ph < 512) {
        t = (ph * 512) / 512 - 256;
    } else {
        t = 256 - ((ph - 512) * 512) / 512;
    }
    return t;
}

int lfo_tick(int n)
{
    int hz;
    int depth;
    if (g_lfo_delay_left > 0) {
        g_lfo_delay_left = g_lfo_delay_left - n;
        if (g_lfo_delay_left < 0) {
            g_lfo_delay_left = 0;
        }
        return 0;
    }
    /* speed 0..99 → ~0..20 Hz */
    hz = (g_patch.lfo_speed * g_patch.lfo_speed) / 50;
    if (hz < 1 && g_patch.lfo_speed > 0) {
        hz = 1;
    }
    g_lfo_step = freq_to_step(hz * 100);
    g_lfo_phase = g_lfo_phase + g_lfo_step * n;
    depth = 0;
    return depth;
}

int voice_eg_chunk(struct voice *v, int n)
{
    int i;
    i = 0;
    while (i < OPS) {
        eg_advance(&v->op[i], &g_patch.op[i], n);
        i = i + 1;
    }
    return 0;
}

int voice_gain_tick(struct voice *v)
{
    int dead;
    dead = 0;
    if (v->gate) {
        if (v->out_gain < 256) {
            v->out_gain = v->out_gain + 8;
            if (v->out_gain > 256) {
                v->out_gain = 256;
            }
        }
    } else {
        dead = voice_dead(v);
        if (dead) {
            if (v->out_gain > 0) {
                v->out_gain = v->out_gain - 8;
                if (v->out_gain < 0) {
                    v->out_gain = 0;
                }
            }
        }
    }
    if (dead && v->out_gain <= 0) {
        v->active = 0;
    }
    return 0;
}

/* One sample; EG is advanced once per chunk in render(). */
int voice_sample(struct voice *v, int amp_mod, int pitch_mod, int alg)
{
    int mod[6];
    int out;
    int i;
    int dest;
    int fb;
    int amp;
    int s;
    int am;

    i = 0;
    while (i < OPS) {
        mod[i] = 0;
        i = i + 1;
    }

    fb = v->fb0 + v->fb1;
    if (g_patch.feedback > 0) {
        mod[0] = (fb * g_patch.feedback) * 1024;
    }

    out = 0;
    i = 0;
    while (i < OPS) {
        v->op[i].phase = v->op[i].phase + v->op[i].step;
        if (pitch_mod != 0) {
            v->op[i].phase =
                v->op[i].phase + (v->op[i].step * pitch_mod) / 512;
        }
        amp = op_amp_fast(v, i);
        if (amp_mod != 0) {
            am = 256 + amp_mod;
            if (am < 1) {
                am = 1;
            }
            amp = (amp * am) >> 8;
        }
        if (amp == 0) {
            s = 0;
        } else {
            s = (sin_tab[((v->op[i].phase + mod[i]) >> PHASE_SH) & SIN_MASK] *
                 amp) /
                99;
        }
        v->op[i].out = s;
        dest = alg_dest[alg * OPS + i];
        if (dest >= 6) {
            out = out + s;
        } else {
            mod[dest] = mod[dest] + (s << 16);
        }
        i = i + 1;
    }

    v->fb0 = v->fb1;
    v->fb1 = v->op[0].out;
    return (out * v->out_gain) >> 9;
}

int find_voice(int note)
{
    int i;
    i = 0;
    while (i < VOICES) {
        if (g_voice[i].active && g_voice[i].note == note && g_voice[i].gate) {
            return i;
        }
        i = i + 1;
    }
    return 0 - 1;
}

int alloc_voice(void)
{
    int i;
    int best;
    i = 0;
    while (i < VOICES) {
        if (g_voice[i].active == 0) {
            return i;
        }
        i = i + 1;
    }
    /* steal quietest */
    best = 0;
    i = 1;
    while (i < VOICES) {
        if (g_voice[i].out_gain < g_voice[best].out_gain) {
            best = i;
        }
        i = i + 1;
    }
    return best;
}

int note_on(int note)
{
    int vi;
    vi = find_voice(note);
    if (vi >= 0) {
        voice_note_on(&g_voice[vi], note);
        return vi;
    }
    vi = alloc_voice();
    voice_note_on(&g_voice[vi], note);
    return vi;
}

int note_off(int note)
{
    int vi;
    vi = find_voice(note);
    if (vi >= 0) {
        voice_note_off(&g_voice[vi]);
    }
    return 0;
}

int voice_panic(void)
{
    int i;
    i = 0;
    while (i < VOICES) {
        if (g_voice[i].active) {
            voice_note_off(&g_voice[i]);
        }
        i = i + 1;
    }
    i = 0;
    while (i < N_KEYS) {
        k_held[i] = 0;
        k_prev[i] = 0;
        i = i + 1;
    }
    return 0;
}

int open_audio(void)
{
    struct audio_fmt fmt;
    int err;

    audio_fd = ag_dev_open("pcmvirt");
    if (audio_fd < 0) {
        audio_fd = ag_dev_open("pcmnull");
        if (audio_fd < 0) {
            ag_print("dx7nofx: no pcmvirt/pcmnull\n");
            return 0 - 1;
        }
        ag_print("dx7nofx: audio = /dev/pcmnull (mute)\n");
    } else {
        ag_print("dx7nofx: audio = /dev/pcmvirt (pcmplay.py :5558)\n");
    }
    fmt.rate = RATE_HZ;
    fmt.channels = 2;
    fmt.bits = 16;
    fmt.pad0 = 0;
    fmt.pad1 = 0;
    err = ag_dev_ioctl(audio_fd, IOC_SETFMT, &fmt, 8);
    if (err != 0) {
        ag_print("dx7nofx: SETFMT failed\n");
        ag_dev_close(audio_fd);
        audio_fd = 0 - 1;
        return 0 - 1;
    }
    return 0;
}

int write_pcm(int n)
{
    int i;
    int o;
    int s;
    int lo;
    int hi;
    int want;
    int wr;

    if (audio_fd < 0) {
        return 0 - 1;
    }
    if (n > CHUNK) {
        n = CHUNK;
    }
    o = 0;
    i = 0;
    while (i < n) {
        s = pcm[i];
        lo = s & 255;
        hi = (s >> 8) & 255;
        pcm_out[o] = lo;
        pcm_out[o + 1] = hi;
        pcm_out[o + 2] = lo;
        pcm_out[o + 3] = hi;
        o = o + 4;
        i = i + 1;
    }
    want = n * 4;
    wr = ag_dev_write(audio_fd, pcm_out, want);
    if (wr < 0) {
        g_drop = g_drop + want;
        return wr;
    }
    if (wr < want) {
        g_drop = g_drop + (want - wr);
    }
    return wr;
}

int open_midi(void)
{
    midi_fd = ag_dev_open("midivirt");
    if (midi_fd < 0) {
        return 0;
    }
    ag_print("dx7nofx: MIDI-in = /dev/midivirt (midikbd.py :5559)\n");
    return 0;
}

int pump_midivirt(void)
{
    int n;
    int i;
    int st;
    int d1;
    int d2;
    int hi;

    if (midi_fd < 0) {
        return 0;
    }
    n = ag_dev_read(midi_fd, midi_buf, 64);
    if (n <= 0) {
        return 0;
    }
    n = (n / 4) * 4;
    i = 0;
    while (i + 3 < n) {
        st = midi_buf[i] & 255;
        d1 = midi_buf[i + 1] & 255;
        d2 = midi_buf[i + 2] & 255;
        hi = st & 240;
        g_midi_ev = g_midi_ev + 1;
        if (hi == 144) {
            if (d2) {
                note_on(d1);
            } else {
                note_off(d1);
            }
        } else {
            if (hi == 128) {
                note_off(d1);
            } else {
                if (hi == 176 && d1 == 123) {
                    voice_panic();
                }
            }
        }
        i = i + 4;
    }
    return 0;
}

int voices_active(void)
{
    int i;
    int n;
    n = 0;
    i = 0;
    while (i < VOICES) {
        if (g_voice[i].active) {
            n = n + 1;
        }
        i = i + 1;
    }
    return n;
}

int pace_wait(int due)
{
    int now;
    int rem;
    now = ag_micros();
    while (now - due < 0) {
        rem = due - now;
        if (rem > 2000) {
            ag_delay((rem / 1000) - 1);
        }
        now = ag_micros();
    }
    return 0;
}

int draw_ui(void)
{
    int budget;
    budget = CHUNK_US;
    if (budget < 1) {
        budget = 1;
    }
    ag_cls();
    ag_printf("dx7nofx  %d-voice structural FM (CC)\n", VOICES);
    ag_printf("-------------------------\n");
    ag_printf("Patch : [%d] alg %d  fb %d  lfo %d\n", g_preset,
              g_patch.algorithm, g_patch.feedback, g_patch.lfo_speed);
    ag_printf("Voices: %d / %d active\n", voices_active(), VOICES);
    if (midi_fd >= 0) {
        ag_printf("MIDI  : midivirt  inev %d\n", g_midi_ev);
    } else {
        ag_printf("MIDI  : (no midivirt — console keys)\n");
    }
    ag_printf("Perf  : render %d us / %d us (%d%%)  send %d us  loop %d us\n",
              g_render_us, budget, g_load_pct, g_send_us, g_loop_us);
    ag_printf("Stream: late %d  drop %d B  resync %d  chunk %d\n", g_late,
              g_drop, g_resync, CHUNK);
    ag_printf("\n");
    ag_printf("Help  : midikbd.py  or  Z..M / Q..I\n");
    ag_printf("        [ ] alg  , . preset  - = fb  Space panic  Esc quit\n");
    g_dirty = 0;
    return 0;
}

int render(int n)
{
    int i;
    int vi;
    int s;
    int lv;
    int amp_mod;
    int pitch_mod;
    int pms;
    int alg;

    i = 0;
    while (i < n) {
        pcm[i] = 0;
        i = i + 1;
    }

    lfo_tick(n);
    lv = 0;
    if (g_lfo_delay_left <= 0) {
        lv = lfo_value();
    }
    amp_mod = (lv * g_patch.lfo_amd) / 99;
    pms = g_patch.lfo_pms;
    pitch_mod = (lv * g_patch.lfo_pmd * pms) / (99 * 4);
    alg = g_patch.algorithm;
    if (alg < 0 || alg >= ALGS) {
        alg = 0;
    }

    /* Voice-outer: EG once per chunk; only active voices touch samples. */
    vi = 0;
    while (vi < VOICES) {
        if (g_voice[vi].active) {
            voice_eg_chunk(&g_voice[vi], n);
            i = 0;
            while (i < n) {
                s = voice_sample(&g_voice[vi], amp_mod, pitch_mod, alg);
                pcm[i] = pcm[i] + s;
                i = i + 1;
            }
            voice_gain_tick(&g_voice[vi]);
        }
        vi = vi + 1;
    }

    i = 0;
    while (i < n) {
        if (pcm[i] > 32000) {
            pcm[i] = 32000;
        }
        if (pcm[i] < 0 - 32000) {
            pcm[i] = 0 - 32000;
        }
        i = i + 1;
    }
    return n;
}

int pump_audio(void)
{
    int t0;
    int t1;
    t0 = ag_micros();
    render(CHUNK);
    t1 = ag_micros();
    g_render_us = t1 - t0;
    if (g_render_us < 0) {
        g_render_us = 0;
    }
    g_load_pct = (g_render_us * 100 + CHUNK_US / 2) / CHUNK_US;
    if (g_load_pct == 0 && g_render_us > 0) {
        g_load_pct = 1;
    }
    write_pcm(CHUNK);
    g_send_us = ag_micros() - t1;
    if (g_send_us < 0) {
        g_send_us = 0;
    }
    return 0;
}

int edge(int cur, int *prev)
{
    int p;
    p = *prev;
    *prev = cur;
    if (cur != 0 && p == 0) {
        return 1;
    }
    return 0;
}

int keys_init(void)
{
    /* Same concert map as apps/dx7/dx7.c (Z..M / Q..I). */
    k_code[0] = KEY_Z;
    k_note[0] = 60;
    k_code[1] = KEY_S;
    k_note[1] = 61;
    k_code[2] = KEY_X;
    k_note[2] = 62;
    k_code[3] = KEY_D;
    k_note[3] = 63;
    k_code[4] = KEY_C;
    k_note[4] = 64;
    k_code[5] = KEY_V;
    k_note[5] = 65;
    k_code[6] = KEY_G;
    k_note[6] = 66;
    k_code[7] = KEY_B;
    k_note[7] = 67;
    k_code[8] = KEY_H;
    k_note[8] = 68;
    k_code[9] = KEY_N;
    k_note[9] = 69;
    k_code[10] = KEY_J;
    k_note[10] = 70;
    k_code[11] = KEY_M;
    k_note[11] = 71;
    k_code[12] = KEY_Q;
    k_note[12] = 72;
    k_code[13] = KEY_2;
    k_note[13] = 73;
    k_code[14] = KEY_W;
    k_note[14] = 74;
    k_code[15] = KEY_3;
    k_note[15] = 75;
    k_code[16] = KEY_E;
    k_note[16] = 76;
    k_code[17] = KEY_R;
    k_note[17] = 77;
    k_code[18] = KEY_5;
    k_note[18] = 78;
    k_code[19] = KEY_T;
    k_note[19] = 79;
    k_code[20] = KEY_6;
    k_note[20] = 80;
    k_code[21] = KEY_Y;
    k_note[21] = 81;
    k_code[22] = KEY_7;
    k_note[22] = 82;
    k_code[23] = KEY_U;
    k_note[23] = 83;
    k_code[24] = KEY_I;
    k_note[24] = 84;
    return 0;
}

int piano_all_off(void)
{
    int i;
    i = 0;
    while (i < N_KEYS) {
        if (k_held[i]) {
            note_off(k_note[i]);
            k_held[i] = 0;
        }
        k_prev[i] = 0;
        i = i + 1;
    }
    return 0;
}

int poll_piano(void)
{
    int i;
    int cur;
    int any_down;
    any_down = 0;
    i = 0;
    while (i < N_KEYS) {
        cur = ag_key(k_code[i]);
        if (cur != 0 && k_prev[i] == 0) {
            /* Plain VT often has no KEY_UP: new press frees the previous. */
            piano_all_off();
            note_on(k_note[i]);
            k_held[i] = 1;
            any_down = 1;
        } else {
            if (cur == 0 && k_held[i]) {
                note_off(k_note[i]);
                k_held[i] = 0;
            }
            if (cur != 0) {
                k_held[i] = 1;
                any_down = 1;
            }
        }
        k_prev[i] = cur;
        i = i + 1;
    }
    return any_down;
}

int poll_controls(void)
{
    if (edge(ag_key(KEY_LBRK), &prev_lbrk)) {
        g_patch.algorithm = g_patch.algorithm - 1;
        if (g_patch.algorithm < 0) {
            g_patch.algorithm = ALGS - 1;
        }
        g_dirty = 1;
    }
    if (edge(ag_key(KEY_RBRK), &prev_rbrk)) {
        g_patch.algorithm = g_patch.algorithm + 1;
        if (g_patch.algorithm >= ALGS) {
            g_patch.algorithm = 0;
        }
        g_dirty = 1;
    }
    if (edge(ag_key(KEY_COMMA), &prev_comma)) {
        apply_preset(g_preset - 1);
        g_dirty = 1;
    }
    if (edge(ag_key(KEY_DOT), &prev_dot)) {
        apply_preset(g_preset + 1);
        g_dirty = 1;
    }
    if (edge(ag_key(KEY_MINUS), &prev_minus)) {
        if (g_patch.feedback > 0) {
            g_patch.feedback = g_patch.feedback - 1;
        }
        g_dirty = 1;
    }
    if (edge(ag_key(KEY_EQUAL), &prev_equal)) {
        if (g_patch.feedback < 7) {
            g_patch.feedback = g_patch.feedback + 1;
        }
        g_dirty = 1;
    }
    if (edge(ag_key(KEY_SPACE), &prev_space)) {
        voice_panic();
        g_dirty = 1;
    }
    return 0;
}

int ag_main(void)
{
    int n;
    int frames;
    int sink;
    int loop0;
    int now;
    int ms;
    int running;

    fill_sin();
    fill_amp_lut();
    init_algs();
    keys_init();
    apply_preset(0);
    g_lfo_phase = 0;
    g_lfo_delay_left = 0;
    audio_fd = 0 - 1;
    midi_fd = 0 - 1;
    prev_lbrk = 0;
    prev_rbrk = 0;
    prev_comma = 0;
    prev_dot = 0;
    prev_minus = 0;
    prev_equal = 0;
    prev_space = 0;
    g_render_us = 0;
    g_send_us = 0;
    g_loop_us = 0;
    g_load_pct = 0;
    g_late = 0;
    g_drop = 0;
    g_resync = 0;
    g_midi_ev = 0;
    g_dirty = 1;

    n = 0;
    while (n < VOICES) {
        g_voice[n].active = 0;
        g_voice[n].gate = 0;
        n = n + 1;
    }
    n = 0;
    while (n < N_KEYS) {
        k_held[n] = 0;
        k_prev[n] = 0;
        n = n + 1;
    }

    if (open_audio() != 0) {
        return 2;
    }
    open_midi();

    /* Short poly smoke so QEMU can verify render without keyboard. */
    note_on(60);
    note_on(64);
    n = 0;
    while (n < 8) {
        pump_audio();
        n = n + 1;
    }
    note_off(60);
    note_off(64);
    n = 0;
    while (n < 4) {
        pump_audio();
        n = n + 1;
    }
    ag_print("dx7nofx: smoke ok\n");

    /*
     * Wait out sticky TTL from console traffic (e.g. -Put of the .AXE),
     * sampling without acting, then arm edge detectors at idle.
     */
    n = 0;
    while (n < 40) {
        frames = 0;
        while (frames < N_KEYS) {
            sink = ag_key(k_code[frames]);
            frames = frames + 1;
        }
        sink = ag_key(KEY_LBRK);
        sink = ag_key(KEY_RBRK);
        sink = ag_key(KEY_COMMA);
        sink = ag_key(KEY_DOT);
        sink = ag_key(KEY_MINUS);
        sink = ag_key(KEY_EQUAL);
        sink = ag_key(KEY_SPACE);
        sink = ag_key(KEY_ESC);
        ag_delay(20);
        n = n + 1;
    }
    prev_lbrk = 0;
    prev_rbrk = 0;
    prev_comma = 0;
    prev_dot = 0;
    prev_minus = 0;
    prev_equal = 0;
    prev_space = 0;
    n = 0;
    while (n < N_KEYS) {
        k_prev[n] = 0;
        k_held[n] = 0;
        n = n + 1;
    }

    g_ui_ms = ag_millis();
    g_next_due = ag_micros() + CHUNK_US;
    g_dirty = 1;
    draw_ui();

    running = 1;
    while (running) {
        struct ag_ev ev;

        loop0 = ag_micros();
        while (ag_poll_event(&ev, 0)) {
            if (ev.type == AG_EV_FOCUS_GAINED) {
                g_dirty = 1;
            } else if (ev.type == AG_EV_QUIT) {
                if (ag_focused()) {
                    running = 0;
                }
            }
        }
        if (running == 0) {
            break;
        }

        /* Keep PCM/MIDI alive in the background; pause console UI + keys. */
        pump_midivirt();
        pump_audio();
        if (!ag_focused()) {
            ag_heartbeat();
            g_next_due = ag_micros() + CHUNK_US;
            continue;
        }
        if (ag_key(KEY_ESC)) {
            running = 0;
            break;
        }
        poll_piano();
        poll_controls();

        now = ag_micros();
        if (now - g_next_due < 0) {
            pace_wait(g_next_due);
            g_next_due = g_next_due + CHUNK_US;
        } else {
            g_late = g_late + 1;
            if (now - g_next_due > CHUNK_US) {
                g_resync = g_resync + 1;
            }
            g_next_due = now + CHUNK_US;
        }
        g_loop_us = ag_micros() - loop0;
        if (g_loop_us < 0) {
            g_loop_us = 0;
        }

        ms = ag_millis();
        if (g_dirty || (ms - g_ui_ms) >= UI_MS) {
            g_ui_ms = ms;
            draw_ui();
        }
    }
    voice_panic();
    if (midi_fd >= 0) {
        ag_dev_close(midi_fd);
        midi_fd = 0 - 1;
    }
    if (audio_fd >= 0) {
        ag_dev_close(audio_fd);
        audio_fd = 0 - 1;
    }
    return 0;
}
