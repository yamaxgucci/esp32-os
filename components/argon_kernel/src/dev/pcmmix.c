/*
 * ArgonOS - software PCM mixer (/dev/pcmmix).
 *
 * Several apps may open this device at once.  Each open gets a private ring;
 * write() pushes interleaved s16 frames into that ring.  Whenever any client
 * has a full mix period, samples are summed (int32 → saturating s16) and
 * written to a single exclusive sink: pcmvirt if present, then pcmrem, then
 * pcm0, else pcmnull.  Missing clients contribute silence for that period.
 *
 * Each of those is looked for HERE and then on the node next door, so a board
 * with no converter of its own plays through its neighbour rather than into
 * pcmnull - quality first, distance only as the tie-break.  See devdefault.c.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "argon/audio.h"

#include <string.h>

#include <argon/port/mem.h>

#include "argon/board.h"
#include "argon/device.h"
#include "argon/devdefault.h"
#include "argon/log.h"

#define TAG "pcmmix"

#define PCMMIX_CLIENTS     4
/* members of a room group played into beside the sink (multiroom) */
#define PCMMIX_ALSO        3
/*
 * 16 KB per client, which is 186 ms of 22 kHz stereo, and not four.
 *
 * A client's ring is what stands between "the program produced sound" and "the
 * sink was ready for it", and on this machine the two can be a long way apart:
 * the kernel holds the device registry across a whole blit, so while a screen
 * is being pushed down a serial link the task with the samples cannot get in.
 * Four kilobytes is 46 ms and the surplus is dropped silently - which is where
 * the game's music went while its picture was being sent.
 */
#define PCMMIX_RING_BYTES  (16u * 1024u)
#define PCMMIX_PERIOD      256u /* frames per mix tick */

typedef struct pcmmix_client {
    int            used;
    ag_audio_fmt_t fmt;
    int            fmt_set;
    uint8_t       *ring; /* PCMMIX_RING_BYTES, allocated while the client is open */
    size_t         ring_r;
    size_t         ring_w;
    size_t         ring_used;
    uint64_t       bytes_in;
    uint64_t       bytes_drop_overflow;
    uint32_t       overflow_events;
} pcmmix_client_t;

typedef struct {
    ag_audio_fmt_t   mix_fmt;
    int              mix_fmt_set;
    pcmmix_client_t  clients[PCMMIX_CLIENTS];
    int              client_count;

    ag_device_t     *sink;
    int              sink_held;
    char             sink_name[AG_DEV_NAME_MAX];
    /* the other members of a chosen group (multiroom), open beside `sink` */
    ag_device_t     *also[PCMMIX_ALSO];
    int              also_n;

    uint64_t         bytes_out;
    uint64_t         bytes_drop_nosink;
    uint32_t         underrun_events;
    int              nosink_said; /* so a silence is reported once, not per period */
    int16_t         *mix_scratch; /* PCMMIX_PERIOD * 2, heap */
} pcmmix_state_t;

static void *pcmmix_alloc(size_t n); /* PSRAM first, then internal */

static pcmmix_state_t *s_st; /* heap/PSRAM — keep DRAM BSS tight */
static ag_device_t    *s_dev;

static void default_fmt(ag_audio_fmt_t *out)
{
    const ag_board_audio_t *a = &ag_board()->audio;
    out->rate = a->rate ? a->rate : 22050u;
    out->channels = 2;
    out->bits = 16;
}

static int fmt_ok(const ag_audio_fmt_t *fmt)
{
    return fmt != NULL && fmt->rate >= 8000u && fmt->rate <= 48000u &&
           fmt->bits == 16u && (fmt->channels == 1u || fmt->channels == 2u);
}

static int fmt_eq(const ag_audio_fmt_t *a, const ag_audio_fmt_t *b)
{
    return a->rate == b->rate && a->channels == b->channels &&
           a->bits == b->bits;
}

static size_t frame_bytes(const ag_audio_fmt_t *fmt)
{
    return (size_t)fmt->channels * sizeof(int16_t);
}

static void ring_discard_oldest(pcmmix_client_t *c, size_t n)
{
    if (c->ring == NULL) {
        return;
    }
    if (n > c->ring_used) {
        n = c->ring_used;
    }
    c->ring_r = (c->ring_r + n) % PCMMIX_RING_BYTES;
    c->ring_used -= n;
    c->bytes_drop_overflow += (uint64_t)n;
    c->overflow_events++;
}

