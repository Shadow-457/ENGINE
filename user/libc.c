/* ================================================================
 *  Systrix OS — user/libc.c
 *  Basic C library implementation for user programs.
 *
 *  All syscalls go through the Systrix OS syscall interface
 *  (SYSCALL instruction, Linux x86-64 ABI register convention).
 * ================================================================ */
#include "libc.h"

/* ── errno ───────────────────────────────────────────────────── */
int errno = 0;

/* ================================================================
 *  Syscall primitives
 *  Systrix OS uses the Linux x86-64 SYSCALL convention:
 *    rax = syscall number
 *    args: rdi, rsi, rdx, r10, r8, r9
 *    return in rax (negative = error code)
 * ================================================================ */

static inline long __syscall1(long n, long a1) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(n), "D"(a1)
        : "rcx", "r11", "memory");
    return r;
}

static inline long __syscall2(long n, long a1, long a2) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(n), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory");
    return r;
}

static inline long __syscall3(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(n), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return r;
}

static inline long __syscall6(long n, long a1, long a2, long a3,
                               long a4, long a5, long a6) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return r;
}

static inline long __syscall0(long n) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(n)
        : "rcx", "r11", "memory");
    return r;
}

static inline long __syscall4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
        : "rcx", "r11", "memory");
    return r;
}

static inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return r;
}

/* ── Syscall wrappers ────────────────────────────────────────── */

