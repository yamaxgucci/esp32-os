/*
 * ArgonOS Mega Drive - save states: the machine to a file and back.
 *
 * The cores write their state through gwenesis' tag API (gwenesis_*_save_state);
 * here it goes to a file as it comes - a tag, a length, the bytes - and is read
 * back in the same order, each tag checked, so nothing as large as the state is
 * ever held in memory.  What the small build keeps its own way is written here
 * beside it: the machine's memory in pages (md_pages.h), VRAM by words (some of
 * its pages are IRAM, which takes no byte access), and the Z80, the YM and the
 * PSG of this port, which gwenesis does not know.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include <stdint.h>
#include <string.h>

#include "gwenesis_savestate.h"
#include "md_savestate.h"

#ifdef ARGON_MD_SMALL
#include "gwenesis_vdp.h"
#include "md_pages.h"
#endif

#define MD_STATE_MAGIC   "MDS2"
#define MD_STATE_IO      256
#define MD_STATE_TAG_MAX 40

static ag_handle_t s_f = AG_INVALID_HANDLE;
static int         s_writing;
static int         s_bad;     /* a write short, or a tag out of step on load */
static uint8_t    *s_bounce;  /* a page's worth, for the pages that take words */
static uint8_t    *s_io;      /* the file's bytes, a buffer at a time */
static size_t      s_io_n, s_io_at;

/* any non-NULL pointer: the cores only hand it back */
SaveState *saveGwenesisStateOpenForRead(const char *fileName)
{
    (void)fileName;
    return (s_f >= 0 && !s_writing) ? (SaveState *)&s_f : 0;
}

SaveState *saveGwenesisStateOpenForWrite(const char *fileName)
{
    (void)fileName;
    return (s_f >= 0 && s_writing) ? (SaveState *)&s_f : 0;
}

static void io_flush(void)
{
    if (s_io_n != 0u && !s_bad && ag_write(s_f, s_io, s_io_n) != (int32_t)s_io_n) {
        s_bad = 1;
    }
    s_io_n = 0;
}

static void io_out(uint8_t b)
{
    s_io[s_io_n++] = b;
    if (s_io_n == MD_STATE_IO) {
        io_flush();
    }
}

static uint8_t io_in(void)
{
    if (s_io_at == s_io_n) {
        const int32_t r = s_bad ? 0 : ag_read(s_f, s_io, MD_STATE_IO);
        if (r <= 0) {
            s_bad = 1;
            return 0;
        }
        s_io_n = (size_t)r;
        s_io_at = 0;
    }
    return s_io[s_io_at++];
}

/*
 * PackBits, one call's bytes at a time (a load asks for exactly what a save
 * wrote, call for call): a header under 128 is that many and one bytes as
 * they are, over 128 the next byte 257 - header times.  The machine's memory
 * is mostly runs - 144 KB a state on C:, whose whole is a megabyte, without.
 */
static void put(const void *p, size_t n)
{
    const uint8_t *const b = (const uint8_t *)p;
    size_t i = 0;
    while (i < n && !s_bad) {
        size_t run = 1;
        while (i + run < n && run < 128u && b[i + run] == b[i]) {
            run++;
        }
        if (run >= 3u) {
            io_out((uint8_t)(257u - run));
            io_out(b[i]);
            i += run;
            continue;
        }
        size_t lit = 0;
        while (i + lit < n && lit < 128u &&
               !(i + lit + 2u < n && b[i + lit] == b[i + lit + 1u] &&
                 b[i + lit] == b[i + lit + 2u])) {
            lit++;
        }
        io_out((uint8_t)(lit - 1u));
        for (size_t k = 0; k < lit; k++) {
            io_out(b[i + k]);
        }
        i += lit;
    }
}

static void get(void *p, size_t n)
{
    uint8_t *const b = (uint8_t *)p;
    size_t i = 0;
    while (i < n && !s_bad) {
        const uint8_t h = io_in();
        if (h < 128u) {
            const size_t k = (size_t)h + 1u;
            if (i + k > n) {
                s_bad = 1;
                return;
            }
            for (size_t j = 0; j < k; j++) {
                b[i++] = io_in();
            }
        } else if (h > 128u) {
            const size_t k = 257u - h;
            if (i + k > n) {
                s_bad = 1;
                return;
            }
            memset(b + i, io_in(), k);
            i += k;
        } else {
            s_bad = 1;
        }
    }
}

static size_t tag_len(const char *tag)
{
    size_t n = 0;
    while (n < MD_STATE_TAG_MAX && tag[n] != 0) {
        n++;
    }
    return n;
}

static void put_tag(const char *tag, uint32_t len)
{
    const uint8_t n = (uint8_t)tag_len(tag);
    put(&n, 1);
    put(tag, n);
    put(&len, sizeof(len));
}

/* the next record is `tag` of `len` bytes, or the load is off */
static int get_tag(const char *tag, uint32_t len)
{
    uint8_t  n = 0;
    char     t[MD_STATE_TAG_MAX];
    uint32_t l = 0;
    get(&n, 1);
    if (s_bad || n > sizeof(t)) {
        s_bad = 1;
        return 0;
    }
    get(t, n);
    get(&l, sizeof(l));
    if (s_bad || n != tag_len(tag) || memcmp(t, tag, n) != 0 ||
        l != len) {
        s_bad = 1;
        return 0;
    }
    return 1;
}