static void ring_push(pcmmix_client_t *c, const uint8_t *src, size_t n)
{
    size_t space;
    size_t first;

    if (n == 0u || c->ring == NULL) {
        return;
    }
    if (n > PCMMIX_RING_BYTES) {
        src += (n - PCMMIX_RING_BYTES);
        c->bytes_drop_overflow += (uint64_t)(n - PCMMIX_RING_BYTES);
        c->overflow_events++;
        n = PCMMIX_RING_BYTES;
    }
    space = PCMMIX_RING_BYTES - c->ring_used;
    if (n > space) {
        ring_discard_oldest(c, n - space);
    }
    first = PCMMIX_RING_BYTES - c->ring_w;
    if (first > n) {
        first = n;
    }
    memcpy(c->ring + c->ring_w, src, first);
    if (n > first) {
        memcpy(c->ring, src + first, n - first);
    }
    c->ring_w = (c->ring_w + n) % PCMMIX_RING_BYTES;
    c->ring_used += n;
}

static size_t ring_pop(pcmmix_client_t *c, uint8_t *dst, size_t n)
{
    size_t first;

    if (c->ring == NULL) {
        return 0;
    }
    if (n > c->ring_used) {
        n = c->ring_used;
    }
    if (n == 0u) {
        return 0;
    }
    first = PCMMIX_RING_BYTES - c->ring_r;
    if (first > n) {
        first = n;
    }
    memcpy(dst, c->ring + c->ring_r, first);
    if (n > first) {
        memcpy(dst + first, c->ring, n - first);
    }
    c->ring_r = (c->ring_r + n) % PCMMIX_RING_BYTES;
    c->ring_used -= n;
    return n;
}

static void ring_clear(pcmmix_client_t *c)
{
    c->ring_r = 0;
    c->ring_w = 0;
    c->ring_used = 0;
}

/*
 * Best first, and this order is a fact about SOUND rather than about devices:
 * a helper on the development machine, the wire to a second board, then the
 * board's own converter, then the one that swallows it.  The registry cannot
 * derive this and should not try - which is why the list stays here and only
 * the looking moved out.
 *
 * pcmmix itself is absent from the list deliberately.  It is of class
 * AG_DEV_AUDIO too, and a mixer that could choose itself as its own output is
 * a loop with no bottom.
 *
 * pcmvirt and pcmrem are both published by a loadable driver, and a driver was
 * loaded on purpose: nobody installs one and then wants the built-in output
 * instead.  pcmrem is the wire to a second board (apps/remdisp), which on a
 * machine whose amplifier is unsoldered is the only output there is - so it has
 * to come ahead of pcm0, which would otherwise win by existing and play to
 * nothing at all, indistinguishably from working.
 */
static const char *const k_audio_prefer[] = {"pcmvirt", "pcmrem", "pcm0",
                                             "pcmnull", NULL};

/*
 * HERE OR ON THE NODE NEXT DOOR, and the road back to that line was the whole
 * point of the lock work below it.
 *
 * This asked only for local hardware for two days, and the retreat was written
 * down rather than quietly made: the mixer did choose the neighbour's
 * converter, it worked, and then it intermittently froze the board - console
 * stops answering and does not come back, in about half of runs.
 *
 * The cause, now proved rather than suspected: the registry held ONE recursive
 * mutex across the whole of a driver's call, and the mixer's reach across the
 * wire happens inside one.  While it waited for a round trip, nothing on this
 * board could touch any device at all, the console included.  Bounding the
 * wait to a second and a half changed nothing, which was the clue that the
 * waiting was not the problem - the LOCK ACROSS THE WAITING was.
 *
 * An application opening a far device itself was never affected, and that is
 * the same fact from the other side: it holds no other device's lock while it
 * waits.
 *
 * The registry now lets its lock go for the length of a driver call and holds
 * that device's own lock instead - open and close included, which is what this
 * path uses.  So the question can be asked of the whole system again.
 */
static ag_device_t *pick_sink(void)
{
    return ag_dev_default(AG_DEV_AUDIO, k_audio_prefer);
}