ssize_t read(int fd, void *buf, size_t count) {
    long r = __syscall3(0, (long)fd, (long)buf, (long)count);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

ssize_t write(int fd, const void *buf, size_t count) {
    long r = __syscall3(1, (long)fd, (long)buf, (long)count);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

int open(const char *path, int flags) {
    long r = __syscall2(2, (long)path, (long)flags);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int close(int fd) {
    long r = __syscall1(3, (long)fd);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

void exit(int code) {
    __syscall1(60, (long)code);
    __builtin_unreachable();
}

void *malloc(size_t n) {
    long r = __syscall1(5, (long)n);
    if (!r) { errno = ENOMEM; return NULL; }
    return (void*)r;
}

void free(void *ptr) {
    if (ptr) __syscall1(6, (long)ptr);
}

pid_t fork(void) {
    long r = __syscall0(57);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}

int execve(const char *path, char **argv, char **envp) {
    long r = __syscall3(59, (long)path, (long)argv, (long)envp);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int waitpid(pid_t pid, int *wstatus, int options) {
    long r = __syscall3(61, (long)pid, (long)wstatus, (long)options);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

pid_t wait(int *wstatus) { return waitpid(-1, wstatus, 0); }

int pipe(int pipefd[2]) {
    long r = __syscall1(22, (long)pipefd);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int dup(int oldfd) {
    long r = __syscall1(32, (long)oldfd);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int dup2(int oldfd, int newfd) {
    long r = __syscall2(33, (long)oldfd, (long)newfd);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int kill(pid_t pid, int sig) {
    long r = __syscall2(62, (long)pid, (long)sig);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

void *signal(int signum, void *handler) {
    long r = __syscall2(13, (long)signum, (long)handler);
    if (r < 0) { errno = (int)-r; return (void*)-1; }
    return (void*)r;
}

pid_t clone(int (*fn)(void*), void *stack, int flags, void *arg) {
    long r = __syscall5(56, (long)flags, (long)stack, 0, 0, (long)arg);
    if (r < 0) { errno = (int)-r; return -1; }
    return (pid_t)r;
}

int socket(int domain, int type, int protocol) {
    long r = __syscall3(41, (long)domain, (long)type, (long)protocol);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

int bind(int sockfd, const void *addr, socklen_t addrlen) {
    long r = __syscall3(49, (long)sockfd, (long)addr, (long)addrlen);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int connect(int sockfd, const void *addr, socklen_t addrlen) {
    long r = __syscall3(42, (long)sockfd, (long)addr, (long)addrlen);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int listen(int sockfd, int backlog) {
    long r = __syscall2(50, (long)sockfd, (long)backlog);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int accept(int sockfd, void *addr, socklen_t *addrlen) {
    long r = __syscall3(43, (long)sockfd, (long)addr, (long)addrlen);
    if (r < 0) { errno = (int)-r; return -1; }
    return (int)r;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    long r = __syscall4(44, (long)sockfd, (long)buf, (long)len, (long)flags);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    long r = __syscall4(45, (long)sockfd, (long)buf, (long)len, (long)flags);
    if (r < 0) { errno = (int)-r; return -1; }
    return (ssize_t)r;
}

int shutdown(int sockfd, int how) {
    long r = __syscall2(48, (long)sockfd, (long)how);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    long r = __syscall6(9, (long)addr, (long)len, (long)prot,
                           (long)flags, (long)fd, (long)off);
    if (r < 0) { errno = (int)-r; return NULL; }
    return (void*)r;
}

int munmap(void *addr, size_t len) {
    long r = __syscall2(11, (long)addr, (long)len);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

void *sbrk(ssize_t increment) {
    /* Get current brk */
    long cur = __syscall1(12, 0);
    if (increment == 0) return (void*)cur;
    long r = __syscall1(12, cur + increment);
    if (r < 0) { errno = ENOMEM; return (void*)-1L; }
    return (void*)cur;   /* return previous break, like POSIX */
}

pid_t getpid(void) {
    return (pid_t)__syscall1(39, 0);
}

/* ================================================================
 *  String functions
 * ================================================================ */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) return (char*)s;
    if (c == '\0') return (char*)s;
    return NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) last = s;
    if (c == '\0') return (char*)s;
    return (char*)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char*)haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++)
        if (strncmp(haystack, needle, nlen) == 0) return (char*)haystack;
    return NULL;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = malloc(len);
    if (p) memcpy(p, s, len);
    return p;
}

/* ================================================================
 *  Memory functions
 * ================================================================ */

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    const uint8_t *s = (const uint8_t*)src;
    if (d < s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t*)a;
    const uint8_t *q = (const uint8_t*)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t*)s;
    while (n--) {
        if (*p == (uint8_t)c) return (void*)p;
        p++;
    }
    return NULL;
}

/* ================================================================
 *  Character classification
 * ================================================================ */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\n' ||
                            c == '\r' || c == '\f' || c == '\v'; }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ================================================================
 *  Number conversion
 * ================================================================ */

static long long __strtoll_base(const char *s, char **endptr, int base,
                                int is_unsigned) {
    while (isspace((unsigned char)*s)) s++;

    int neg = 0;
    if (!is_unsigned) {
        if (*s == '-') { neg = 1; s++; }
        else if (*s == '+') s++;
    }

    if (base == 0 || base == 16) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
            base = 16;
        } else if (base == 0 && s[0] == '0') {
            base = 8; s++;
        } else if (base == 0) {
            base = 10;
        }
    }

    long long result = 0;
    int any = 0;
    for (; *s; s++) {
        int digit;
        if (isdigit(*s))         digit = *s - '0';
        else if (isupper(*s))    digit = *s - 'A' + 10;
        else if (islower(*s))    digit = *s - 'a' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        any = 1;
    }
    if (endptr) *endptr = any ? (char*)s : (char*)(s - (neg ? 1 : 0));
    return neg ? -result : result;
}

long strtol(const char *s, char **end, int base) {
    return (long)__strtoll_base(s, end, base, 0);
}

long long strtoll(const char *s, char **end, int base) {
    return __strtoll_base(s, end, base, 0);
}

unsigned long strtoul(const char *s, char **end, int base) {
    return (unsigned long)__strtoll_base(s, end, base, 1);
}

int atoi(const char *s)        { return (int)strtol(s, NULL, 10); }
long atol(const char *s)       { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

/* ================================================================
 *  Formatted output — vsnprintf core
 * ================================================================ */

/* Write a single char into buf (with bounds check) */
#define _PUTC(c) do {                     \
    if (pos < size - 1) buf[pos] = (c);  \
    pos++;                                \
} while(0)

/* Emit a string with optional width/left-align */
static size_t _emit_str(char *buf, size_t size, size_t pos,
                         const char *s, int width, int left, int prec) {
    size_t slen = strlen(s);
    if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;
    int pad = (width > 0 && (size_t)width > slen) ? (int)((size_t)width - slen) : 0;
    if (!left) while (pad-- > 0) _PUTC(' ');
    for (size_t i = 0; i < slen; i++) _PUTC(s[i]);
    if (left)  while (pad-- > 0) _PUTC(' ');
    return pos;
}

/* Emit an unsigned integer (any base) */
static size_t _emit_uint(char *buf, size_t size, size_t pos,
                          unsigned long long v, int base, int upper,
                          int width, int zero_pad, int left, int prefix) {
    char tmp[66];
    int  tlen = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) { tmp[tlen++] = '0'; }
    else { while (v) { tmp[tlen++] = digits[v % (unsigned)base]; v /= (unsigned)base; } }

    /* prefix: 0x / 0X / 0 */
    char pre[3]; int prelen = 0;
    if (prefix) {
        if (base == 16) { pre[prelen++] = '0'; pre[prelen++] = upper ? 'X' : 'x'; }
        else if (base == 8 && !(tlen == 1 && tmp[0] == '0')) { pre[prelen++] = '0'; }
    }

    int total = tlen + prelen;
    int pad = (width > total) ? width - total : 0;
    char padch = (zero_pad && !left) ? '0' : ' ';

    if (!left && padch == ' ') while (pad-- > 0) _PUTC(' ');
    for (int i = 0; i < prelen; i++) _PUTC(pre[i]);
    if (!left && padch == '0') while (pad-- > 0) _PUTC('0');
    while (tlen-- > 0) _PUTC(tmp[tlen]);   /* tmp is reversed */
    if (left) while (pad-- > 0) _PUTC(' ');
    return pos;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    if (!buf || size == 0) return 0;
    size_t pos = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') { _PUTC(*fmt); continue; }
        fmt++;
        if (!*fmt) break;

        /* Flags */
        int left = 0, zero_pad = 0, alt = 0, plus = 0, space = 0;
        for (;;) {
            if (*fmt == '-')      { left = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else if (*fmt == '#') { alt = 1; fmt++; }
            else if (*fmt == '+') { plus = 1; fmt++; }
            else if (*fmt == ' ') { space = 1; fmt++; }
            else break;
        }

        /* Width */
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (isdigit(*fmt)) width = width * 10 + (*fmt++ - '0');

        /* Precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) prec = prec * 10 + (*fmt++ - '0');
        }

        /* Length modifier */
        int lmod = 0; /* 0=int, 1=long, 2=long long */
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { lmod = 2; fmt++; }
            else lmod = 1;
        } else if (*fmt == 'h') {
            fmt++; if (*fmt == 'h') fmt++;
        } else if (*fmt == 'z' || *fmt == 't') {
            lmod = 1; fmt++;
        }

        char spec = *fmt;
        if (!spec) break;

        switch (spec) {
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = (width > 1) ? width - 1 : 0;
            if (!left) while (pad-- > 0) _PUTC(' ');
            _PUTC(c);
            if (left)  while (pad-- > 0) _PUTC(' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            pos = _emit_str(buf, size, pos, s, width, left, prec);
            break;
        }
        case 'd': case 'i': {
            long long v;
            if (lmod == 2)      v = va_arg(ap, long long);
            else if (lmod == 1) v = va_arg(ap, long);
            else                v = va_arg(ap, int);
            char tmp[24]; int tlen = 0;
            int neg = (v < 0);
            unsigned long long uv = neg ? (unsigned long long)-v : (unsigned long long)v;
            if (uv == 0) tmp[tlen++] = '0';
            else { unsigned long long t = uv; while(t){tmp[tlen++]='0'+(int)(t%10);t/=10;} }
            char sign = neg ? '-' : (plus ? '+' : (space ? ' ' : 0));
            int total = tlen + (sign ? 1 : 0);
            int pad = (width > total) ? width - total : 0;
            char padch = (zero_pad && !left) ? '0' : ' ';
            if (!left && padch == ' ') while (pad-- > 0) _PUTC(' ');
            if (sign) _PUTC(sign);
            if (!left && padch == '0') while (pad-- > 0) _PUTC('0');
            while (tlen-- > 0) _PUTC(tmp[tlen]);
            if (left) while (pad-- > 0) _PUTC(' ');
            break;
        }
        case 'u': {
            unsigned long long v;
            if (lmod == 2)      v = va_arg(ap, unsigned long long);
            else if (lmod == 1) v = va_arg(ap, unsigned long);
            else                v = va_arg(ap, unsigned int);
            pos = _emit_uint(buf, size, pos, v, 10, 0, width, zero_pad, left, 0);
            break;
        }
        case 'x': case 'X': {
            unsigned long long v;
            if (lmod == 2)      v = va_arg(ap, unsigned long long);
            else if (lmod == 1) v = va_arg(ap, unsigned long);
            else                v = va_arg(ap, unsigned int);
            pos = _emit_uint(buf, size, pos, v, 16, spec=='X', width, zero_pad, left, alt);
            break;
        }
        case 'o': {
            unsigned long long v;
            if (lmod == 2)      v = va_arg(ap, unsigned long long);
            else if (lmod == 1) v = va_arg(ap, unsigned long);
            else                v = va_arg(ap, unsigned int);
            pos = _emit_uint(buf, size, pos, v, 8, 0, width, zero_pad, left, alt);
            break;
        }
        case 'p': {
            void *_pv = va_arg(ap, void*);
            unsigned long long v;
            __builtin_memcpy(&v, &_pv, sizeof(_pv));
            /* Always print "0x" prefix for pointers */
            _PUTC('0'); _PUTC('x');
            pos = _emit_uint(buf, size, pos, v, 16, 0,
                             width > 2 ? width - 2 : 0, 1, left, 0);
            break;
        }
        case '%':
            _PUTC('%');
            break;
        case 'f': case 'F':
        case 'e': case 'E':
        case 'g': case 'G': {
            /* Software float-to-string. No FPU instructions — uses
             * __builtin_memcpy to reinterpret the double bits, then
             * pure integer arithmetic for the conversion.           */
            double dv = va_arg(ap, double);
            if (prec < 0) prec = 6;

            /* Decompose sign */
            char fsign = 0;
            if (dv < 0.0) { dv = -dv; fsign = '-'; }
            else if (plus)  fsign = '+';
            else if (space) fsign = ' ';

            /* Reinterpret bits to detect inf/nan */
            unsigned long long dbits;
            __builtin_memcpy(&dbits, &dv, 8);
            unsigned int fexp  = (unsigned int)((dbits >> 52) & 0x7FF);
            unsigned long long fmant = dbits & 0x000FFFFFFFFFFFFFULL;

            char ftmp[64];
            int  flen = 0;

            if (fexp == 0x7FF) {
                /* inf or nan */
                const char *fs = fmant ? "nan" : "inf";
                if (fsign) ftmp[flen++] = fsign;
                ftmp[flen++] = (spec >= 'a') ? fs[0] : (char)(fs[0]-32);
                ftmp[flen++] = (spec >= 'a') ? fs[1] : (char)(fs[1]-32);
                ftmp[flen++] = (spec >= 'a') ? fs[2] : (char)(fs[2]-32);
            } else {
                /* Limit precision to avoid buffer overrun */
                if (prec > 17) prec = 17;

                /* Extract integer and fractional parts using integer math.
                 * Multiply fractional part by 10^prec to get decimal digits. */

                /* Integer part */
                unsigned long long ipart = (unsigned long long)dv;
                /* Fractional part scaled by 10^prec */
                double frac = dv - (double)ipart;
                /* Multiply up: 10^prec (max 10^17 fits in u64) */
                unsigned long long scale = 1;
                for (int pi = 0; pi < prec; pi++) scale *= 10;
                unsigned long long frac_digits = (unsigned long long)(frac * (double)scale + 0.5);
                /* Carry if frac_digits rounded up to scale */
                if (frac_digits >= scale) { ipart++; frac_digits -= scale; }

                /* Build integer part string (reversed) */
                char ibuf[24]; int ilen = 0;
                if (ipart == 0) ibuf[ilen++] = '0';
                else { unsigned long long t = ipart; while(t){ibuf[ilen++]='0'+(int)(t%10);t/=10;} }

                /* Sign */
                if (fsign) ftmp[flen++] = fsign;
                /* Integer digits (un-reverse) */
                for (int ii = ilen-1; ii >= 0; ii--) ftmp[flen++] = ibuf[ii];

                if (prec > 0 || alt) {
                    ftmp[flen++] = '.';
                    /* Fractional digits — leading zeros matter */
                    char fbuf[20]; int fdi = 0;
                    unsigned long long fd = frac_digits;
                    for (int pi = 0; pi < prec; pi++) {
                        fbuf[fdi++] = '0' + (int)(fd % 10);
                        fd /= 10;
                    }
                    /* fbuf is reversed — emit in reverse */
                    for (int fi = fdi-1; fi >= 0; fi--) ftmp[flen++] = fbuf[fi];
                }
            }

            /* Pad and emit */
            int fpad = (width > flen) ? width - flen : 0;
            char fpadch = (zero_pad && !left) ? '0' : ' ';
            if (!left && fpadch == ' ') while (fpad-- > 0) _PUTC(' ');
            /* For zero-pad: emit sign before zeros */
            if (!left && fpadch == '0') {
                if (flen > 0 && (ftmp[0]=='-'||ftmp[0]=='+'||ftmp[0]==' ')) {
                    _PUTC(ftmp[0]);
                    for (int fi = 1; fi < flen; fi++) _PUTC(ftmp[fi]);
                    goto float_done; /* sign already emitted */
                }
                while (fpad-- > 0) _PUTC('0');
            }
            for (int fi = 0; fi < flen; fi++) _PUTC(ftmp[fi]);
            float_done:
            if (left) while (fpad-- > 0) _PUTC(' ');
            break;
        }
        case 'n': {
            /* write chars-so-far into *arg — standard but rarely used */
            int *np = va_arg(ap, int*);
            if (np) *np = (int)pos;
            break;
        }
        default:
            /* Unknown specifier: emit literally */
            _PUTC('%'); _PUTC(spec);
            break;
        }

        (void)plus; (void)space; /* silence unused-variable warnings */
    }

    /* NUL-terminate */
    buf[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int vfprintf(int fd, const char *fmt, va_list ap) {
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    write(fd, tmp, (size_t)(n < 0 ? 0 : n));
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(STDOUT_FILENO, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(int fd, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* ================================================================
 *  Simple I/O helpers
 * ================================================================ */

int putchar(int c) {
    uint8_t ch = (uint8_t)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}

int puts(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
    write(STDOUT_FILENO, "\r\n", 2);   /* \r\n for VGA terminal */
    return 0;
}

int getchar(void) {
    uint8_t c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return (n == 1) ? (int)(unsigned char)c : EOF;
}

char *gets_s(char *buf, size_t n) {
    if (!buf || n == 0) return NULL;
    ssize_t r = read(STDIN_FILENO, buf, n - 1);
    if (r <= 0) return NULL;
    buf[r] = '\0';
    /* Strip trailing newline */
    if (r > 0 && (buf[r-1] == '\n' || buf[r-1] == '\r')) buf[r-1] = '\0';
    return buf;
}

/* ================================================================
 *  Utility
 * ================================================================ */

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr)  return malloc(size);
    if (!size) { free(ptr); return NULL; }
    void *newp = malloc(size);
    if (!newp) return NULL;
    /* We don't know the old size; copy 'size' bytes.
     * This is safe as long as size <= old allocation. */
    memcpy(newp, ptr, size);
    free(ptr);
    return newp;
}

void abort(void) {
    exit(1);
}

int abs(int x)   { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

/* ================================================================
 *  FILE* stdio layer
 * ================================================================ */

/* Static FILE pool — no heap needed for the 16 possible streams */
static FILE _file_pool[FOPEN_MAX];
static int  _file_pool_init = 0;

/* Standard streams */
static FILE _stdin_file  = { .fd = 0, .flags = _FILE_READ  | _FILE_STRM };
static FILE _stdout_file = { .fd = 1, .flags = _FILE_WRITE | _FILE_STRM };
static FILE _stderr_file = { .fd = 2, .flags = _FILE_WRITE | _FILE_STRM };

FILE *stdin  = &_stdin_file;
FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;

static void _file_pool_setup(void) {
    if (_file_pool_init) return;
    for (int i = 0; i < FOPEN_MAX; i++)
        _file_pool[i].fd = -1;
    _file_pool_init = 1;
}

static FILE *_file_alloc(void) {
    _file_pool_setup();
    for (int i = 0; i < FOPEN_MAX; i++)
        if (_file_pool[i].fd == -1) return &_file_pool[i];
    return NULL;
}

/* ── fflush ───────────────────────────────────────────────────── */
int fflush(FILE *f) {
    if (!f || !(f->flags & _FILE_WRITE)) return 0;
    if (f->wpos == 0) return 0;
    int n = (int)write(f->fd, f->wbuf, (size_t)f->wpos);
    f->wpos = 0;
    if (n < 0) { f->flags |= _FILE_ERR; return -1; }
    return 0;
}

/* ── fopen ────────────────────────────────────────────────────── */
FILE *fopen(const char *path, const char *mode) {
    int writing = 0, reading = 0;
    for (const char *m = mode; *m; m++) {
        if (*m == 'r') reading = 1;
        if (*m == 'w' || *m == 'a') writing = 1;
        if (*m == '+') { reading = 1; writing = 1; }
    }

    /* O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT=0x40, O_TRUNC=0x200 */
    int flags = 0;
    if (reading && writing) flags = 2 | 0x40;
    else if (writing)       flags = 1 | 0x40 | 0x200;
    else                    flags = 0;

    int fd = open(path, flags);
    if (fd < 0) return NULL;

    FILE *f = _file_alloc();
    if (!f) { close(fd); return NULL; }

    f->fd    = fd;
    f->flags = (reading ? _FILE_READ : 0) | (writing ? _FILE_WRITE : 0);
    f->wpos  = 0;
    f->rpos  = 0;
    f->rlen  = 0;

    /* Append mode: seek to end */
    if (mode[0] == 'a') {
        /* syscall 62 = lseek, whence 2 = SEEK_END */
        long long r;
        __asm__ volatile("syscall"
            : "=a"(r)
            : "0"(62LL), "D"((long long)fd), "S"(0LL), "d"(2LL)
            : "rcx","r11","memory");
    }
    return f;
}

/* ── fclose ───────────────────────────────────────────────────── */
int fclose(FILE *f) {
    if (!f || f->fd < 0) return -1;
    fflush(f);
    close(f->fd);
    f->fd    = -1;
    f->flags = 0;
    f->wpos  = 0;
    f->rpos  = 0;
    f->rlen  = 0;
    return 0;
}

/* ── fread ────────────────────────────────────────────────────── */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !(f->flags & _FILE_READ) || (f->flags & _FILE_EOF)) return 0;
    size_t total = size * nmemb;
    if (total == 0) return 0;
    size_t got = 0;
    unsigned char *out = (unsigned char *)ptr;

    /* Drain read buffer first */
    while (got < total && f->rpos < f->rlen) {
        out[got++] = f->rbuf[f->rpos++];
    }

    /* Direct read for remainder */
    while (got < total) {
        ssize_t n = read(f->fd, out + got, total - got);
        if (n <= 0) {
            if (n == 0) f->flags |= _FILE_EOF;
            else        f->flags |= _FILE_ERR;
            break;
        }
        got += (size_t)n;
    }
    return (size > 0) ? got / size : 0;
}

/* ── fgetc ────────────────────────────────────────────────────── */
int fgetc(FILE *f) {
    if (!f || !(f->flags & _FILE_READ) || (f->flags & _FILE_EOF)) return EOF;
    if (f->rpos < f->rlen) return (unsigned char)f->rbuf[f->rpos++];
    /* Refill buffer */
    ssize_t n = read(f->fd, f->rbuf, BUFSIZ);
    if (n <= 0) { f->flags |= (n == 0 ? _FILE_EOF : _FILE_ERR); return EOF; }
    f->rpos = 0; f->rlen = (int)n;
    return (unsigned char)f->rbuf[f->rpos++];
}

/* ── fgets ────────────────────────────────────────────────────── */
char *fgets(char *buf, int n, FILE *f) {
    if (!buf || n <= 0 || !f) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return NULL; break; }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}

/* ── fwrite ───────────────────────────────────────────────────── */
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || !(f->flags & _FILE_WRITE)) return 0;
    size_t total = size * nmemb;
    if (total == 0) return 0;
    const unsigned char *src = (const unsigned char *)ptr;
    size_t written = 0;

    if (f->flags & _FILE_STRM) {
        /* Unbuffered — write directly */
        ssize_t n = write(f->fd, src, total);
        if (n < 0) { f->flags |= _FILE_ERR; return 0; }
        return (size > 0) ? (size_t)n / size : 0;
    }

    while (written < total) {
        int space = BUFSIZ - f->wpos;
        int chunk = (int)(total - written);
        if (chunk > space) chunk = space;
        for (int i = 0; i < chunk; i++) f->wbuf[f->wpos++] = src[written + i];
        written += (size_t)chunk;
        if (f->wpos == BUFSIZ) if (fflush(f) < 0) break;
    }
    return (size > 0) ? written / size : 0;
}

/* ── fputc ────────────────────────────────────────────────────── */
int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, f) != 1) return EOF;
    return c;
}

