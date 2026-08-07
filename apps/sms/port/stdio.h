#ifndef ARGON_SMS_STDIO_H
#define ARGON_SMS_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct {
    int unused;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define EOF (-1)

int  printf(const char *fmt, ...);
int  puts(const char *s);
int  sprintf(char *buf, const char *fmt, ...);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
int  fprintf(FILE *f, const char *fmt, ...);
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int   fseek(FILE *f, long off, int whence);
long  ftell(FILE *f);

#endif
