/*
 * The C library the agent needs, on Win32 alone (see compat.h).
 *
 * Copyright (c) 2026 ArgonOS contributors.  SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#include "compat.h"

struct agent_file {
    HANDLE h;
    int    std; /* one of the console handles: never closed */
};

static struct agent_file s_in = {NULL, STD_INPUT_HANDLE};
static struct agent_file s_out = {NULL, STD_OUTPUT_HANDLE};
static struct agent_file s_err = {NULL, STD_ERROR_HANDLE};
FILE *stdin = &s_in;
FILE *stdout = &s_out;
FILE *stderr = &s_err;

static HANDLE fh(FILE *f)
{
    if (f->std != 0) {
        return GetStdHandle((DWORD)f->std);
    }
    return f->h;
}

/* ---- memory: the process heap ----------------------------------------- */

void *malloc(size_t n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
void *calloc(size_t a, size_t b)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (a * b) ? a * b : 1);
}
void *realloc(void *p, size_t n)
{
    if (p == NULL) {
        return malloc(n);
    }
    return HeapReAlloc(GetProcessHeap(), 0, p, n ? n : 1);
}
void free(void *p)
{
    if (p != NULL) {
        HeapFree(GetProcessHeap(), 0, p);
    }
}

/* ---- bytes and strings ------------------------------------------------- */

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char       *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) {
        *dp++ = *sp++;
    }
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char       *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    if (dp < sp) {
        while (n--) {
            *dp++ = *sp++;
        }
    } else {
        while (n--) {
            dp[n] = sp[n];
        }
    }
    return d;
}
void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    while (n--) {
        *dp++ = (unsigned char)c;
    }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x - *y;
        }
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
    for (; n; n--, a++, b++) {
        if (*a != *b || *a == 0) {
            return (unsigned char)*a - (unsigned char)*b;
        }
    }
    return 0;
}
char *strcat(char *d, const char *s)
{
    char *p = d + strlen(d);
    while ((*p++ = *s++) != 0) {
    }
    return d;
}
char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    do {
        if (*s == (char)c) {
            last = s;
        }
    } while (*s++);
    return (char *)last;
}
int atoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ') {
        s++;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return neg ? -v : v;
}
int agent_isalnum(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

/* ---- formatting: the subset the agent uses ----------------------------- */

typedef struct {
    char  *buf;
    size_t cap, len;
} out_t;

static void put(out_t *o, char c)
{
    if (o->len + 1 < o->cap) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void put_num(out_t *o, unsigned long long v, int base, int upper,
                    int neg, int width, char pad)
{
    char tmp[24];
    int  n = 0;
    do {
        const int d = (int)(v % (unsigned)base);
        tmp[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= (unsigned)base;
    } while (v != 0 && n < (int)sizeof(tmp));
    int total = n + (neg ? 1 : 0);
    if (neg && pad == '0') {
        put(o, '-');
    }
    for (; total < width; total++) {
        put(o, pad);
    }
    if (neg && pad != '0') {
        put(o, '-');
    }
    while (n) {
        put(o, tmp[--n]);
    }
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    out_t o = {buf, cap, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        int  left = 0, width = 0, lng = 0;
        char pad = ' ';
        if (*fmt == '-') {
            left = 1;
            fmt++;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l' || *fmt == 'z') {
            lng += (*fmt == 'l') ? 1 : 1;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            long long v = (lng >= 2) ? va_arg(ap, long long)
                          : (lng == 1) ? va_arg(ap, long)
                                       : va_arg(ap, int);
            put_num(&o, (unsigned long long)(v < 0 ? -v : v), 10, 0, v < 0,
                    width, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long)
                                   : (lng == 1) ? va_arg(ap, unsigned long)
                                                : va_arg(ap, unsigned int);
            put_num(&o, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', 0, width, pad);
            break;
        }
        case 'p':
            put_num(&o, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16,
                    0, 0, width, pad);
            break;
        case 'c':
            put(&o, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            int n = (int)strlen(s);
            if (!left) {
                for (; n < width; n++) {
                    put(&o, ' ');
                }
            }
            while (*s) {
                put(&o, *s++);
            }
            if (left) {
                for (; n < width; n++) {
                    put(&o, ' ');
                }
            }
            break;
        }
        case '%':
            put(&o, '%');
            break;
        default:
            put(&o, '%');
            put(&o, *fmt);
            break;
        }
    }
    if (cap > 0) {
        buf[o.len < cap ? o.len : cap - 1] = '\0';
    }
    return (int)o.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

static int vfout(FILE *f, const char *fmt, va_list ap)
{
    char      line[1024];
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    DWORD     w = 0;
    const DWORD len = (DWORD)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    WriteFile(fh(f), line, len, &w, NULL);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vfout(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vfout(f, fmt, ap);
    va_end(ap);
    return n;
}

int fflush(FILE *f)
{
    (void)f; /* WriteFile is unbuffered */
    return 0;
}

/* ---- input and files --------------------------------------------------- */

char *fgets(char *buf, int cap, FILE *f)
{
    HANDLE h = fh(f);
    int    n = 0;
    while (n + 1 < cap) {
        char  c;
        DWORD got = 0;
        if (!ReadFile(h, &c, 1, &got, NULL) || got == 0) {
            break;
        }
        buf[n++] = c;
        if (c == '\n') {
            break;
        }
    }
    if (n == 0) {
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

FILE *fopen(const char *path, const char *mode)
{
    (void)mode; /* read-only is all the agent asks for */
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    FILE *f = (FILE *)calloc(1, sizeof(*f));
    if (f == NULL) {
        CloseHandle(h);
        return NULL;
    }
    f->h = h;
    return f;
}

int fclose(FILE *f)
{
    if (f != NULL && f->std == 0) {
        CloseHandle(f->h);
        free(f);
    }
    return 0;
}

/* ---- the entry point the C runtime would have supplied ----------------- */

int main(int argc, char **argv);

/* GetCommandLineA split the way the CRT does for the cases that matter here:
 * blanks separate, double quotes group (a path with spaces). */
void __stdcall agent_start(void)
{
    char       *cl = GetCommandLineA();
    const size_t n = strlen(cl);
    char       *buf = (char *)malloc(n + 1);
    char      **argv = (char **)malloc(sizeof(char *) * (n / 2 + 2));
    int         argc = 0;
    char       *o = buf;
    const char *p = cl;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = o;
        int q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                q = !q;
                p++;
                continue;
            }
            *o++ = *p++;
        }
        *o++ = '\0';
    }
    argv[argc] = NULL;
    ExitProcess((UINT)main(argc, argv));
}

/* MinGW compilers put a call to this at the top of main() to run static
 * constructors; there are none in C. */
void __main(void) {}