/* ── fputs ────────────────────────────────────────────────────── */
int fputs(const char *s, FILE *f) {
    if (!s) return EOF;
    size_t len = strlen(s);
    return (fwrite(s, 1, len, f) == len) ? 0 : EOF;
}

/* ── fprintf / vfprintf ──────────────────────────────────────── */
int vfprint(FILE *f, const char *fmt, __builtin_va_list ap) {
    char tmp[2048];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n <= 0) return n;
    fwrite(tmp, 1, (size_t)n, f);
    return n;
}

int fprint(FILE *f, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vfprint(f, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

/* ── fseek / ftell / rewind ──────────────────────────────────── */
int fseek(FILE *f, long offset, int whence) {
    if (!f || f->fd < 0) return -1;
    fflush(f);
    /* Invalidate read buffer */
    f->rpos = 0; f->rlen = 0;
    f->flags &= ~_FILE_EOF;
    long long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(62LL), "D"((long long)f->fd),
          "S"((long long)offset), "d"((long long)whence)
        : "rcx","r11","memory");
    return (r < 0) ? -1 : 0;
}

long ftell(FILE *f) {
    if (!f || f->fd < 0) return -1;
    long long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(62LL), "D"((long long)f->fd),
          "S"(0LL), "d"(1LL)           /* SEEK_CUR, offset 0 = current pos */
        : "rcx","r11","memory");
    /* Subtract unread buffered bytes from reported position */
    return (long)(r - (long long)(f->rlen - f->rpos));
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }

