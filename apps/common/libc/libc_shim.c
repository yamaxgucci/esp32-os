/*
 * Minimal libc stand-ins so SMS Plus GX can build as an ArgonOS .AXE (-nostdlib).
 * SPDX-License-Identifier: Apache-2.0 (shim only; SMS core remains GPLv2+)
 */
#include <argon/argon.h>
#include <stdarg.h>
#include <stdint.h>

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "math.h"
#include "ctype.h"
#include "strings.h"
#include "errno.h"
#include <sys/types.h>

int errno;

static FILE s_stdin = {-4, 0, 0};
static FILE s_stdout = {-2, 0, 0};
static FILE s_stderr = {-3, 0, 0};
FILE *stdin = &s_stdin;
FILE *stdout = &s_stdout;
FILE *stderr = &s_stderr;

/*
 * A failed allocation is the quietest failure there is.  A vendored engine
 * checks the pointer, gives up on whatever it was doing, and reports something
 * unrelated three layers up - "Error: Loading proto message files!" when what
 * actually happened is that the arena was full.  So the first one says so.
 *
 * Once, not every time: the caller usually keeps trying, and a program that
 * has run out of memory does not need its console filled as well.
 */
static int s_malloc_failed;

/*
 * The lowest the arena has been, which is not what sampling it once a frame
 * tells you.
 *
 * A big transient - the engine reading a whole file into a buffer, copying it
 * somewhere and freeing it again - lives and dies between two frames, so a
 * per-frame watcher never sees it and reports a high-water mark that is short
 * by however large it was.  Fallout 2 decodes a map's ambience whole - half a
 * megabyte, allocated and handed on between two frames - and the difference
 * was what decided whether the game fitted in 8 MB of PSRAM.
 *
 * So the arena is sampled here as well, but only for allocations big enough to
 * matter: ag_meminfo is a call and the engine makes thirty thousand small
 * allocations before it draws anything.
 */
#define ARENA_WATCH_MIN (64u * 1024u)

static size_t s_arena_low = (size_t)-1;

size_t ag_libc_arena_low(void)
{
    return s_arena_low;
}

/*
 * The big ones, named by where they were asked for.
 *
 * A failed allocation says how many bytes it wanted and nothing about who
 * wanted them, and in a vendored engine of 128000 lines that is not a lead.
 * The return address is, once it is run through the link map - and the ones
 * before it say which phase the program was in.  Bounded, because a program
 * that streams audio asks for large blocks forever.
 *
 * Off unless asked for: a program that allocates a megabyte on purpose should
 * not have to explain itself on the console every time it runs.
 */
#define ARENA_REPORT_MIN (512u * 1024u)
#define ARENA_REPORT_MAX 12

static int s_big_reported;
static int s_big_trace;

void ag_libc_malloc_trace(int on)
{
    s_big_trace = on ? 1 : 0;
}

static void report_big(size_t n, void *ret, void *p)
{
    if (!s_big_trace || n < ARENA_REPORT_MIN ||
        s_big_reported >= ARENA_REPORT_MAX) {
        return;
    }
    s_big_reported++;
    ag_printf("malloc: %u bytes from %p%s\n", (unsigned)n, ret,
              (p == NULL) ? " FAILED" : "");
}

void *malloc(size_t n)
{
    if (n == 0) {
        n = 1;
    }
    void *p = ag_malloc(n);
    report_big(n, __builtin_return_address(0), p);
    if (p != NULL && n >= ARENA_WATCH_MIN) {
        ag_meminfo_t mi;
        ag_meminfo(&mi);
        if ((size_t)mi.arena_free < s_arena_low) {
            s_arena_low = (size_t)mi.arena_free;
        }
    }
    if (p == NULL && !s_malloc_failed) {
        ag_meminfo_t mi;
        s_malloc_failed = 1;
        ag_meminfo(&mi);
        ag_printf("malloc: out of arena asking for %u bytes; %u of %u KB free, "
                  "largest block %u\n",
                  (unsigned)n, (unsigned)(mi.arena_free / 1024u),
                  (unsigned)(mi.arena_total / 1024u),
                  (unsigned)mi.arena_largest);
    }
    return p;
}

void *calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    void  *p = malloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

void *realloc(void *p, size_t n)
{
    /* Prefer the ABI realloc: it knows the old block size (multi_heap). */
    return ag_realloc(p, n);
}

void free(void *p)
{
    if (p != NULL) {
        ag_free(p);
    }
}