/*
 * The sink went away under us.
 *
 * This was safe to leave out for as long as a sink was a kernel device that
 * outlives everything.  It stopped being safe the moment the sink can be the
 * NODE NEXT DOOR: a proxy disappears when the link drops or when somebody
 * detaches it, and a held pointer into a slot that has been cleared is the
 * use-after-free this tree has already had twice - the kind that does not
 * crash, but answers with zeros.
 *
 * The registry lock is held here, so nothing in the registry may be called:
 * not even the close that release_sink would do.  Drop the pointer and return.
 * The mixer picks again on its next period, which is where the recovery
 * belongs anyway - "the sound moved to the other board" should not need anyone
 * to notice and act.
 */
static void on_revoke(ag_device_t *dev)
{
    if (s_st != NULL && s_st->sink == dev) {
        s_st->sink = NULL;
        s_st->sink_held = 0;
        s_st->sink_name[0] = '\0';
    }
    /* a room of a group left: the others play on */
    for (int i = 0; s_st != NULL && i < s_st->also_n; i++) {
        if (s_st->also[i] == dev) {
            s_st->also[i] = s_st->also[--s_st->also_n];
            i--;
        }
    }
}

static void release_also(pcmmix_state_t *st)
{
    for (int i = 0; i < st->also_n; i++) {
        (void)ag_dev_close(st->also[i]);
    }
    st->also_n = 0;
}

/*
 * THE REST OF A GROUP, beside the sink.  Opened, set to the mixer's format
 * and written every period after the sink - so each member is paced by its
 * own write, as the one sink is (a neighbour's converter by its proxy's
 * clock, noderdev.c pace_audio).  Asked every period like the sink, so a
 * member that appears joins and one that goes is let go.
 */
static void ensure_also(pcmmix_state_t *st)
{
    ag_device_t *g[1 + PCMMIX_ALSO];
    const int    n = ag_dev_default_group(AG_DEV_AUDIO, g, 1 + PCMMIX_ALSO);
    int          want_n = 0;
    ag_device_t *want[PCMMIX_ALSO];
    for (int i = 0; i < n; i++) {
        if (g[i] != st->sink && want_n < PCMMIX_ALSO) {
            want[want_n++] = g[i];
        }
    }
    bool same = (want_n == st->also_n);
    for (int i = 0; same && i < want_n; i++) {
        same = (want[i] == st->also[i]);
    }
    if (same) {
        return;
    }
    release_also(st);
    for (int i = 0; i < want_n; i++) {
        if (ag_dev_open(want[i], AG_O_WRONLY) != AG_OK) {
            continue;
        }
        ag_audio_fmt_t fmt = st->mix_fmt;
        (void)ag_dev_ioctl(want[i], AG_IOC_AUDIO_SETFMT, &fmt, sizeof(fmt));
        st->also[st->also_n++] = want[i];
        ag_log(AG_LOG_INFO, TAG, "and /dev/%s, as the same room group",
               want[i]->name);
    }
}

static void release_sink(pcmmix_state_t *st)
{
    release_also(st);
    if (st->sink_held && st->sink != NULL) {
        (void)ag_dev_close(st->sink);
    }
    st->sink_held = 0;
    st->sink = NULL;
    st->sink_name[0] = '\0';
}

static ag_err_t ensure_sink(pcmmix_state_t *st)
{
    ag_device_t *want;
    ag_err_t     err;

    want = pick_sink();
    if (want == NULL) {
        return -AG_ENODEV;
    }
    if (st->sink_held && st->sink == want) {
        return AG_OK;
    }
    if (st->sink_held) {
        release_sink(st);
    }

    err = ag_dev_open(want, AG_O_WRONLY);
    if (err != AG_OK) {
        return err;
    }
    {
        ag_audio_fmt_t fmt = st->mix_fmt;
        (void)ag_dev_ioctl(want, AG_IOC_AUDIO_SETFMT, &fmt, sizeof(fmt));
    }
    st->sink = want;
    st->sink_held = 1;
    {
        size_t i = 0;
        while (want->name[i] && i + 1u < sizeof(st->sink_name)) {
            st->sink_name[i] = want->name[i];
            i++;
        }
        st->sink_name[i] = '\0';
    }
    ag_log(AG_LOG_INFO, TAG, "sink /dev/%s @ %u Hz %u ch", st->sink_name,
           (unsigned)st->mix_fmt.rate, (unsigned)st->mix_fmt.channels);
    return AG_OK;
}