/* ── feof / ferror / clearerr ───────────────────────────────── */
int feof(FILE *f)    { return f && (f->flags & _FILE_EOF)  ? 1 : 0; }
int ferror(FILE *f)  { return f && (f->flags & _FILE_ERR)  ? 1 : 0; }
void clearerr(FILE *f) { if (f) f->flags &= ~(_FILE_EOF | _FILE_ERR); }

/* ── remove / rename ─────────────────────────────────────────── */
int remove(const char *path) {
    long long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(87LL), "D"(path)
        : "rcx","r11","memory");
    return (int)r;
}

int rename_file(const char *old, const char *nw) {
    long long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(82LL), "D"(old), "S"(nw)
        : "rcx","r11","memory");
    return (int)r;
}

/* ── environ — defined once here, extern in libc.h ─────────────── */
#define ENV_MAX     128
static char  *_environ_ptrs[ENV_MAX + 1];
char        **environ = _environ_ptrs;

/* ── env_init_defaults — called from crt0.S before main() ──────── */
/* setenv is defined as static inline in libc.h and visible here    */
void env_init_defaults(void) {
    setenv("PATH",    "/bin:/usr/bin", 0);
    setenv("HOME",    "/",            0);
    setenv("TMPDIR",  "/tmp",         0);
    setenv("LANG",    "C",            0);
    setenv("LC_ALL",  "C",            0);
    setenv("TZ",      "UTC",          0);
    setenv("TERM",    "vt100",        0);
    setenv("USER",    "root",         0);
    setenv("LOGNAME", "root",         0);
}

