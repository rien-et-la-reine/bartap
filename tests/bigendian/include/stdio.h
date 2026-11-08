/* The few stdio calls the fixture dump makes, for the freestanding big-endian build (R19). Test
 * code only: see libc.c. */
#ifndef BARTAP_BE_STDIO_H
#define BARTAP_BE_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct {
    int fd;
    int eof;
} FILE;

extern FILE *const stdout;
extern FILE *const stderr;

FILE *fopen(const char *path, const char *mode);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
int feof(FILE *f);
int fclose(FILE *f);
int fputs(const char *s, FILE *f);
int fprintf(FILE *f, const char *fmt, ...);
int printf(const char *fmt, ...);
int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
int snprintf(char *out, size_t cap, const char *fmt, ...);
void perror(const char *s);

#endif