static int16_t sat16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

static int client_has_period(const pcmmix_client_t *c, size_t fb)
{
    return c->used && c->fmt_set && c->ring_used >= fb * PCMMIX_PERIOD;
}

static void mix_one_period(pcmmix_state_t *st)
{
    const size_t fb = frame_bytes(&st->mix_fmt);
    const size_t ch = (size_t)st->mix_fmt.channels;
    const size_t period_bytes = fb * PCMMIX_PERIOD;
    uint8_t      tmp[PCMMIX_PERIOD * 2u * sizeof(int16_t)];
    size_t       i;
    size_t       s;
    int32_t      acc[2];
    int          any_data = 0;
    int32_t      n;

    memset(st->mix_scratch, 0, period_bytes);

    for (i = 0; i < PCMMIX_CLIENTS; i++) {
        pcmmix_client_t *c = &st->clients[i];
        size_t           got;

        if (!c->used || !c->fmt_set) {
            continue;
        }
        got = ring_pop(c, tmp, period_bytes);
        if (got < period_bytes) {
            if (got > 0u) {
                memset(tmp + got, 0, period_bytes - got);
            } else {
                st->underrun_events++;
                continue;
            }
            st->underrun_events++;
        }
        any_data = 1;
        for (s = 0; s < PCMMIX_PERIOD; s++) {
            const int16_t *in =
                (const int16_t *)(tmp + s * fb);
            if (ch == 1u) {
                int32_t v = (int32_t)st->mix_scratch[s] + (int32_t)in[0];
                st->mix_scratch[s] = sat16(v);
            } else {
                acc[0] = (int32_t)st->mix_scratch[s * 2u] + (int32_t)in[0];
                acc[1] =
                    (int32_t)st->mix_scratch[s * 2u + 1u] + (int32_t)in[1];
                st->mix_scratch[s * 2u] = sat16(acc[0]);
                st->mix_scratch[s * 2u + 1u] = sat16(acc[1]);
            }
        }
    }

    if (!any_data) {
        return;
    }

    {
        const ag_err_t serr = ensure_sink(st);
        if (serr != AG_OK) {
            st->bytes_drop_nosink += (uint64_t)period_bytes;
            /*
             * Once per spell of silence, not once per period - a mixer that
             * cannot place its output produces a period every few milliseconds
             * and would bury the console.  Said at all because the symptom
             * otherwise is silence, and silence is the one symptom that says
             * nothing about its cause: a counter nobody reads is not an answer.
             */
            if (!st->nosink_said) {
                st->nosink_said = 1;
                ag_log(AG_LOG_WARN, TAG, "nowhere to send sound: %d",
                       (int)serr);
            }
            return;
        }
        st->nosink_said = 0;
    }
    n = ag_dev_write(st->sink, st->mix_scratch, period_bytes, 0);
    if (n > 0) {
        st->bytes_out += (uint64_t)n;
    } else {
        st->bytes_drop_nosink += (uint64_t)period_bytes;
    }
    ensure_also(st);
    for (i = 0; i < (size_t)st->also_n; i++) {
        (void)ag_dev_write(st->also[i], st->mix_scratch, period_bytes, 0);
    }
}

static void mix_drain(pcmmix_state_t *st)
{
    const size_t fb = frame_bytes(&st->mix_fmt);
    int          guard = 32;

    if (!st->mix_fmt_set) {
        return;
    }
    while (guard-- > 0) {
        int ready = 0;
        size_t i;
        for (i = 0; i < PCMMIX_CLIENTS; i++) {
            if (client_has_period(&st->clients[i], fb)) {
                ready = 1;
                break;
            }
        }
        if (!ready) {
            break;
        }
        mix_one_period(st);
    }
}

static pcmmix_client_t *alloc_client(pcmmix_state_t *st)
{
    size_t i;
    for (i = 0; i < PCMMIX_CLIENTS; i++) {
        if (!st->clients[i].used) {
            pcmmix_client_t *c = &st->clients[i];
            uint8_t *ring = (uint8_t *)pcmmix_alloc(PCMMIX_RING_BYTES);
            if (ring == NULL) {
                return NULL; /* no memory for a ring is no client */
            }
            memset(c, 0, sizeof(*c));
            c->ring = ring;
            c->used = 1;
            st->client_count++;
            return c;
        }
    }
    return NULL;
}