/* ── locale stubs ─────────────────────────────────────────────── */
/* ICU, HarfBuzz, and other libs call setlocale at init.
   Always return "C" — no locale switching supported. */

/* struct lconv defined in libc.h */

char *setlocale(int category, const char *locale) {
    (void)category; (void)locale;
    return "C";
}

static struct lconv _lconv_C = {
    ".", "", "", "", "", "", "", "", "", "",
    127, 127, 127, 127, 127, 127, 127, 127,
    127, 127, 127, 127, 127, 127
};

struct lconv *localeconv(void) {
    return &_lconv_C;
}

/* ================================================================
 *  POSIX additions — needed for Lynx and browser support
 * ================================================================ */

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)__syscall2(35, (long)req, (long)rem);
}

unsigned int sleep(unsigned int seconds) {
    struct timespec req = { (long)seconds, 0 };
    nanosleep(&req, 0);
    return 0;
}

int usleep(unsigned int us) {
    struct timespec req = { 0, (long)us * 1000 };
    return nanosleep(&req, 0);
}

int chdir(const char *path) {
    return (int)__syscall1(80, (long)path);
}

char *getcwd(char *buf, size_t size) {
    long r = __syscall2(79, (long)buf, (long)size);
    return r < 0 ? NULL : buf;
}

