/*
 * Minimal libc stand-ins so SMS Plus GX can build as an ArgonOS .AXE (-nostdlib).
 * SPDX-License-Identifier: Apache-2.0 (shim only; SMS core remains GPLv2+)
 */
#include <argon/argon.h>

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "math.h"

FILE *stdin;
FILE *stdout;
FILE *stderr;

void *malloc(size_t n)
{
    if (n == 0) {
        n = 1;
    }
    return ag_malloc(n);
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
    if (p == NULL) {
        return malloc(n);
    }
    if (n == 0) {
        free(p);
        return NULL;
    }
    void *q = malloc(n);
    if (q != NULL) {
        memcpy(q, p, n);
        free(p);
    }
    return q;
}

void free(void *p)
{
    if (p != NULL) {
        ag_free(p);
    }
}

void exit(int code) { ag_exit(code); }
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

int printf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

int puts(const char *s)
{
    (void)s;
    return 0;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    (void)f;
    (void)fmt;
    return 0;
}

int sprintf(char *buf, const char *fmt, ...)
{
    if (buf != NULL) {
        buf[0] = '\0';
    }
    (void)fmt;
    return 0;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    if (buf != NULL && n > 0) {
        buf[0] = '\0';
    }
    (void)fmt;
    return 0;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    (void)ap;
    if (buf != NULL && n > 0) {
        buf[0] = '\0';
    }
    (void)fmt;
    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    return NULL;
}

int fclose(FILE *f)
{
    (void)f;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    (void)ptr;
    (void)size;
    (void)nmemb;
    (void)f;
    return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    (void)ptr;
    (void)size;
    (void)nmemb;
    (void)f;
    return 0;
}

int fseek(FILE *f, long off, int whence)
{
    (void)f;
    (void)off;
    (void)whence;
    return -1;
}

long ftell(FILE *f)
{
    (void)f;
    return -1;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
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
    unsigned char *d = (unsigned char *)dst;
    while (n--) {
        *d++ = (unsigned char)c;
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