static ag_err_t mix_open_session(ag_device_t *dev, uint32_t flags,
                                 void **session)
{
    pcmmix_client_t *c;

    (void)dev;
    (void)flags;
    if (session == NULL || s_st == NULL) {
        return -AG_EINVAL;
    }
    c = alloc_client(s_st);
    if (c == NULL) {
        return -AG_ENFILE;
    }
    if (!s_st->mix_fmt_set) {
        default_fmt(&s_st->mix_fmt);
        s_st->mix_fmt_set = 1;
    }
    c->fmt = s_st->mix_fmt;
    c->fmt_set = 1;
    *session = c;
    return AG_OK;
}

static ag_err_t mix_close_session(ag_device_t *dev, void *session)
{
    pcmmix_client_t *c = (pcmmix_client_t *)session;

    (void)dev;
    if (c == NULL || !c->used) {
        return -AG_EBADF;
    }
    c->used = 0;
    ring_clear(c);
    ag_port_free(c->ring);
    c->ring = NULL;
    if (s_st->client_count > 0) {
        s_st->client_count--;
    }
    if (s_st->client_count == 0) {
        release_sink(s_st);
    }
    return AG_OK;
}

static ag_err_t mix_ioctl_session(ag_device_t *dev, void *session, uint32_t cmd,
                                  void *arg, size_t arglen)
{
    pcmmix_client_t *c = (pcmmix_client_t *)session;

    (void)dev;
    if (c == NULL || !c->used) {
        return -AG_EBADF;
    }

    if (cmd == AG_IOC_AUDIO_GETFMT) {
        if (arg == NULL || arglen < sizeof(ag_audio_fmt_t)) {
            return -AG_EINVAL;
        }
        if (!s_st->mix_fmt_set) {
            default_fmt(&s_st->mix_fmt);
            s_st->mix_fmt_set = 1;
        }
        *(ag_audio_fmt_t *)arg = s_st->mix_fmt;
        return AG_OK;
    }
    if (cmd == AG_IOC_AUDIO_SETFMT) {
        const ag_audio_fmt_t *fmt;
        if (arg == NULL || arglen < sizeof(ag_audio_fmt_t)) {
            return -AG_EINVAL;
        }
        fmt = (const ag_audio_fmt_t *)arg;
        if (!fmt_ok(fmt)) {
            return -AG_EINVAL;
        }
        if (!s_st->mix_fmt_set) {
            s_st->mix_fmt = *fmt;
            s_st->mix_fmt_set = 1;
        } else if (!fmt_eq(&s_st->mix_fmt, fmt)) {
            /*
             * Shared clock: later clients must match.  The sole open client may
             * still retarget before any mixed audio has left the sink.
             */
            if (s_st->client_count > 1 || s_st->bytes_out > 0u) {
                return -AG_EINVAL;
            }
            s_st->mix_fmt = *fmt;
            if (s_st->sink_held) {
                release_sink(s_st);
            }
        }
        c->fmt = s_st->mix_fmt;
        c->fmt_set = 1;
        if (s_st->sink_held && s_st->sink != NULL) {
            ag_audio_fmt_t f = s_st->mix_fmt;
            (void)ag_dev_ioctl(s_st->sink, AG_IOC_AUDIO_SETFMT, &f, sizeof(f));
        }
        for (int k = 0; k < s_st->also_n; k++) {
            ag_audio_fmt_t f = s_st->mix_fmt;
            (void)ag_dev_ioctl(s_st->also[k], AG_IOC_AUDIO_SETFMT, &f, sizeof(f));
        }
        return AG_OK;
    }
    if (cmd == AG_IOC_AUDIO_GETSTATS) {
        ag_audio_stats_t *out;
        if (arg == NULL || arglen < sizeof(ag_audio_stats_t)) {
            return -AG_EINVAL;
        }
        out = (ag_audio_stats_t *)arg;
        memset(out, 0, sizeof(*out));
        out->bytes_in = c->bytes_in;
        out->bytes_sent = s_st->bytes_out;
        out->bytes_drop_noclient = s_st->bytes_drop_nosink;
        out->bytes_drop_overflow = c->bytes_drop_overflow;
        out->overflow_events = c->overflow_events;
        out->eagain_events = s_st->underrun_events;
        out->ring_used = (uint32_t)c->ring_used;
        out->ring_cap = PCMMIX_RING_BYTES;
        return AG_OK;
    }
    if (cmd == AG_IOC_FLUSH || cmd == AG_IOC_RESET) {
        ring_clear(c);
        return AG_OK;
    }
    return -AG_ENOTSUP;
}