/*
 * atexit, and an exit that honours it.
 *
 * Both are here rather than left to newlib because newlib's version reaches
 * for __on_exit_args and a registry the C++ runtime also wants; letting that
 * object in means two exit paths over one process.  Thirty-two is more than
 * any port has registered - the C++ statics go through __cxa_atexit in
 * apps/common/cxx, not through here.
 */
#define ATEXIT_MAX 32

static void (*s_atexit[ATEXIT_MAX])(void);
static int s_atexit_count;

int atexit(void (*fn)(void))
{
    if (fn == NULL || s_atexit_count >= ATEXIT_MAX) {
        return -1;
    }
    s_atexit[s_atexit_count++] = fn;
    return 0;
}

void exit(int code)
{
    while (s_atexit_count > 0) {
        /* Decrement first: a handler that calls exit again must not run the
         * same handler twice. */
        s_atexit[--s_atexit_count]();
    }
    ag_exit(code);
}

void abort(void) { ag_panic("abort"); }

/* What a failed assert() expands to.  Print where it happened, then panic. */
void __assert_func(const char *file, int line, const char *func,
                   const char *expr)
{
    ag_printf("assert: %s:%d %s: %s\n", file ? file : "?", line,
              func ? func : "?", expr ? expr : "?");
    ag_panic("assert");
}

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

int atoi(const char *s)
{
    int v = 0, sign = 1;
    if (s == NULL) {
        return 0;
    }
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v * sign;
}

double atof(const char *s)
{
    /* This used to be atoi in disguise, which read 0.75 as zero.  Nothing in
     * the tree had a fractional constant to notice until a game engine did. */
    return strtod(s, NULL);
}

static int fmt_out(char *buf, size_t n, size_t *pos, char c)
{
    if (*pos + 1 < n && buf != NULL) {
        buf[*pos] = c;
    }
    (*pos)++;
    return 1;
}

/*
 * %s, with the width and precision that used to be parsed and thrown away.
 *
 * Precision on a string is a maximum, not a minimum: "%.4s" of "items" is
 * "item".  Ignoring it does not look like a formatting bug when it happens -
 * Fallout builds the name of a message file that way, so the engine asked the
 * archive for pro_items.msg, was told there was no such file, and reported
 * that it could not load its prototypes.  An afternoon, that one.
 *
 * Width is a minimum, padded with spaces, left-justified when the caller said
 * '-'.  Zero padding is not honoured for strings, which is what C says.
 */
static int fmt_str(char *buf, size_t n, size_t *pos, const char *s, int width,
                   int prec, int left)
{
    int len = 0;
    int i;

    if (s == NULL) {
        s = "(null)";
    }
    while (s[len] != '\0' && (prec < 0 || len < prec)) {
        len++;
    }

    if (!left) {
        for (i = len; i < width; i++) {
            fmt_out(buf, n, pos, ' ');
        }
    }
    for (i = 0; i < len; i++) {
        fmt_out(buf, n, pos, s[i]);
    }
    if (left) {
        for (i = len; i < width; i++) {
            fmt_out(buf, n, pos, ' ');
        }
    }
    return len > width ? len : width;
}

static int fmt_uint_field(char *buf, size_t n, size_t *pos, unsigned long v,
                          int negative, int base, int upper, int width,
                          int prec, int zeropad, int left)
{
    char        tmp[32];
    char        digs[32];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int         i = 0, nd = 0, pad, min_d;
    unsigned long x = v;

    if (v == 0 && prec == 0) {
        nd = 0;
    } else {
        if (x == 0) {
            tmp[i++] = '0';
        }
        while (x != 0 && i < (int)sizeof tmp) {
            tmp[i++] = dig[x % (unsigned)base];
            x /= (unsigned)base;
        }
        while (i--) {
            digs[nd++] = tmp[i];
        }
    }
    min_d = prec >= 0 ? prec : 1;
    if (v == 0 && prec == 0) {
        min_d = 0;
    }
    while (nd < min_d && nd < (int)sizeof digs) {
        int k;
        for (k = nd; k > 0; k--) {
            digs[k] = digs[k - 1];
        }
        digs[0] = '0';
        nd++;
    }
    pad = width - nd - (negative ? 1 : 0);
    if (pad < 0) {
        pad = 0;
    }
    if (!left && !(zeropad && prec < 0)) {
        while (pad--) {
            fmt_out(buf, n, pos, ' ');
        }
        pad = 0;
    }
    if (negative) {
        fmt_out(buf, n, pos, '-');
    }
    if (!left && zeropad && prec < 0) {
        while (pad--) {
            fmt_out(buf, n, pos, '0');
        }
        pad = 0;
    }
    for (i = 0; i < nd; i++) {
        fmt_out(buf, n, pos, digs[i]);
    }
    if (left) {
        while (pad--) {
            fmt_out(buf, n, pos, ' ');
        }
    }
    return 0;
}

