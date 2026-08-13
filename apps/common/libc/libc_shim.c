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
    /* Prefer the ABI realloc: it knows the old block size (multi_heap). */
    return ag_realloc(p, n);
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

double atof(const char *s)
{
    return (double)atoi(s);
}

static int fmt_out(char *buf, size_t n, size_t *pos, char c)
{
    if (*pos + 1 < n && buf != NULL) {
        buf[*pos] = c;
    }
    (*pos)++;
    return 1;
}

static int fmt_str(char *buf, size_t n, size_t *pos, const char *s)
{
    int c = 0;
    if (s == NULL) {
        s = "(null)";
    }
    while (*s) {
        fmt_out(buf, n, pos, *s++);
        c++;
    }
    return c;
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
                fmt_str(buf, n, &pos, va_arg(ap, const char *));
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
            fmt_str(buf, n, &pos, "0x");
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
        ag_handle_t h = ag_open(path, flags);
        if (h < 0) {
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
        return f;
    }
}

int fclose(FILE *f)
{
    if (f == NULL || f->h < 0) {
        return 0;
    }
    ag_close((ag_handle_t)f->h);
    free(f);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t  want, got = 0;
    int32_t n;
    if (f == NULL || ptr == NULL || size == 0) {
        return 0;
    }
    want = size * nmemb;
    while (got < want) {
        n = ag_read((ag_handle_t)f->h, (uint8_t *)ptr + got, want - got);
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
    return size ? got / size : 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t  want, got = 0;
    int32_t n;
    if (f == NULL || ptr == NULL || size == 0) {
        return 0;
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
    if (ag_seek((ag_handle_t)f->h, off, ag_wh) < 0) {
        return -1;
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
    return (long)p;
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