static int32_t mix_write_session(ag_device_t *dev, void *session,
                                 const void *buf, size_t len, uint64_t off)
{
    pcmmix_client_t *c = (pcmmix_client_t *)session;
    size_t           fb;
    size_t           frames;
    size_t           bytes;

    (void)dev;
    (void)off;
    if (c == NULL || !c->used) {
        return -AG_EBADF;
    }
    if (buf == NULL || len < 2u) {
        return -AG_EINVAL;
    }
    if (!s_st->mix_fmt_set) {
        default_fmt(&s_st->mix_fmt);
        s_st->mix_fmt_set = 1;
    }
    if (!c->fmt_set) {
        c->fmt = s_st->mix_fmt;
        c->fmt_set = 1;
    }

    fb = frame_bytes(&s_st->mix_fmt);
    frames = len / fb;
    if (frames == 0u) {
        return -AG_EINVAL;
    }
    bytes = frames * fb;
    ring_push(c, (const uint8_t *)buf, bytes);
    c->bytes_in += (uint64_t)bytes;
    mix_drain(s_st);
    return (int32_t)bytes;
}

static const ag_dev_ops_t k_mix_ops = {
    .open_session = mix_open_session,
    .close_session = mix_close_session,
    .write_session = mix_write_session,
    .ioctl_session = mix_ioctl_session,
};

static void *pcmmix_alloc(size_t n)
{
    void *p = ag_port_alloc(n, AG_MEM_SLOW | AG_MEM_BYTE);
    if (p == NULL) {
        p = ag_port_alloc(n, AG_MEM_FAST | AG_MEM_BYTE);
    }
    return p;
}

ag_err_t ag_pcmmix_init(void)
{
    const size_t scratch_bytes = PCMMIX_PERIOD * 2u * sizeof(int16_t);
    ag_dev_desc_t desc;
    ag_err_t      err;
    size_t        i;

    s_st = (pcmmix_state_t *)pcmmix_alloc(sizeof(*s_st));
    if (s_st == NULL) {
        return -AG_ENOMEM;
    }
    memset(s_st, 0, sizeof(*s_st));
    default_fmt(&s_st->mix_fmt);
    s_st->mix_fmt_set = 1;

    /*
     * No ring pool here any more.  Four clients' rings at sixteen kilobytes
     * each is sixty-four, taken at boot whether anything ever opens the mixer
     * or not - and on the CYD, which has 179 KB and plays its sound through
     * pcm0 directly, that was the memory the picture receiver needed to load
     * at all (`load remterm.axe failed (-12)`).  A ring is allocated when a
     * client opens and freed when it closes; a board that never mixes never
     * pays.
     */
    s_st->mix_scratch = (int16_t *)pcmmix_alloc(scratch_bytes);
    if (s_st->mix_scratch == NULL) {
        ag_port_free(s_st);
        s_st = NULL;
        return -AG_ENOMEM;
    }
    memset(s_st->mix_scratch, 0, scratch_bytes);
    (void)i;

    memset(&desc, 0, sizeof(desc));
    desc.name = "pcmmix";
    desc.driver = "mix";
    desc.cls = AG_DEV_AUDIO;
    desc.flags = AG_DEVF_DMA;
    desc.ops = &k_mix_ops;
    desc.priv = s_st;
    err = ag_dev_register(&desc, &s_dev);
    if (err != AG_OK) {
        ag_port_free(s_st->mix_scratch);
        ag_port_free(s_st);
        s_st = NULL;
        return err;
    }
    (void)ag_dev_on_revoke(on_revoke);
    ag_log(AG_LOG_INFO, TAG,
           "pcmmix ready (sum %u clients → pcmvirt|pcmrem|pcm0|pcmnull, here "
           "or on the node next door)",
           (unsigned)PCMMIX_CLIENTS);
    return AG_OK;
}