int access(const char *path, int mode) {
    return (int)__syscall2(21, (long)path, (long)mode);
}

ssize_t readlink(const char *path, char *buf, size_t bufsz) {
    return (ssize_t)__syscall3(89, (long)path, (long)buf, (long)bufsz);
}

int lstat(const char *path, struct stat *buf) {
    return (int)__syscall2(6, (long)path, (long)buf);
}

int stat(const char *path, struct stat *buf) {
    return (int)__syscall4(262, (long)-100 /* AT_FDCWD */, (long)path, (long)buf, 0);
}

int fstat(int fd, struct stat *buf) {
    return (int)__syscall2(5, (long)fd, (long)buf);  /* sys_fstat */
}

int ioctl(int fd, unsigned long req, void *arg) {
    return (int)__syscall3(16, (long)fd, (long)req, (long)arg);
}

int sysinfo(struct sysinfo *info) {
    return (int)__syscall1(99, (long)info);
}

pid_t getppid(void) {
    return (pid_t)__syscall0(110);
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    return (int)__syscall3(13, (long)signum, (long)act, (long)oldact);
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return (int)__syscall3(14, (long)how, (long)set, (long)oldset);
}

int sigemptyset(sigset_t *set) {
    if (set) *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (set) *set = ~(sigset_t)0;
    return 0;
}

