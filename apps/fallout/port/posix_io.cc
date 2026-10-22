/*
 * ArgonOS - the POSIX file calls newlib expects an operating system to supply.
 *
 * The engine opens its .DAT files with open() and read(), not fopen(): a
 * 700 MB archive is seeked around, not streamed.  Those go through newlib,
 * newlib reduces them to _open, _read, _lseek and their neighbours, and on a
 * bare-metal target nothing defines those.  Here they are, over the ArgonOS
 * filesystem.
 *
 * The descriptor is the ArgonOS handle plus three, so that 0, 1 and 2 stay the
 * console the way every program assumes.  There is no descriptor table: a
 * handle is already a small integer the kernel owns, and duplicating the
 * bookkeeping would only add a way for the two to disagree.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#include <argon/argon.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

/* Where the standard three end and real files begin. */
#define FD_BIAS 3

extern "C" {

static ag_handle_t handle_of(int fd)
{
    return (ag_handle_t)(fd - FD_BIAS);
}

/*
 * The working directory.  Fallout 2 changes into each of its data directories
 * to search them and changes back - xbaseOpen does it several times per file -
 * so these are not decoration.
 *
 * ArgonOS has a current directory of its own, per process, which is what the
 * shell prints in its prompt.  There is nothing to emulate.
 */
char *getcwd(char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (ag_getcwd(buf, size) != AG_OK) {
        errno = ERANGE;
        return NULL;
    }
    return buf;
}

int chdir(const char *path)
{
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ag_chdir(path) != AG_OK) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

/*
 * Four stubs newlib insists on having somewhere before it will link.
 *
 * They arrive with clock(), raise() and the random number seeding, none of
 * which this program can do anything useful with: there is one process, it
 * cannot signal itself, and the entropy it would be given is not entropy.  A
 * failure return is the honest answer and the callers all check.
 */
int _getpid(void) { return 1; }

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

clock_t _times(struct tms *buf)
{
    (void)buf;
    errno = ENOSYS;
    return (clock_t)-1;
}

int _getentropy(void *buf, size_t len)
{
    (void)buf;
    (void)len;
    errno = ENOSYS;
    return -1;
}

int _open(const char *path, int flags, ...)
{
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    uint32_t f = 0;
    switch (flags & O_ACCMODE) {
    case O_WRONLY:
        f |= AG_O_WRONLY;
        break;
    case O_RDWR:
        f |= AG_O_RDWR;
        break;
    default:
        f |= AG_O_RDONLY;
        break;
    }
    if (flags & O_CREAT) {
        f |= AG_O_CREATE;
    }
    if (flags & O_TRUNC) {
        f |= AG_O_TRUNC;
    }
    if (flags & O_APPEND) {
        f |= AG_O_APPEND;
    }
    if (flags & O_EXCL) {
        f |= AG_O_EXCL;
    }

    ag_handle_t h = ag_open(path, f);
    if (h < 0) {
        errno = ENOENT;
        return -1;
    }
    return (int)h + FD_BIAS;
}

int _close(int fd)
{
    if (fd < FD_BIAS) {
        return 0;
    }
    return ag_close(handle_of(fd)) == AG_OK ? 0 : -1;
}

extern "C" uint32_t ag_us(void);
/*
 * ARGON: every read from the card passes here, and the worst one is the
 * question.
 *
 * The game still reads while it is being played - music streams in blocks,
 * and a block arrives about once a second, which is the same rhythm as the
 * stall Maxim describes.  A card that is busy with its own housekeeping can
 * take a hundred milliseconds to answer a read that usually costs two, and
 * the thread that asked simply stops until it does.  The average says
 * nothing about that; the worst single read says everything.
 */
unsigned ag_io_read_n, ag_io_read_us, ag_io_read_kb;
unsigned ag_io_read_worst_us, ag_io_read_worst_len;

int _read(int fd, void *buf, size_t len)
{
    if (fd < FD_BIAS) {
        errno = EBADF;
        return -1;
    }

    const uint32_t t0 = ag_us();
    int32_t n = ag_read(handle_of(fd), buf, len);
    const uint32_t took = ag_us() - t0;

    ag_io_read_n++;
    ag_io_read_us += took;
    if (n > 0) {
        ag_io_read_kb += (unsigned)n >> 10;
    }
    if (took > ag_io_read_worst_us) {
        ag_io_read_worst_us = took;
        ag_io_read_worst_len = (unsigned)len;
    }
    return n < 0 ? -1 : (int)n;
}

int _write(int fd, const void *buf, size_t len)
{
    /* Standard output and standard error are the console.  The engine writes
     * there through printf, which the shim in apps/common/libc already
     * intercepts, but debug.cc reaches for write() directly. */
    if (fd == 1 || fd == 2) {
        const char *p = (const char *)buf;
        for (size_t i = 0; i < len; ++i) {
            char one[2] = { p[i], '\0' };
            ag_print(one);
        }
        return (int)len;
    }
    if (fd < FD_BIAS) {
        errno = EBADF;
        return -1;
    }
    int32_t n = ag_write(handle_of(fd), buf, len);
    return n < 0 ? -1 : (int)n;
}

long _lseek(int fd, long off, int whence)
{
    if (fd < FD_BIAS) {
        errno = EBADF;
        return -1;
    }
    int64_t at = ag_seek(handle_of(fd), off, whence);
    return at < 0 ? -1 : (long)at;
}

/*
 * newlib asks for this to decide whether a descriptor is a terminal and how to
 * buffer it.  Only the mode matters, and only enough to say "a regular file" -
 * which is true of everything that gets here, the console having been handled
 * above.
 */
int _fstat(int fd, struct stat *st)
{
    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = (fd < FD_BIAS) ? S_IFCHR : S_IFREG;
    return 0;
}

int _isatty(int fd) { return fd < FD_BIAS ? 1 : 0; }

/*
 * Nothing allocates through newlib: the shim's malloc goes to the process
 * arena.  This exists because one archive member reaches for it anyway, and
 * refusing is the honest answer - a heap here would be a second allocator over
 * the same memory, quietly competing with the first.
 */
void *_sbrk(int increment)
{
    (void)increment;
    errno = ENOMEM;
    return (void *)-1;
}

/* ---- time ---------------------------------------------------------------- */

/*
 * Milliseconds since the machine started, presented as seconds since 1970 -
 * which is wrong, and knowably so.  The engine uses the clock for elapsed
 * time and for seeding the random number generator; neither needs the date.
 * A save file's timestamp will read as January 1970 until the OS grows a real
 * time of day, and that is the visible consequence.
 */
int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv == NULL) {
        errno = EINVAL;
        return -1;
    }
    uint32_t ms = ag_millis();
    tv->tv_sec = (time_t)(ms / 1000u);
    tv->tv_usec = (suseconds_t)((ms % 1000u) * 1000u);
    return 0;
}