void saveGwenesisStateSet(SaveState *state, const char *tagName, int value)
{
    if (state == 0) {
        return;
    }
    const int32_t v = (int32_t)value;
    put_tag(tagName, sizeof(v));
    put(&v, sizeof(v));
}

int saveGwenesisStateGet(SaveState *state, const char *tagName)
{
    int32_t v = 0;
    if (state != 0 && get_tag(tagName, sizeof(v))) {
        get(&v, sizeof(v));
    }
    return (int)v;
}

void saveGwenesisStateSetBuffer(SaveState *state, const char *tagName,
                                void *buffer, int length)
{
    if (state == 0) {
        return;
    }
    put_tag(tagName, (uint32_t)length);
    put(buffer, (size_t)length);
}

void saveGwenesisStateGetBuffer(SaveState *state, const char *tagName,
                                void *buffer, int length)
{
    if (state != 0 && get_tag(tagName, (uint32_t)length)) {
        get(buffer, (size_t)length);
    }
}

#ifdef ARGON_MD_SMALL
/* the port's own parts (md_z80.c, md_psg.c, ym2612.c) */
extern void md_z80_state(void *buf, size_t *len, int load);
extern void md_psg_state(void *buf, size_t *len, int load);
extern void md_ym_state(void *buf, size_t *len, int load);

/* One part of the port through a scratch block: its size asked first. */
static void port_part(const char *tag, void (*fn)(void *, size_t *, int))
{
    size_t n = 0;
    fn(NULL, &n, 0);
    void *b = ag_malloc(n);
    if (b == NULL) {
        s_bad = 1;
        return;
    }
    if (s_writing) {
        fn(b, &n, 0);
        put_tag(tag, (uint32_t)n);
        put(b, n);
    } else if (get_tag(tag, (uint32_t)n)) {
        get(b, n);
        if (!s_bad) {
            fn(b, &n, 1);
        }
    }
    ag_free(b);
}

static void pages(const char *tag, unsigned char **pg, unsigned count)
{
    if (s_writing) {
        put_tag(tag, count * MD_PAGE);
        for (unsigned i = 0; i < count; i++) {
            put(pg[i], MD_PAGE);
        }
    } else if (get_tag(tag, count * MD_PAGE)) {
        for (unsigned i = 0; i < count; i++) {
            get(pg[i], MD_PAGE);
        }
    }
}

/* VRAM a word at a time, through the bounce page */
static void vram_pages(void)
{
    if (s_writing) {
        put_tag("VRAMP", MD_VRAM_PAGES * MD_PAGE);
    } else if (!get_tag("VRAMP", MD_VRAM_PAGES * MD_PAGE)) {
        return;
    }
    uint32_t *const w = (uint32_t *)(void *)s_bounce;
    for (unsigned i = 0; i < MD_VRAM_PAGES && !s_bad; i++) {
        volatile uint32_t *const pg = (volatile uint32_t *)(void *)md_vram[i];
        if (s_writing) {
            for (unsigned k = 0; k < MD_PAGE / 4u; k++) {
                w[k] = pg[k];
            }
            put(w, MD_PAGE);
        } else {
            get(w, MD_PAGE);
            for (unsigned k = 0; k < MD_PAGE / 4u; k++) {
                pg[k] = w[k];
            }
        }
    }
}
#endif

static void whole_machine(void)
{
    if (s_writing) {
        gwenesis_save_state();
    } else {
        gwenesis_load_state();
    }
#ifdef ARGON_MD_SMALL
    pages("RAMP", md_ram, MD_RAM_PAGES);
    /* without the Z80 its pages are one page many times: once is enough */
    pages("ZRAMP", md_zram, (md_zram[1] == md_zram[0]) ? 1u : MD_ZRAM_PAGES);
    vram_pages();
    port_part("z80", md_z80_state);
    port_part("ym", md_ym_state);
    port_part("psg", md_psg_state);
#endif
}

static int run(const char *path, int writing)
{
    s_f = ag_open(path, writing ? (AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC)
                                : AG_O_RDONLY);
    if (s_f < 0) {
        s_f = AG_INVALID_HANDLE;
        return MD_STATE_NO_FILE;
    }
    s_writing = writing;
    s_bad = 0;
    s_bounce = (uint8_t *)ag_malloc(MD_STATE_BOUNCE + MD_STATE_IO);
    s_io = s_bounce + MD_STATE_BOUNCE;
    s_io_n = s_io_at = 0;
    if (s_bounce == NULL) {
        (void)ag_close(s_f);
        s_f = AG_INVALID_HANDLE;
        return MD_STATE_NO_MEMORY;
    }
    char magic[4];
    if (writing) {
        put(MD_STATE_MAGIC, 4);
    } else {
        get(magic, 4);
        if (!s_bad && memcmp(magic, MD_STATE_MAGIC, 4) != 0) {
            s_bad = 1;
        }
    }
    if (!s_bad) {
        whole_machine();
    }
    if (writing) {
        io_flush();
    }
    ag_free(s_bounce);
    s_bounce = NULL;
    (void)ag_close(s_f);
    s_f = AG_INVALID_HANDLE;
    return s_bad ? MD_STATE_BAD : MD_STATE_OK;
}

int md_state_save(const char *path)
{
    return run(path, 1);
}

int md_state_load(const char *path)
{
    return run(path, 0);
}