static int fmt_uint(char *buf, size_t n, size_t *pos, unsigned long v, int base,
                    int upper)
{
    return fmt_uint_field(buf, n, pos, v, 0, base, upper, 0, -1, 0, 0);
}

static int argon_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    size_t pos = 0;
    if (fmt == NULL) {
        if (buf != NULL && n > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            fmt_out(buf, n, &pos, *fmt);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            fmt_out(buf, n, &pos, '%');
            continue;
        }
        {
            int left = 0, zeropad = 0, width = 0, prec = -1;
            while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' ||
                   *fmt == '0') {
                if (*fmt == '-') {
                    left = 1;
                } else if (*fmt == '0') {
                    zeropad = 1;
                }
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
            if (*fmt == '.') {
                prec = 0;
                fmt++;
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt - '0');
                    fmt++;
                }
            }
            if (*fmt == 'l') {
                fmt++;
                if (*fmt == 'l') {
                    fmt++;
                }
            } else if (*fmt == 'z') {
                fmt++;
            }
            switch (*fmt) {
            case 's':
                fmt_str(buf, n, &pos, va_arg(ap, const char *), width, prec,
                        left);
                break;
            case 'c':
                fmt_out(buf, n, &pos, (char)va_arg(ap, int));
                break;
            case 'd':
            case 'i': {
                long v = va_arg(ap, int);
                if (v < 0) {
                    fmt_uint_field(buf, n, &pos, (unsigned long)(-v), 1, 10, 0,
                                   width, prec, zeropad, left);
                } else {
                    fmt_uint_field(buf, n, &pos, (unsigned long)v, 0, 10, 0,
                                   width, prec, zeropad, left);
                }
                break;
            }
            case 'u':
                fmt_uint_field(buf, n, &pos, va_arg(ap, unsigned), 0, 10, 0,
                               width, prec, zeropad, left);
                break;
            case 'x':
                fmt_uint_field(buf, n, &pos, va_arg(ap, unsigned), 0, 16, 0,
                               width, prec, zeropad, left);
                break;
            case 'X':
                fmt_uint_field(buf, n, &pos, va_arg(ap, unsigned), 0, 16, 1,
                               width, prec, zeropad, left);
                break;
        case 'p':
            fmt_str(buf, n, &pos, "0x", 0, -1, 0);
            fmt_uint(buf, n, &pos, (unsigned long)va_arg(ap, void *), 16, 0);
            break;
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            double dv = va_arg(ap, double);
            long iv = (long)dv;
            unsigned frac;
            if (iv < 0) {
                fmt_out(buf, n, &pos, '-');
                iv = -iv;
                dv = -dv;
            }
            fmt_uint(buf, n, &pos, (unsigned long)iv, 10, 0);
            fmt_out(buf, n, &pos, '.');
            frac = (unsigned)((dv - (double)iv) * 100.0 + 0.5);
            if (frac < 10u) {
                fmt_out(buf, n, &pos, '0');
            }
            fmt_uint(buf, n, &pos, frac, 10, 0);
            break;
        }
            default:
                fmt_out(buf, n, &pos, *fmt);
                break;
            }
        }
    }
    if (buf != NULL && n > 0) {
        size_t term = pos < n ? pos : n - 1;
        buf[term] = '\0';
    }
    return (int)pos;
}

static int file_write_str(FILE *f, const char *s)
{
    if (f == NULL || s == NULL) {
        return 0;
    }
    if (f->h == -2 || f->h == -3) {
        ag_print(s);
        return (int)strlen(s);
    }
    if (f->h < 0) {
        return 0;
    }
    {
        size_t n = strlen(s);
        int32_t w = ag_write(f->h, s, n);
        if (w < 0) {
            f->err = 1;
            return 0;
        }
        return (int)w;
    }
}

int printf(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    argon_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ag_print(buf);
    return (int)strlen(buf);
}

int puts(const char *s)
{
    if (s != NULL) {
        ag_print(s);
    }
    ag_print("\n");
    return 1;
}

