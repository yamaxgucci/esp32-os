/*
 * Minimal SMF → flat timed event list.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ag_mid.h"

#include <argon/argon.h>

/* Keep under the typical AXE arena (128 KB): temp+out must both fit. */
enum { AG_MID_MAX_EV = 1024, AG_MID_MAX_TMP = 1024 };

static void mem_zero(void *p, unsigned n)
{
    unsigned char *d = (unsigned char *)p;
    while (n--) {
        *d++ = 0;
    }
}

static void name_copy(char *dst, const char *src, int maxn)
{
    int i = 0;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    for (; i < maxn - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static int rd_u16(const uint8_t *p)
{
    return ((int)p[0] << 8) | (int)p[1];
}

static uint32_t rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int read_vlq(const uint8_t *data, int len, int *pos, uint32_t *out)
{
    uint32_t v = 0;
    int n = 0;
    for (;;) {
        uint8_t b;
        if (*pos >= len || n >= 4) {
            return -1;
        }
        b = data[(*pos)++];
        v = (v << 7) | (uint32_t)(b & 0x7fu);
        n++;
        if ((b & 0x80u) == 0u) {
            break;
        }
    }
    *out = v;
    return 0;
}

typedef struct {
    uint32_t tick;
    uint8_t  type;
    uint8_t  note;
    uint8_t  vel;
    uint8_t  ch;
    uint32_t tempo; /* only for type==0 meta-tempo stash: us/qn */
} raw_ev_t;

static int push_raw(raw_ev_t *raw, int *nraw, uint32_t tick, uint8_t type,
                    uint8_t note, uint8_t vel, uint8_t ch, uint32_t tempo)
{
    if (*nraw >= AG_MID_MAX_TMP) {
        return -1;
    }
    raw[*nraw].tick = tick;
    raw[*nraw].type = type;
    raw[*nraw].note = note;
    raw[*nraw].vel = vel;
    raw[*nraw].ch = ch;
    raw[*nraw].tempo = tempo;
    (*nraw)++;
    return 0;
}

static int parse_track(const uint8_t *data, int len, raw_ev_t *raw, int *nraw)
{
    int pos = 0;
    uint32_t tick = 0;
    uint8_t running = 0;

    while (pos < len) {
        uint32_t delta;
        uint8_t st;
        if (read_vlq(data, len, &pos, &delta) != 0) {
            return -1;
        }
        tick += delta;
        if (pos >= len) {
            break;
        }
        st = data[pos];
        if (st & 0x80u) {
            running = st;
            pos++;
        } else if (running == 0u) {
            return -1;
        } else {
            st = running;
        }

        if (st == 0xffu) {
            uint8_t meta;
            uint32_t mlen;
            if (pos >= len) {
                return -1;
            }
            meta = data[pos++];
            if (read_vlq(data, len, &pos, &mlen) != 0 ||
                pos + (int)mlen > len) {
                return -1;
            }
            if (meta == 0x51u && mlen == 3u) {
                uint32_t us = ((uint32_t)data[pos] << 16) |
                              ((uint32_t)data[pos + 1] << 8) |
                              (uint32_t)data[pos + 2];
                if (us < 1u) {
                    us = 1u;
                }
                if (push_raw(raw, nraw, tick, 0, 0, 0, 0, us) != 0) {
                    return -1;
                }
            } else if (meta == 0x2fu) {
                /* end of track */
                break;
            }
            pos += (int)mlen;
        } else if (st == 0xf0u || st == 0xf7u) {
            uint32_t slen;
            if (read_vlq(data, len, &pos, &slen) != 0 ||
                pos + (int)slen > len) {
                return -1;
            }
            pos += (int)slen;
        } else {
            uint8_t hi = (uint8_t)(st & 0xf0u);
            uint8_t ch = (uint8_t)(st & 0x0fu);
            if (hi == 0xc0u || hi == 0xd0u) {
                uint8_t a;
                if (pos >= len) {
                    return -1;
                }
                a = data[pos++];
                if (hi == 0xc0u) {
                    if (push_raw(raw, nraw, tick, AG_MID_EV_PROG, a, 0, ch,
                                 0) != 0) {
                        return -1;
                    }
                }
            } else if (hi == 0x80u || hi == 0x90u || hi == 0xa0u ||
                       hi == 0xb0u || hi == 0xe0u) {
                uint8_t a, b;
                if (pos + 1 >= len) {
                    return -1;
                }
                a = data[pos++];
                b = data[pos++];
                if (hi == 0x90u) {
                    if (b == 0u) {
                        if (push_raw(raw, nraw, tick, AG_MID_EV_OFF, a, 0, ch,
                                     0) != 0) {
                            return -1;
                        }
                    } else if (push_raw(raw, nraw, tick, AG_MID_EV_ON, a, b, ch,
                                        0) != 0) {
                        return -1;
                    }
                } else if (hi == 0x80u) {
                    if (push_raw(raw, nraw, tick, AG_MID_EV_OFF, a, b, ch, 0) !=
                        0) {
                        return -1;
                    }
                }
            } else {
                /* unknown status — bail track */
                break;
            }
        }
    }
    return 0;
}

static int cmp_raw(const raw_ev_t *a, const raw_ev_t *b)
{
    if (a->tick < b->tick) {
        return -1;
    }
    if (a->tick > b->tick) {
        return 1;
    }
    /* tempo before notes at same tick */
    if (a->type == 0 && b->type != 0) {
        return -1;
    }
    if (a->type != 0 && b->type == 0) {
        return 1;
    }
    return 0;
}

static void sort_raw(raw_ev_t *raw, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        raw_ev_t key = raw[i];
        j = i - 1;
        while (j >= 0 && cmp_raw(&raw[j], &key) > 0) {
            raw[j + 1] = raw[j];
            j--;
        }
        raw[j + 1] = key;
    }
}

void ag_mid_init(ag_mid_player_t *p)
{
    if (p == NULL) {
        return;
    }
    mem_zero(p, sizeof(*p));
    p->loop = 1;
}

void ag_mid_unload(ag_mid_player_t *p)
{
    if (p == NULL) {
        return;
    }
    if (p->ev != NULL) {
        ag_free(p->ev);
        p->ev = NULL;
    }
    mem_zero(p, sizeof(*p));
    p->loop = 1;
}

int ag_mid_load(ag_mid_player_t *p, const uint8_t *data, int len,
                const char *name)
{
    raw_ev_t *raw;
    int nraw = 0;
    int pos;
    int format, ntrks, division;
    uint32_t tempo = 500000u; /* 120 BPM */
    uint32_t ppqn;
    uint64_t us = 0;
    uint32_t last_tick = 0;
    int i, out_n;
    ag_mid_ev_t *out;

    if (p == NULL || data == NULL || len < 14) {
        return -1;
    }
    ag_mid_unload(p);
    ag_mid_init(p);
    name_copy(p->name, name ? name : "song.mid", (int)sizeof(p->name));

    if (data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd') {
        return -1;
    }
    if (rd_u32(data + 4) < 6u) {
        return -1;
    }
    format = rd_u16(data + 8);
    ntrks = rd_u16(data + 10);
    division = rd_u16(data + 12);
    (void)format;
    if (division & 0x8000) {
        /* SMPTE — not supported */
        return -1;
    }
    ppqn = (uint32_t)division;
    if (ppqn < 1u) {
        ppqn = 96u;
    }

    raw = (raw_ev_t *)ag_malloc(sizeof(raw_ev_t) * AG_MID_MAX_TMP);
    if (raw == NULL) {
        return -1;
    }

    pos = 8 + (int)rd_u32(data + 4); /* after MThd chunk */
    while (pos + 8 <= len && ntrks > 0) {
        uint32_t tlen;
        if (data[pos] != 'M' || data[pos + 1] != 'T' || data[pos + 2] != 'r' ||
            data[pos + 3] != 'k') {
            break;
        }
        tlen = rd_u32(data + pos + 4);
        pos += 8;
        if (pos + (int)tlen > len) {
            ag_free(raw);
            return -1;
        }
        if (parse_track(data + pos, (int)tlen, raw, &nraw) != 0) {
            ag_free(raw);
            return -1;
        }
        pos += (int)tlen;
        ntrks--;
    }

    if (nraw < 1) {
        ag_free(raw);
        return -1;
    }
    sort_raw(raw, nraw);

    out = (ag_mid_ev_t *)ag_malloc(sizeof(ag_mid_ev_t) * AG_MID_MAX_EV);
    if (out == NULL) {
        ag_free(raw);
        return -1;
    }
    out_n = 0;
    for (i = 0; i < nraw && out_n < AG_MID_MAX_EV; i++) {
        uint32_t dt = raw[i].tick - last_tick;
        last_tick = raw[i].tick;
        /* us += dt * tempo / ppqn */
        us += ((uint64_t)dt * (uint64_t)tempo) / (uint64_t)ppqn;
        if (raw[i].type == 0) {
            tempo = raw[i].tempo;
            continue;
        }
        out[out_n].t_us = (uint32_t)(us > 0xffffffffull ? 0xffffffffull : us);
        out[out_n].type = raw[i].type;
        out[out_n].note = raw[i].note;
        out[out_n].vel = raw[i].vel;
        out[out_n].ch = raw[i].ch;
        out_n++;
    }
    ag_free(raw);

    if (out_n < 1) {
        ag_free(out);
        return -1;
    }
    p->ev = out;
    p->nev = out_n;
    p->len_us = out[out_n - 1].t_us + 250000u; /* pad 250 ms after last */
    p->iev = 0;
    p->pos_us = 0;
    p->playing = 0;
    p->loop = 1;
    return 0;
}

void ag_mid_start(ag_mid_player_t *p)
{
    if (p == NULL || p->nev < 1) {
        return;
    }
    p->iev = 0;
    p->pos_us = 0;
    p->playing = 1;
}

void ag_mid_stop(ag_mid_player_t *p)
{
    if (p == NULL) {
        return;
    }
    p->playing = 0;
}

void ag_mid_set_loop(ag_mid_player_t *p, int on)
{
    if (p != NULL) {
        p->loop = on ? 1 : 0;
    }
}

void ag_mid_advance(ag_mid_player_t *p, uint32_t frames, uint32_t sample_rate,
                    ag_mid_note_fn on_note, ag_mid_prog_fn on_prog, void *ctx)
{
    uint64_t dt_us;
    if (p == NULL || !p->playing || p->nev < 1 || sample_rate < 1u) {
        return;
    }
    dt_us = ((uint64_t)frames * 1000000ull) / (uint64_t)sample_rate;
    if (dt_us < 1u) {
        dt_us = 1u;
    }
    p->pos_us += dt_us;

    for (;;) {
        while (p->iev < p->nev && (uint64_t)p->ev[p->iev].t_us <= p->pos_us) {
            ag_mid_ev_t *e = &p->ev[p->iev++];
            if (e->type == AG_MID_EV_ON || e->type == AG_MID_EV_OFF) {
                if (on_note != NULL) {
                    on_note(ctx, e->type == AG_MID_EV_ON, e->note, e->vel,
                            e->ch);
                }
            } else if (e->type == AG_MID_EV_PROG) {
                if (on_prog != NULL) {
                    on_prog(ctx, e->note, e->ch); /* prog in note field */
                }
            }
        }
        if (p->iev < p->nev || p->pos_us < p->len_us) {
            break;
        }
        /* end of song */
        if (!p->loop) {
            p->playing = 0;
            break;
        }
        if (on_note != NULL) {
            /* release everything before restart */
            on_note(ctx, 0, 0xffu, 0, 0); /* special: all notes off */
        }
        p->iev = 0;
        p->pos_us = 0;
    }
}