time_t time(time_t *out)
{
    time_t now = (time_t)(ag_millis() / 1000u);
    if (out != NULL) {
        *out = now;
    }
    return now;
}

/*
 * Broken-down time, written out rather than left to newlib.
 *
 * newlib's localtime consults the time zone, the time zone is parsed out of
 * the TZ string with sscanf, and that one call pulls in newlib's whole stdio -
 * scanf, ungetc, fflush, the FILE table and fclose - four of which then
 * collide with apps/common/libc.  A dozen lines of calendar arithmetic here
 * keep the archive shut.
 *
 * There is one zone and it is UTC, because ArgonOS has no time of day to be in
 * a zone of.  localtime and gmtime are therefore the same function.
 *
 * The date is Howard Hinnant's civil-from-days: shift the epoch to March 1st
 * so that the leap day lands at the end of the year, and the month lengths
 * become a repeating pattern with no table.
 */
static struct tm s_tm;

struct tm *gmtime_r(const time_t *when, struct tm *out)
{
    if (when == NULL || out == NULL) {
        return NULL;
    }

    long long secs = (long long)*when;
    long long days = secs / 86400;
    long long rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }

    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)((rem % 3600) / 60);
    out->tm_sec = (int)(rem % 60);

    /* 1970-01-01 was a Thursday. */
    out->tm_wday = (int)((days + 4) % 7);
    if (out->tm_wday < 0) {
        out->tm_wday += 7;
    }

    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;
    unsigned long d = doy - (153 * mp + 2) / 5 + 1;
    unsigned long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) {
        ++y;
    }

    out->tm_year = (int)(y - 1900);
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_isdst = 0;

    /* Day of the year, counted from the first of January of this year. */
    static const int k_cum[12] = { 0, 31, 59, 90, 120, 151,
                                   181, 212, 243, 273, 304, 334 };
    int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 1 : 0;
    out->tm_yday = k_cum[out->tm_mon] + (int)d - 1 +
                   ((out->tm_mon > 1) ? leap : 0);
    return out;
}

struct tm *localtime_r(const time_t *when, struct tm *out)
{
    return gmtime_r(when, out);
}

struct tm *gmtime(const time_t *when) { return gmtime_r(when, &s_tm); }

struct tm *localtime(const time_t *when) { return gmtime_r(when, &s_tm); }

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    if (ts == NULL) {
        errno = EINVAL;
        return -1;
    }
    uint32_t ms = ag_millis();
    ts->tv_sec = (time_t)(ms / 1000u);
    ts->tv_nsec = (long)((ms % 1000u) * 1000000u);
    return 0;
}

}  // extern "C"