int putchar(int c)
{
    char s[2];
    s[0] = (char)c;
    s[1] = '\0';
    ag_print(s);
    return c;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int     n;
    va_start(ap, fmt);
    n = argon_vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int     r;
    va_start(ap, fmt);
    r = argon_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return argon_vsnprintf(buf, n, fmt, ap);
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[512];
    argon_vsnprintf(buf, sizeof buf, fmt, ap);
    return file_write_str(f, buf);
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int     n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int fflush(FILE *f)
{
    (void)f;
    return 0;
}

/*
 * How much is read ahead.  Four kilobytes, and measured rather than assumed.
 *
 * Sixteen was tried on the guess that a read costs mostly the call.  It is the
 * other way round: the calls fell by two and a half times and the time in the
 * filesystem doubled, taking Fallout 2 from 5.7 frames a second to 3.3.  The
 * cost is in the bytes, so reading ahead further only fetches what nobody asks
 * for.  Four is enough to take the per-call overhead off the byte-at-a-time
 * callers - fgets was one system call per character - without hauling a
 * kilobyte nobody wanted.
 *
 * Every open file pays for it, which is why it is allocated on the first read
 * small enough to want one and never for a caller streaming something bigger.
 */
#define FILE_RBUF 4096

/*
 * What the filesystem costs, kept because "the engine is slow" is not a
 * finding.  A frame that spends its time in ag_read is a frame waiting on a
 * card and a FAT driver, which is ours to make faster; a frame that spends it
 * anywhere else is the program thinking, which is not.  Two clock reads per
 * call, and buffering has already made the calls rare.
 */
static uint64_t s_io_us;
static uint32_t s_io_reads;
static uint32_t s_io_opens;
static uint32_t s_io_seeks;

/*
 * And split by which of the three it was.  One number for the filesystem says
 * a frame is waiting on the card; it does not say whether it is waiting to be
 * told where a file is, to be moved inside one, or for bytes - and those three
 * have nothing in common to fix.
 */
static uint64_t s_open_us;
static uint64_t s_seek_us;

/*
 * Opens that found nothing, counted separately.
 *
 * A program that looks for a file in three places before the one it is in
 * pays for two directory walks every time, and on FAT a directory walk is not
 * cheap.  Nothing about that shows up as slow code or as bytes read, so it is
 * invisible unless it is counted.
 */
static uint32_t s_io_misses;


uint32_t ag_libc_io_misses(void)
{
    return s_io_misses;
}

void ag_libc_io_stats(uint64_t *us, uint32_t *reads, uint32_t *opens,
                      uint32_t *seeks, uint64_t *open_us, uint64_t *seek_us)
{
    if (us != NULL) {
        *us = s_io_us;
    }
    if (open_us != NULL) {
        *open_us = s_open_us;
    }
    if (seek_us != NULL) {
        *seek_us = s_seek_us;
    }
    if (reads != NULL) {
        *reads = s_io_reads;
    }
    if (opens != NULL) {
        *opens = s_io_opens;
    }
    if (seeks != NULL) {
        *seeks = s_io_seeks;
    }
}

void ag_libc_io_reset(void)
{
    s_io_us = 0;
    s_io_reads = 0;
    s_io_opens = 0;
    s_io_seeks = 0;
    s_io_misses = 0;
    s_open_us = 0;
    s_seek_us = 0;
}

static int32_t timed_read(ag_handle_t h, void *buf, size_t len)
{
    const ag_time_t t0 = ag_micros();
    const int32_t   n = ag_read(h, buf, len);
    s_io_us += (uint64_t)(ag_micros() - t0);
    s_io_reads++;
    return n;
}

/*
 * Give back what has been read ahead and not consumed.
 *
 * The kernel's position is past the end of the buffer, so anything that moves
 * or uses the position directly - a seek, a write, a tell - has to put it back
 * where the caller thinks it is first.
 */
static void read_drop(FILE *f)
{
    const int unread = f->rlen - f->rpos;
    if (unread > 0) {
        (void)ag_seek((ag_handle_t)f->h, -(int64_t)unread, AG_SEEK_CUR);
    }
    f->rlen = 0;
    f->rpos = 0;
}

/* Fill the buffer from the current kernel position.  Returns bytes available,
 * 0 at end of file, -1 on error. */
static int read_fill(FILE *f)
{
    if (f->rbuf == NULL) {
        f->rbuf = (unsigned char *)malloc(FILE_RBUF);
        if (f->rbuf == NULL) {
            return -1; /* no buffer: the caller reads unbuffered instead */
        }
        f->rcap = FILE_RBUF;
    }
    const int32_t n = timed_read((ag_handle_t)f->h, f->rbuf, (size_t)f->rcap);
    if (n < 0) {
        f->err = 1;
        return -1;
    }
    f->rlen = (int)n;
    f->rpos = 0;
    return (int)n;
}

FILE *fopen(const char *path, const char *mode)
{
    uint32_t flags = AG_O_RDONLY;
    FILE    *f;
    if (path == NULL || mode == NULL) {
        return NULL;
    }
    if (strchr(mode, 'w') != NULL) {
        flags = AG_O_WRONLY | AG_O_CREATE | AG_O_TRUNC;
    } else if (strchr(mode, 'a') != NULL) {
        flags = AG_O_WRONLY | AG_O_CREATE | AG_O_APPEND;
    } else if (strchr(mode, '+') != NULL) {
        flags = AG_O_RDWR;
    }
    {
        const ag_time_t open_t0 = ag_micros();
        ag_handle_t h = ag_open(path, flags);
        const uint64_t open_dt = (uint64_t)(ag_micros() - open_t0);
        s_io_us += open_dt;
        s_open_us += open_dt;
        s_io_opens++;
        if (h < 0) {
            s_io_misses++;
            errno = ENOENT;
            return NULL;
        }
        f = (FILE *)malloc(sizeof(FILE));
        if (f == NULL) {
            ag_close(h);
            errno = ENOMEM;
            return NULL;
        }
        f->h = (int)h;
        f->eof = 0;
        f->err = 0;
        f->rbuf = NULL;
        f->rcap = 0;
        f->rlen = 0;
        f->rpos = 0;
        return f;
    }
}

int fclose(FILE *f)
{
    if (f == NULL || f->h < 0) {
        return 0;
    }
    if (f->rbuf != NULL) {
        free(f->rbuf);
        f->rbuf = NULL;
    }
    ag_close((ag_handle_t)f->h);
    free(f);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t   want, got = 0;
    uint8_t *out = (uint8_t *)ptr;

    if (f == NULL || ptr == NULL || size == 0) {
        return 0;
    }
    want = size * nmemb;

    /* The console is not a file and has nothing to read ahead from. */
    if (f->h < 0) {
        while (got < want) {
            const int32_t n = timed_read((ag_handle_t)f->h, out + got, want - got);
            if (n < 0) {
                f->err = 1;
                break;
            }
            if (n == 0) {
                f->eof = 1;
                break;
            }
            got += (size_t)n;
        }
        return got / size;
    }

    /* Whatever is already in hand, first. */
    {
        const size_t have = (size_t)(f->rlen - f->rpos);
        const size_t take = (have < want) ? have : want;
        if (take > 0) {
            memcpy(out, f->rbuf + f->rpos, take);
            f->rpos += (int)take;
            got += take;
        }
    }

    while (got < want) {
        const size_t left = want - got;
        /* A request the buffer could not hold goes straight to the kernel: no
         * point copying a megabyte twice to save a call. */
        if (left >= (size_t)FILE_RBUF) {
            const int32_t n = timed_read((ag_handle_t)f->h, out + got, left);
            if (n < 0) {
                f->err = 1;
                break;
            }
            if (n == 0) {
                f->eof = 1;
                break;
            }
            got += (size_t)n;
            continue;
        }
        {
            const int n = read_fill(f);
            if (n < 0) {
                /* Out of memory for a buffer, or a read error.  Fall back to
                 * reading straight through rather than failing. */
                const int32_t d = timed_read((ag_handle_t)f->h, out + got, left);
                if (d <= 0) {
                    if (d == 0) {
                        f->eof = 1;
                    }
                    break;
                }
                got += (size_t)d;
                continue;
            }
            if (n == 0) {
                f->eof = 1;
                break;
            }
            {
                const size_t take = ((size_t)n < left) ? (size_t)n : left;
                memcpy(out + got, f->rbuf, take);
                f->rpos = (int)take;
                got += take;
            }
        }
    }
    return size ? got / size : 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t  want, got = 0;
    int32_t n;
    if (f == NULL || ptr == NULL || size == 0) {
        return 0;
    }
    if (f->h >= 0 && f->rlen > f->rpos) {
        /* The kernel is ahead of where the caller thinks it is. */
        read_drop(f);
    }
    want = size * nmemb;
    while (got < want) {
        n = ag_write((ag_handle_t)f->h, (const uint8_t *)ptr + got, want - got);
        if (n < 0) {
            f->err = 1;
            break;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    return size ? got / size : 0;
}

int fseek(FILE *f, long off, int whence)
{
    int ag_wh = AG_SEEK_SET;
    if (f == NULL || f->h < 0) {
        return -1;
    }
    if (whence == SEEK_CUR) {
        ag_wh = AG_SEEK_CUR;
    } else if (whence == SEEK_END) {
        ag_wh = AG_SEEK_END;
    }
    /* SEEK_CUR is relative to where the *caller* is, not to the kernel. */
    read_drop(f);
    {
        const ag_time_t t0 = ag_micros();
        const int64_t   r = ag_seek((ag_handle_t)f->h, off, ag_wh);
        const uint64_t seek_dt = (uint64_t)(ag_micros() - t0);
        s_io_us += seek_dt;
        s_seek_us += seek_dt;
        s_io_seeks++;
        if (r < 0) {
            return -1;
        }
    }
    f->eof = 0;
    return 0;
}

long ftell(FILE *f)
{
    int64_t p;
    if (f == NULL || f->h < 0) {
        return -1;
    }
    p = ag_seek((ag_handle_t)f->h, 0, AG_SEEK_CUR);
    if (p < 0) {
        return -1;
    }
    /* Less whatever was read ahead and not yet handed out. */
    return (long)(p - (int64_t)(f->rlen - f->rpos));
}

/*
 * These three are here for a reason that is not obvious: without them the
 * linker takes newlib's, and newlib's rewind lives in an object that also
 * defines fseek and reaches for _fflush_r, which drags in newlib's fflush and
 * fclose - and those collide with the ones above.  One missing function pulls
 * a whole stdio in behind it.  Providing them keeps the archive closed.
 */
void rewind(FILE *f)
{
    if (f != NULL && f->h >= 0) {
        f->rlen = 0;
        f->rpos = 0;
        ag_seek((ag_handle_t)f->h, 0, AG_SEEK_SET);
        f->eof = 0;
    }
}

int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    return fwrite(&ch, 1, 1, f) == 1 ? (int)ch : -1;
}

int fputs(const char *s, FILE *f)
{
    size_t n;
    if (s == NULL) {
        return -1;
    }
    n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : -1;
}

/*
 * Enough of strtod for a configuration file: sign, digits, a point, more
 * digits, and an exponent.  No hexadecimal float, no infinity, no NaN - none
 * of which appear in a .cfg, and each of which is a corner where a
 * hand-written parser goes quietly wrong.
 *
 * Accumulating in a double and scaling once at the end costs one rounding
 * rather than one per digit.
 */
double strtod(const char *s, char **end)
{
    double mantissa = 0.0;
    int    exponent = 0;
    int    sign = 1;
    int    digits = 0;
    const char *start = s;

    if (s == NULL) {
        if (end) {
            *end = NULL;
        }
        return 0.0;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        mantissa = mantissa * 10.0 + (double)(*s - '0');
        s++;
        digits++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            mantissa = mantissa * 10.0 + (double)(*s - '0');
            exponent--;
            s++;
            digits++;
        }
    }
    if (digits == 0) {
        /* Nothing was consumed, so nothing was a number. */
        if (end) {
            *end = (char *)start;
        }
        return 0.0;
    }
    if (*s == 'e' || *s == 'E') {
        const char *mark = s;
        int         esign = 1;
        int         evalue = 0;
        int         edigits = 0;
        s++;
        if (*s == '-') {
            esign = -1;
            s++;
        } else if (*s == '+') {
            s++;
        }
        while (*s >= '0' && *s <= '9') {
            evalue = evalue * 10 + (*s - '0');
            s++;
            edigits++;
        }
        if (edigits == 0) {
            s = mark; /* an 'e' with no exponent is not part of the number */
        } else {
            exponent += esign * evalue;
        }
    }

    while (exponent > 0) {
        mantissa *= 10.0;
        exponent--;
    }
    while (exponent < 0) {
        mantissa /= 10.0;
        exponent++;
    }

    if (end) {
        *end = (char *)s;
    }
    return sign < 0 ? -mantissa : mantissa;
}

/*
 * A word at a time when the alignment allows it.
 *
 * These were both a byte at a time, which on a 32-bit core is four
 * instructions per byte and is fine until something calls them in an audio
 * loop.  Measured: the IR convolution zeroes four buffers of two kilobytes per
 * block of 256 samples, and that alone was a hundred and thirty instructions
 * per audio sample - a fifth of what the whole cabinet cost - spent storing
 * zeros one byte at a time.
 *
 * The head and tail loops are what keeps it correct for any pointer: only the
 * middle runs as words, and only when both ends agree on their offset.
 */
void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
        while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
            *d++ = *s++;
            n--;
        }
        while (n >= 4u) {
            *(uint32_t *)d = *(const uint32_t *)s;
            d += 4;
            s += 4;
            n -= 4u;
        }
    }
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        return memcpy(dst, src, n);
    }
    d += n;
    s += n;
    while (n--) {
        *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char      *d = (unsigned char *)dst;
    const unsigned char b = (unsigned char)c;

    while (n > 0u && (((uintptr_t)d & 3u) != 0u)) {
        *d++ = b;
        n--;
    }
    if (n >= 4u) {
        const uint32_t w = 0x01010101u * (uint32_t)b;
        while (n >= 4u) {
            *(uint32_t *)d = w;
            d += 4;
            n -= 4u;
        }
    }
    while (n--) {
        *d++ = b;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y) {
            return *x - *y;
        }
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

char *strcpy(char *d, const char *s)
{
    char *o = d;
    while ((*d++ = *s++) != '\0') {
    }
    return o;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && s[i]; i++) {
        d[i] = s[i];
    }
    for (; i < n; i++) {
        d[i] = '\0';
    }
    return d;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n > 0 && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcat(char *d, const char *s)
{
    char *o = d;
    while (*d) {
        d++;
    }
    while ((*d++ = *s++) != '\0') {
    }
    return o;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) {
        if ((unsigned char)*s == (unsigned char)c) {
            return (char *)s;
        }
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++) {
        if ((unsigned char)*s == (unsigned char)c) {
            last = s;
        }
    }
    return (char *)last;
}

