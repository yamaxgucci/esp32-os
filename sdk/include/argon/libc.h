/*
 * ArgonOS - the handful of C library functions an application cannot do without.
 *
 * An application links with -nostdlib: there is no libc, and that is on purpose
 * [Т-9] - a printf implementation is 20 KB of an arena that holds 64.  But two
 * kinds of function are still needed.
 *
 * The first kind the compiler emits by itself.  A struct assignment becomes a
 * memcpy call, a zeroed array becomes memset, and no amount of not calling them
 * avoids it - so they are defined here, weakly, which lets an application of
 * several files include this header in each without a duplicate symbol.
 *
 * The second kind is the string handling every program writes anyway, spelled
 * ag_* and bounded by construction: a copy that takes the size of its
 * destination cannot overrun it, which is the only version worth having in a
 * system with no memory protection.
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_LIBC_H
#define ARGON_LIBC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- what the compiler emits ------------------------------------------- */

/*
 * Left out where a real C library is present - the kernel builds the file
 * manager into itself, and there these would shadow newlib's own, which are
 * better than these and live in the right kind of memory.  An application
 * defines nothing and gets them.
 */
#ifndef AG_HAVE_LIBC

__attribute__((weak)) void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    /* Word at a time while both ends are aligned; the tail byte-wise. */
    if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
        uint32_t       *dw = (uint32_t *)dst;
        const uint32_t *sw = (const uint32_t *)src;
        while (n >= 4) {
            *dw++ = *sw++;
            n -= 4;
        }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n-- > 0) {
        *d++ = *s++;
    }
    return dst;
}

__attribute__((weak)) void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        return memcpy(dst, src, n);
    }
    /* Overlapping the other way: backwards, or the tail eats the head. */
    d += n;
    s += n;
    while (n-- > 0) {
        *--d = *--s;
    }
    return dst;
}

__attribute__((weak)) void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;

    while (n-- > 0) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

__attribute__((weak)) int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    while (n-- > 0) {
        if (*x != *y) {
            return (int)*x - (int)*y;
        }
        x++;
        y++;
    }
    return 0;
}

__attribute__((weak)) size_t strlen(const char *s)
{
    const char *p = s;
    while (*p != '\0') {
        p++;
    }
    return (size_t)(p - s);
}

#else /* AG_HAVE_LIBC */
#include <string.h>
#endif

/* ---- bounded string handling ------------------------------------------- */

/* Copies as much as fits and always terminates.  Returns what was copied. */
static inline size_t ag_strlcpy(char *dst, const char *src, size_t size)
{
    if (dst == NULL || size == 0) {
        return 0;
    }
    size_t n = 0;
    if (src != NULL) {
        while (src[n] != '\0' && n + 1 < size) {
            dst[n] = src[n];
            n++;
        }
    }
    dst[n] = '\0';
    return n;
}

/* Appends as much as fits and always terminates. */
static inline size_t ag_strlcat(char *dst, const char *src, size_t size)
{
    if (dst == NULL || size == 0) {
        return 0;
    }
    const size_t at = strlen(dst);
    if (at + 1 >= size) {
        return at;
    }
    return at + ag_strlcpy(&dst[at], src, size - at);
}

static inline int ag_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline char ag_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static inline char ag_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Case-insensitive, which is what comparing file names on FAT means. */
static inline int ag_stricmp(const char *a, const char *b)
{
    while (*a != '\0' && ag_lower(*a) == ag_lower(*b)) {
        a++;
        b++;
    }
    return (int)(unsigned char)ag_lower(*a) - (int)(unsigned char)ag_lower(*b);
}

static inline bool ag_ends_with_i(const char *s, const char *suffix)
{
    const size_t n = strlen(s);
    const size_t m = strlen(suffix);

    if (m > n) {
        return false;
    }
    return ag_stricmp(&s[n - m], suffix) == 0;
}

/*
 * An unsigned number, right-aligned in `width`, without printf.  Returns the
 * start of the text inside the buffer, which is the point: formatting a column
 * of sizes is the one thing a file manager does on every redraw.
 */
static inline const char *ag_utoa(uint64_t value, char *buf, size_t size,
                                 size_t width, bool group)
{
    if (buf == NULL || size < 2) {
        return "";
    }

    size_t at = size;
    buf[--at] = '\0';

    size_t digits = 0;
    do {
        if (at == 0) {
            break;
        }
        if (group && digits > 0 && (digits % 3) == 0) {
            buf[--at] = ' ';
            if (at == 0) {
                break;
            }
        }
        buf[--at] = (char)('0' + (int)(value % 10u));
        value /= 10u;
        digits++;
    } while (value != 0);

    while (at > 0 && (size - 1 - at) < width) {
        buf[--at] = ' ';
    }
    return &buf[at];
}

#ifdef __cplusplus
}
#endif

#endif /* ARGON_LIBC_H */