int sigaddset(sigset_t *set, int signum) {
    if (set && signum > 0 && signum < 64) *set |= (1ULL << signum);
    return 0;
}

int sigdelset(sigset_t *set, int signum) {
    if (set && signum > 0 && signum < 64) *set &= ~(1ULL << signum);
    return 0;
}

int sigismember(const sigset_t *set, int signum) {
    if (!set || signum <= 0 || signum >= 64) return 0;
    return (*set >> signum) & 1;
}

/* getrlimit / setrlimit — Lynx checks these */
int getrlimit(int resource, struct rlimit *rlim) {
    (void)resource;
    if (rlim) {
        rlim->rlim_cur = 0x7FFFFFFFFFFFFFFF;
        rlim->rlim_max = 0x7FFFFFFFFFFFFFFF;
    }
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim) {
    (void)resource; (void)rlim;
    return 0;
}

/* times — Lynx and some libc init code calls this */
clock_t times(struct tms *buf) {
    if (buf) {
        buf->tms_utime = 0;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return 0;
}

/* gettid — used by some threading stubs */
pid_t gettid(void) {
    return getpid();
}

/* getrusage stub */
int getrusage(int who, struct rusage *usage) {
    (void)who;
    if (usage) memset(usage, 0, sizeof(*usage));
    return 0;
}

/* fcntl — already in socket layer but user code calls it directly */
int fcntl(int fd, int cmd, long arg) {
    return (int)__syscall3(72, (long)fd, (long)cmd, (long)arg);
}

/* uname */
int uname(struct utsname *buf) {
    return (int)__syscall1(63, (long)buf);
}

/* isatty */
int isatty(int fd) {
    struct termios t;
    return ioctl(fd, 0x5401 /* TCGETS */, &t) == 0;
}

/* link / symlink / unlinkat stubs — FAT32 has no links */
int link(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath; return -1;
}
int symlink(const char *target, const char *linkpath) {
    (void)target; (void)linkpath; return -1;
}
int unlinkat(int dirfd, const char *path, int flags) {
    (void)dirfd; (void)flags;
    return (int)__syscall1(87, (long)path);
}

/* mkdir / rmdir */
int mkdir(const char *path, int mode) {
    return (int)__syscall2(83, (long)path, (long)mode);
}
int rmdir(const char *path) {
    return (int)__syscall1(84, (long)path);
}

/* rename */
int rename(const char *old, const char *newp) {
    return (int)__syscall2(82, (long)old, (long)newp);
}

/* opendir / readdir / closedir */
DIR *opendir(const char *path) {
    int fd = open(path, 0);
    if (fd < 0) return NULL;
    DIR *d = malloc(sizeof(DIR));
    if (!d) { close(fd); return NULL; }
    d->fd = fd;
    d->buf_pos = 0;
    d->buf_len = 0;
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) return NULL;
    if (d->buf_pos >= d->buf_len) {
        long n = __syscall3(78, (long)d->fd, (long)d->buf, sizeof(d->buf));
        if (n <= 0) return NULL;
        d->buf_len = (int)n;
        d->buf_pos = 0;
    }
    struct dirent *de = (struct dirent*)(d->buf + d->buf_pos);
    d->buf_pos += de->d_reclen;
    return de;
}

int closedir(DIR *d) {
    if (!d) return -1;
    close(d->fd);
    free(d);
    return 0;
}

/* inet helpers used by Lynx / curl-like code */
unsigned int inet_addr(const char *cp) {
    unsigned a=0, b=0, c=0, d=0;
    int n = 0;
    const char *p = cp;
    while (*p) {
        if (*p == '.') n++;
        else {
            unsigned *oc = n==0?&a:n==1?&b:n==2?&c:&d;
            *oc = *oc * 10 + (unsigned)(*p - '0');
        }
        p++;
    }
    return (d<<24)|(c<<16)|(b<<8)|a;  /* little-endian packed */
}

unsigned short htons(unsigned short v) {
    return (unsigned short)((v>>8)|(v<<8));
}
unsigned short ntohs(unsigned short v) { return htons(v); }
unsigned int   htonl(unsigned int v) {
    return ((v>>24)&0xFF)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|((v<<24)&0xFF000000u);
}
unsigned int   ntohl(unsigned int v) { return htonl(v); }