int feof(FILE *f) { return f != NULL && f->eof; }
int ferror(FILE *f) { return f != NULL && f->err; }

int fgetc(FILE *f)
{
    unsigned char c;
    if (fread(&c, 1, 1, f) != 1) {
        return EOF;
    }
    return (int)c;
}

/*
 * By seeking back a byte rather than keeping a pushback slot, which means the
 * character that comes back is the one that was there - not the one handed in.
 * Every caller in sight pushes back what it just read, which is what ungetc is
 * for; a caller that pushes back something else would find this lying, and the
 * fix then is a byte in FILE rather than a smarter seek.
 *
 * It is also here to keep newlib out: a single call to newlib's ungetc drags
 * in its fflush, which drags in its stdio table, which then collides with
 * three functions this file already defines.
 */
int ungetc(int c, FILE *f)
{
    if (f == NULL || f->h < 0 || c == EOF) {
        return EOF;
    }
    /* Usually the byte is still in the buffer, and stepping back over it is
     * both exact and free. */
    if (f->rpos > 0) {
        f->rpos--;
        f->eof = 0;
        return c;
    }
    read_drop(f);
    if (ag_seek((ag_handle_t)f->h, -1, AG_SEEK_CUR) < 0) {
        return EOF;
    }
    f->eof = 0;
    return c;
}

