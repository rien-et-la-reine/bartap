/* A minimal C runtime for running host test tools on big-endian Arm under qemu-armeb (R19, phase
 * 10). No big-endian libc is installed here, and the library needs only the mem* functions (R34),
 * so the rest is just what the fixture dump calls: files, formatted output and exit, straight over
 * Linux system calls, which qemu-user passes to the host. Test code only. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- System calls (Arm EABI: number in r7, result in r0) ---- */

enum { SYS_EXIT_GROUP = 248, SYS_READ = 3, SYS_WRITE = 4, SYS_OPEN = 5, SYS_CLOSE = 6 };

static long sys3(long n, long a, long b, long c)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
    return r0;
}

_Noreturn void exit(int status)
{
    for (;;) {
        sys3(SYS_EXIT_GROUP, status, 0, 0);
    }
}

_Noreturn void abort(void)
{
    exit(134);
}

/* ---- Entry: the kernel leaves argc, then argv, at the stack pointer ---- */

int main(int argc, char **argv);

_Noreturn void be_start(long *sp);

_Noreturn void be_start(long *sp)
{
    exit(main((int)sp[0], (char **)(sp + 1)));
}

__asm__(".global _start\n"
        "_start:\n"
        "    mov r0, sp\n"
        "    bl be_start\n");

/* ---- Memory and strings ---- */

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n-- != 0) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n-- != 0) {
            *d++ = *s++;
        }
    } else {
        while (n-- != 0) {
            d[n] = s[n];
        }
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n-- != 0) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    do {
        if (*s == (char)c) {
            last = s;
        }
    } while (*s++ != '\0');
    return (char *)last;
}

/* ---- Files ---- */

static FILE files[8] = {{0, 0}, {1, 0}, {2, 0}};
FILE *const stdout = &files[1];
FILE *const stderr = &files[2];

FILE *fopen(const char *path, const char *mode)
{
    (void)mode;
    long fd = sys3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) {
        return NULL;
    }
    for (size_t i = 3; i < sizeof files / sizeof files[0]; i++) {
        if (files[i].fd == 0) {
            files[i].fd = (int)fd;
            files[i].eof = 0;
            return &files[i];
        }
    }
    sys3(SYS_CLOSE, fd, 0, 0);
    return NULL;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f)
{
    size_t want = size * count;
    size_t got = 0;
    while (got < want) {
        long n = sys3(SYS_READ, f->fd, (long)((uint8_t *)buf + got), (long)(want - got));
        if (n <= 0) {
            f->eof = 1;
            break;
        }
        got += (size_t)n;
    }
    if (got == want) {
        /* at the end exactly: stdio reports it only on the next read, so look */
        uint8_t probe;
        if (sys3(SYS_READ, f->fd, (long)&probe, 1) <= 0) {
            f->eof = 1;
        }
    }
    return size == 0 ? 0 : got / size;
}

int feof(FILE *f)
{
    return f->eof;
}

int fclose(FILE *f)
{
    sys3(SYS_CLOSE, f->fd, 0, 0);
    f->fd = 0;
    return 0;
}

static void write_all(int fd, const char *s, size_t n)
{
    while (n != 0) {
        long w = sys3(SYS_WRITE, fd, (long)s, (long)n);
        if (w <= 0) {
            return;
        }
        s += w;
        n -= (size_t)w;
    }
}

int fputs(const char *s, FILE *f)
{
    write_all(f->fd, s, strlen(s));
    return 0;
}

void perror(const char *s)
{
    fputs(s, stderr);
    fputs(": cannot open\n", stderr);
}

/* ---- Formatting: %s %c %d %u %x %X, with 0, a width, l, ll, z and .* for %s ---- */

typedef struct {
    char *out;
    size_t cap;
    size_t n;
} sink_t;

static void emit(sink_t *k, char c)
{
    if (k->n + 1u < k->cap) {
        k->out[k->n] = c;
    }
    k->n++;
}

/* v / base, leaving the remainder in *rem, 16 bits at a time: 64-bit division would need libgcc,
 * which has no big-endian build here. */
static uint64_t divide(uint64_t v, unsigned base, unsigned *rem)
{
    uint64_t q = 0;
    uint32_t r = 0;
    for (int shift = 48; shift >= 0; shift -= 16) {
        uint32_t cur = (r << 16) | (uint32_t)((v >> shift) & 0xFFFFu);
        q |= (uint64_t)(cur / base) << shift;
        r = cur % base;
    }
    *rem = r;
    return q;
}

static void emit_number(sink_t *k, uint64_t v, unsigned base, bool upper, bool negative, int width, bool zero)
{
    char digits[24];
    int n = 0;
    do {
        unsigned d;
        v = divide(v, base, &d);
        digits[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
    } while (v != 0);
    int len = n + (negative ? 1 : 0);
    if (negative && zero) {
        emit(k, '-');
    }
    for (; len < width; len++) {
        emit(k, zero ? '0' : ' ');
    }
    if (negative && !zero) {
        emit(k, '-');
    }
    while (n != 0) {
        emit(k, digits[--n]);
    }
}

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
    sink_t k = {out, cap, 0};
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            emit(&k, *fmt);
            continue;
        }
        fmt++;
        bool zero = false;
        int width = 0;
        int precision = -1;
        int longs = 0;
        bool size = false;
        if (*fmt == '0') {
            zero = true;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (fmt[0] == '.' && fmt[1] == '*') {
            precision = va_arg(ap, int);
            fmt += 2;
        }
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        if (*fmt == 'z') {
            size = true;
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            for (int i = 0; s[i] != '\0' && (precision < 0 || i < precision); i++) {
                emit(&k, s[i]);
            }
            break;
        }
        case 'c':
            emit(&k, (char)va_arg(ap, int));
            break;
        case 'd': {
            int64_t v = longs >= 2 ? va_arg(ap, long long) : longs == 1 ? va_arg(ap, long) : va_arg(ap, int);
            emit_number(&k, v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, 10, false, v < 0, width, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = longs >= 2 ? va_arg(ap, unsigned long long)
                         : longs == 1 ? va_arg(ap, unsigned long)
                         : size ? va_arg(ap, size_t)
                                : va_arg(ap, unsigned);
            emit_number(&k, v, *fmt == 'u' ? 10u : 16u, *fmt == 'X', false, width, zero);
            break;
        }
        case '%':
            emit(&k, '%');
            break;
        default:
            /* not needed by the tools built this way: show it rather than guess */
            emit(&k, '?');
            break;
        }
    }
    if (cap != 0) {
        out[k.n < cap ? k.n : cap - 1u] = '\0';
    }
    return (int)k.n;
}

int snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

static int vfprintf_(FILE *f, const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    write_all(f->fd, buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1u);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf_(f, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf_(stdout, fmt, ap);
    va_end(ap);
    return n;
}
