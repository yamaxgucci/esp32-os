/*
 * Minimal libc stand-in header for -nostdlib .AXE builds.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ARGON_SMS_STDIO_H
#define ARGON_SMS_STDIO_H

#include <stddef.h>
#include <stdarg.h>

/*
 * A read buffer, because without one every fgetc() is a system call.
 *
 * fgets() reads a byte at a time, fread() was passing every request straight
 * to ag_read, and an engine that reads its message files line by line then
 * pays a FAT lookup and an SD transaction per character.  Fallout 2 spent
 * minutes loading a map that way.
 *
 * The buffer is allocated on the first read small enough to want one - a
 * caller asking for more than the buffer holds is handed straight to the
 * kernel, so streaming a megabyte does not allocate anything or copy twice.
 * The kernel's file position is always at the far end of the buffer; the
 * logical position is that, less what is still unread.  Everything that moves
 * the position has to say so, which is what read_drop() below is for.
 */
typedef struct FILE {
    int h;   /* >=0 ag_handle; -2 stdout; -3 stderr; -4 stdin */
    int eof;
    int err;
    unsigned char *rbuf;
    int rcap;    /* bytes allocated, 0 if none */
    int rlen;    /* valid bytes in rbuf */
    int rpos;    /* cursor within rbuf */
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
int  putchar(int c);
int  sprintf(char *buf, const char *fmt, ...);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
int  fprintf(FILE *f, const char *fmt, ...);
int  vfprintf(FILE *f, const char *fmt, va_list ap);
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int  fflush(FILE *f);
int  sscanf(const char *str, const char *fmt, ...);

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int   fseek(FILE *f, long off, int whence);
long  ftell(FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
int   fgetc(FILE *f);
int   ungetc(int c, FILE *f);
int   fputc(int c, FILE *f);
int   fputs(const char *s, FILE *f);
void  rewind(FILE *f);
char *fgets(char *s, int n, FILE *f);
int   remove(const char *path);
int   rename(const char *from, const char *to);

#endif