char *fgets(char *s, int n, FILE *f)
{
    int i = 0, c;
    if (s == NULL || n <= 0 || f == NULL) {
        return NULL;
    }
    while (i < n - 1) {
        c = fgetc(f);
        if (c == EOF) {
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    if (i == 0) {
        return NULL;
    }
    s[i] = '\0';
    return s;
}

int remove(const char *path)
{
    return ag_unlink(path) == AG_OK ? 0 : -1;
}

int rename(const char *from, const char *to)
{
    return ag_rename(from, to) == AG_OK ? 0 : -1;
}

int mkdir(const char *path, mode_t mode)
{
    (void)mode;
    return ag_mkdir(path) == AG_OK ? 0 : -1;
}

int access(const char *path, int mode)
{
    ag_handle_t h;
    (void)mode;
    h = ag_open(path, AG_O_RDONLY);
    if (h < 0) {
        return -1;
    }
    ag_close(h);
    return 0;
}

char *strstr(const char *h, const char *n)
{
    size_t ln;
    if (h == NULL || n == NULL) {
        return NULL;
    }
    ln = strlen(n);
    if (ln == 0) {
        return (char *)h;
    }
    for (; *h; h++) {
        if (strncmp(h, n, ln) == 0) {
            return (char *)h;
        }
    }
    return NULL;
}

char *strerror(int err)
{
    (void)err;
    return "error";
}

char *strdup(const char *s)
{
    size_t n;
    char  *d;
    if (s == NULL) {
        return NULL;
    }
    n = strlen(s) + 1;
    d = (char *)malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

int system(const char *cmd)
{
    (void)cmd;
    return -1;
}

long strtol(const char *s, char **end, int base)
{
    long v = 0;
    int  sign = 1;
    if (s == NULL) {
        if (end) {
            *end = NULL;
        }
        return 0;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    }
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (;;) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * base + d;
        s++;
    }
    if (end) {
        *end = (char *)s;
    }
    return v * sign;
}

/*
 * The unsigned twin, needed because std::stoul and its neighbours live in the
 * same libstdc++ member as the rest of std::string: without one here, newlib's
 * comes in, and newlib's reaches for a per-thread reentrancy structure that
 * this side of the world does not have.
 *
 * Like strtol above it does not report overflow through errno.  A caller that
 * needs to tell 4294967296 from 0 has to check the digits itself.
 */
unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long v = 0;
    int negate = 0;

    if (s == NULL) {
        if (end) {
            *end = NULL;
        }
        return 0;
    }
    while (isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '-') {
        /* Standard, and surprising: a negated unsigned conversion is legal and
         * wraps.  Ports parse -1 as a mask often enough to matter. */
        negate = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    }
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    for (;;) {
        int  d;
        char c = *s;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'z') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'Z') {
            d = c - 'A' + 10;
        } else {
            break;
        }
        if (d >= base) {
            break;
        }
        v = v * (unsigned long)base + (unsigned long)d;
        s++;
    }
    if (end) {
        *end = (char *)s;
    }
    return negate ? (unsigned long)0 - v : v;
}

void *memchr(const void *p, int c, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    const unsigned char  want = (unsigned char)c;
    for (size_t i = 0; i < n; ++i) {
        if (b[i] == want) {
            return (void *)(b + i);
        }
    }
    return NULL;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    int     assigned = 0;
    if (str == NULL || fmt == NULL) {
        return 0;
    }
    va_start(ap, fmt);
    while (*fmt && *str) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*fmt)) {
                fmt++;
            }
            while (isspace((unsigned char)*str)) {
                str++;
            }
            continue;
        }
        if (*fmt != '%') {
            if (*fmt != *str) {
                break;
            }
            fmt++;
            str++;
            continue;
        }
        fmt++;
        while (*fmt >= '0' && *fmt <= '9') {
            fmt++;
        }
        if (*fmt == 'd' || *fmt == 'i' || *fmt == 'x' || *fmt == 'o' ||
            *fmt == 'u') {
            int *out = va_arg(ap, int *);
            int  base = (*fmt == 'x') ? 16 : (*fmt == 'o') ? 8 : 10;
            char *end = NULL;
            long  v;
            while (isspace((unsigned char)*str)) {
                str++;
            }
            v = strtol(str, &end, base);
            if (end == str) {
                break;
            }
            if (out) {
                *out = (int)v;
            }
            str = end;
            assigned++;
            fmt++;
            continue;
        }
        break;
    }
    va_end(ap);
    return assigned;
}

int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int isalpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isprint(int c) { return c >= 32 && c < 127; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

int strcasecmp(const char *a, const char *b)
{
    while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) {
        a++;
        b++;
    }
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n > 0 && *a && *b &&
           toupper((unsigned char)*a) == toupper((unsigned char)*b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0) {
        return 0;
    }
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

double sin(double x)
{
    (void)x;
    return 0.0;
}
double cos(double x)
{
    (void)x;
    return 0.0;
}
/* No soft-double compares (would pull libgcc __ltdf2 into the .AXE). */
double fabs(double x)
{
    (void)x;
    return 0;
}
float fabsf(float x)
{
    union {
        float    f;
        uint32_t u;
    } v;
    v.f = x;
    v.u &= ~0x80000000u;
    return v.f;
}
float  sinf(float x)
{
    (void)x;
    return 0.0f;
}
float cosf(float x)
{
    (void)x;
    return 0.0f;
}
float powf(float x, float y)
{
    (void)y;
    return x;
}
float logf(float x)
{
    (void)x;
    return 0.0f;
}
float floorf(float x)
{
    return (float)(int)x;
}
