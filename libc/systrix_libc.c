/* ═══════════════════════════════════════════════════════════════════════
 *  SystrixOS — libc/systrix_libc.c
 *  Unified C library implementation — kernel AND user space.
 *
 *  Build flags expected:
 *    Kernel: -ffreestanding -nostdlib -nostdinc -DSYSTRIX_KERNEL -O2
 *    User:   -ffreestanding -nostdlib -nostdinc -O2
 *
 *  No #include of anything outside this directory.
 *
 *  SECTION ORDER (matches systrix_libc.h — keep in sync!):
 *   §1  Output sink (raw write to stderr / kputs)
 *   §2  Debug, Assert, Panic implementations  ← NEW
 *   §3  Memory
 *   §4  String — length / comparison
 *   §5  String — copy
 *   §6  String — concatenation
 *   §7  String — search
 *   §8  Character classification
 *   §9  Integer conversion
 *   §10 Numeric helpers
 *   §11 Integer-to-string helpers
 *   §12 Formatted output (slibc_vprintf_cb + snprintf family)
 *   §13 Sorting / searching (qsort, bsearch)
 *   §14 strerror — human-readable error messages
 *   §15 Integer math & bit manipulation
 *   §16 String extras
 *   §17 Hashing (FNV-1a, MurmurHash3, CRC-32, Adler-32)
 *   §18 PRNG (splitmix64 + xorshift64)
 *   §19 Ring buffer
 *   §20 Bitmap
 *   §21 Formatting helpers (bytes, duration, IPv4, MAC, progress)
 *   §22 Dynamic string builder (SStrBuf)
 *   §23 Path utilities
 *   §24 UUID v4
 * ═══════════════════════════════════════════════════════════════════════ */

#include "systrix_libc.h"


/* ═══════════════════════════════════════════════════════════════════════
 *  §1  Raw output sink
 *
 *  All debug/panic output flows through slibc_debug_puts() and
 *  slibc_debug_printf(). In kernel builds, kputs() is used.
 *  In user builds, write() syscall (fd 2 = stderr) is used.
 *
 *  CONTRIBUTOR: if you add a new output channel (serial, VGA, etc.),
 *  wire it in here — you don't need to touch any of the debug macros.
 * ═══════════════════════════════════════════════════════════════════════ */

#ifdef SYSTRIX_KERNEL

/* Kernel provides kprintf() */
extern void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void kputs(const char *s) { kprintf("%s", s); }
static void kputc(char c)        { kprintf("%c", c); }

void slibc_debug_puts(const char *s) {
    kputs(s);
}

#else  /* user space */

/* Use write(2, buf, len) syscall directly — no libc dependency */
static void _raw_write(const char *buf, size_t len) {
    /* syscall: write(fd=2, buf, len) */
    __asm__ volatile(
        "syscall"
        :
        : "D"((long)2),          /* rdi = fd = stderr */
          "S"(buf),              /* rsi = buf         */
          "d"(len),              /* rdx = len         */
          "a"((long)1)           /* rax = SYS_write   */
        : "rcx", "r11", "memory"
    );
}

void slibc_debug_puts(const char *s) {
    if (!s) return;
    size_t n = 0;
    while (s[n]) n++;
    _raw_write(s, n);
}

#endif /* SYSTRIX_KERNEL */

void slibc_debug_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) slibc_debug_puts(buf);
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §2  Debug, Assert, and Panic implementations
 *
 *  These produce human-readable output, NOT register numbers.
 *
 *  Output examples:
 *
 *  PANIC:
 *    ╔══════════════════════════════════════════════════════╗
 *    ║  SYSTRIX PANIC                                       ║
 *    ╚══════════════════════════════════════════════════════╝
 *    Location : myfile.c : 42 in my_function()
 *    Reason   : out of memory allocating page table
 *
 *  ASSERT FAILED:
 *    ╔══════════════════════════════════════════════════════╗
 *    ║  SYSTRIX ASSERT FAILED                               ║
 *    ╚══════════════════════════════════════════════════════╝
 *    Location  : myfile.c : 42 in my_function()
 *    Condition : ptr != NULL
 *    Message   : pointer must not be null here
 * ═══════════════════════════════════════════════════════════════════════ */

/* Write an integer as decimal to stderr without using snprintf
   (safe to call even if snprintf itself panics) */
static void _write_dec(long long v) {
    char buf[24];
    int n = 0;
    if (v < 0) { slibc_debug_puts("-"); v = -v; }
    if (v == 0) { slibc_debug_puts("0"); return; }
    unsigned long long u = (unsigned long long)v;
    while (u) { buf[n++] = (char)('0' + u % 10); u /= 10; }
    /* reverse */
    char rev[24];
    for (int i = 0; i < n; i++) rev[i] = buf[n - 1 - i];
    rev[n] = '\0';
    slibc_debug_puts(rev);
}

/* Print the panic / assert banner */
static void _print_banner(const char *title) {
    slibc_debug_puts("\n");
    slibc_debug_puts("╔══════════════════════════════════════════════════════╗\n");
    slibc_debug_puts("║  ");
    slibc_debug_puts(title);
    slibc_debug_puts("\n");
    slibc_debug_puts("╚══════════════════════════════════════════════════════╝\n");
}

__attribute__((noreturn))
void slibc_panic_impl(const char *file, int line,
                      const char *func, const char *msg) {
    _print_banner("SYSTRIX PANIC");
    slibc_debug_puts("Location : ");
    slibc_debug_puts(file ? file : "?");
    slibc_debug_puts(" : ");
    _write_dec(line);
    slibc_debug_puts(" in ");
    slibc_debug_puts(func ? func : "?");
    slibc_debug_puts("()\n");
    slibc_debug_puts("Reason   : ");
    slibc_debug_puts(msg ? msg : "(no message)");
    slibc_debug_puts("\n\n");

#ifdef SYSTRIX_KERNEL
    /* Halt the CPU in the kernel */
    __asm__ volatile("cli; hlt" ::: "memory");
    __builtin_unreachable();
#else
    /* User space: call _exit(1) via syscall */
    __asm__ volatile(
        "syscall"
        :
        : "D"((long)1),   /* exit code = 1 */
          "a"((long)60)   /* SYS_exit       */
        :
    );
    __builtin_unreachable();
#endif
}

__attribute__((noreturn))
void slibc_assert_impl(const char *file, int line, const char *func,
                       const char *cond_str, const char *msg) {
    _print_banner("SYSTRIX ASSERT FAILED");
    slibc_debug_puts("Location  : ");
    slibc_debug_puts(file ? file : "?");
    slibc_debug_puts(" : ");
    _write_dec(line);
    slibc_debug_puts(" in ");
    slibc_debug_puts(func ? func : "?");
    slibc_debug_puts("()\n");
    slibc_debug_puts("Condition : ");
    slibc_debug_puts(cond_str ? cond_str : "?");
    slibc_debug_puts("\n");
    slibc_debug_puts("Message   : ");
    slibc_debug_puts(msg ? msg : "(no message)");
    slibc_debug_puts("\n\n");
    slibc_panic_impl(file, line, func, "assertion failure (see above)");
}

__attribute__((noreturn))
void slibc_assert_range_impl(const char *file, int line, const char *func,
                              const char *name, long long val,
                              long long lo, long long hi) {
    char msg[128];
    /* Build message: "value N is out of range [lo, hi]" */
    snprintf(msg, sizeof(msg),
             "value %lld is out of range [%lld, %lld]", val, lo, hi);

    char cond[64];
    snprintf(cond, sizeof(cond), "%s in range [%lld, %lld]", name, lo, hi);

    slibc_assert_impl(file, line, func, cond, msg);
}

/*
 * slibc_hexdump_impl — dump memory as hex + printable ASCII.
 *
 * Output format (16 bytes per line):
 *   [HEXDUMP myfile.c:42] label (N bytes):
 *   0000: 48 65 6c 6c  6f 20 57 6f  72 6c 64 0a  00 00 00 00  Hello Wo rld.....
 */
void slibc_hexdump_impl(const char *file, int line, const char *label,
                        const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    char line_buf[128];

    slibc_debug_printf("[HEXDUMP %s:%d] %s (%zu bytes):\n",
                       file, line, label ? label : "", len);

    for (size_t offset = 0; offset < len; offset += 16) {
        /* offset */
        snprintf(line_buf, 8, "%04zx: ", offset);
        slibc_debug_puts(line_buf);

        /* hex bytes, grouped as 4+4+4+4 */
        for (size_t i = 0; i < 16; i++) {
            if (offset + i < len)
                slibc_debug_printf("%02x ", p[offset + i]);
            else
                slibc_debug_puts("   ");
            if (i == 3 || i == 7 || i == 11)
                slibc_debug_puts(" ");
        }

        slibc_debug_puts(" ");

        /* ASCII column */
        for (size_t i = 0; i < 16 && offset + i < len; i++) {
            uint8_t c = p[offset + i];
            slibc_debug_puts((c >= 0x20 && c <= 0x7e)
                             ? (char[]){(char)c, '\0'}
                             : ".");
        }
        slibc_debug_puts("\n");
    }
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §3  Memory
 * ═══════════════════════════════════════════════════════════════════════ */

void *memset(void *dst, int c, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    unsigned char  v = (unsigned char)c;
    for (size_t i = 0; i < n; i++) p[i] = v;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s || d >= s + n)
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    else
        for (size_t i = n; i-- > 0;) d[i] = s[i];
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)c;
    for (size_t i = 0; i < n; i++)
        if (p[i] == v) return (void *)(p + i);
    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §4  String — length / comparison
 * ═══════════════════════════════════════════════════════════════════════ */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

size_t strnlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++; b++; n--;
    }
    if (n == 0) return 0;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §5  String — copy
 * ═══════════════════════════════════════════════════════════════════════ */

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

/* strlcpy: always NUL-terminates; returns strlen(src).
 * Prefer this over strcpy/strncpy everywhere. */
size_t strlcpy(char *dst, const char *src, size_t sz) {
    size_t i = 0;
    if (sz > 0) {
        for (; i < sz - 1 && src[i]; i++) dst[i] = src[i];
        dst[i] = '\0';
    }
    while (src[i]) i++;
    return i;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §6  String — concatenation
 * ═══════════════════════════════════════════════════════════════════════ */

char *strcat(char *dst, const char *src) {
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (*d) d++;
    while (n-- && (*d++ = *src++)) {}
    *d = '\0';
    return dst;
}

/* strlcat: always NUL-terminates; returns total desired length.
 * Prefer this over strcat/strncat everywhere. */
size_t strlcat(char *dst, const char *src, size_t sz) {
    size_t dl = 0;
    while (dl < sz && dst[dl]) dl++;
    size_t sl = 0;
    while (dl + sl + 1 < sz && src[sl]) { dst[dl + sl] = src[sl]; sl++; }
    if (dl < sz) dst[dl + sl] = '\0';
    while (src[sl]) sl++;
    return dl + sl;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §7  String — search
 * ═══════════════════════════════════════════════════════════════════════ */

char *strchr(const char *s, int c) {
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (; *s; s++)
        if ((unsigned char)*s == (unsigned char)c) last = s;
    if (c == '\0') return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++)
        for (const char *a = accept; *a; a++)
            if (*s == *a) return (char *)s;
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (*s) {
        const char *a;
        for (a = accept; *a; a++) if (*s == *a) break;
        if (!*a) break;
        n++; s++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (*s) {
        const char *r;
        for (r = reject; *r; r++) if (*s == *r) return n;
        n++; s++;
    }
    return n;
}

/* strtok — NOT thread-safe. Use strtok_r in new code. */
char *strtok(char *s, const char *delim) {
    static char *saved = NULL;
    return strtok_r(s, delim, &saved);
}

char *strtok_r(char *s, const char *delim, char **saveptr) {
    if (s) *saveptr = s;
    if (!*saveptr) return NULL;
    /* skip leading delimiters */
    char *p = *saveptr;
    while (*p) {
        const char *d;
        for (d = delim; *d; d++) if (*p == *d) break;
        if (!*d) break;
        p++;
    }
    if (!*p) { *saveptr = p; return NULL; }
    char *tok = p;
    /* find end of token */
    while (*p) {
        const char *d;
        for (d = delim; *d; d++) if (*p == *d) break;
        if (*d) { *p++ = '\0'; break; }
        p++;
    }
    *saveptr = p;
    return tok;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §8  Character classification
 * ═══════════════════════════════════════════════════════════════════════ */

int isdigit (int c) { return c >= '0' && c <= '9'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isupper (int c) { return c >= 'A' && c <= 'Z'; }
int islower (int c) { return c >= 'a' && c <= 'z'; }
int isalpha (int c) { return isupper(c) || islower(c); }
int isalnum (int c) { return isalpha(c) || isdigit(c); }
int isspace (int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isprint (int c) { return c >= 0x20 && c <= 0x7e; }
int ispunct (int c) { return isprint(c) && !isalnum(c) && c != ' '; }
int iscntrl (int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
int toupper (int c) { return islower(c) ? c - ('a' - 'A') : c; }
int tolower (int c) { return isupper(c) ? c + ('a' - 'A') : c; }


/* ═══════════════════════════════════════════════════════════════════════
 *  §9  Integer conversion
 * ═══════════════════════════════════════════════════════════════════════ */

static int _skip_ws_sign(const char **sp) {
    const char *s = *sp;
    while (isspace((unsigned char)*s)) s++;
    int neg = (*s == '-');
    if (neg || *s == '+') s++;
    *sp = s;
    return neg;
}

long long strtoll(const char *s, char **endptr, int base) {
    int neg = _skip_ws_sign(&s);
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    long long v = 0;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -v : v;
}

unsigned long long strtoull(const char *s, char **endptr, int base) {
    while (isspace((unsigned char)*s)) s++;
    int neg = (*s == '-'); if (neg || *s == '+') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    unsigned long long v = 0;
    for (;;) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned long long)base + (unsigned long long)d; s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? (unsigned long long)(-(long long)v) : v;
}

long strtol(const char *s, char **endptr, int base) {
    return (long)strtoll(s, endptr, base);
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    return (unsigned long)strtoull(s, endptr, base);
}

int       atoi (const char *s) { return (int)strtol(s, NULL, 10); }
long      atol (const char *s) { return      strtol(s, NULL, 10); }
long long atoll(const char *s) { return      strtoll(s, NULL, 10); }


/* ═══════════════════════════════════════════════════════════════════════
 *  §10  Numeric helpers
 * ═══════════════════════════════════════════════════════════════════════ */

int       abs  (int x)       { return x < 0 ? -x : x; }
long      labs (long x)      { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }


/* ═══════════════════════════════════════════════════════════════════════
 *  §11  Integer-to-string helpers
 * ═══════════════════════════════════════════════════════════════════════ */

int slibc_u64_to_dec(uint64_t v, char *buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[20]; int n = 0;
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

int slibc_u64_to_hex(uint64_t v, char *buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    const char *d = "0123456789abcdef";
    char tmp[16]; int n = 0;
    while (v) { tmp[n++] = d[v & 0xF]; v >>= 4; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

int slibc_u64_to_HEX(uint64_t v, char *buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    const char *d = "0123456789ABCDEF";
    char tmp[16]; int n = 0;
    while (v) { tmp[n++] = d[v & 0xF]; v >>= 4; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §12  Formatted output
 *
 *  slibc_vprintf_cb is the core engine. All other printf variants call it.
 *  cb(ctx, c) is invoked once per output character.
 *
 *  Supported: %d %i %u %x %X %o %c %s %p %% with width/prec/flags.
 *  Not supported: %f %e %g (no floating-point in freestanding env).
 * ═══════════════════════════════════════════════════════════════════════ */

int slibc_vprintf_cb(slibc_putc_fn cb, void *ctx,
                     const char *fmt, va_list ap) {
    int total = 0;

    while (*fmt) {
        if (*fmt != '%') { cb(ctx, *fmt++); total++; continue; }
        fmt++;

        /* flags */
        int flag_left  = 0;   /* '-' : left-justify          */
        int flag_zero  = 0;   /* '0' : zero-pad              */
        int flag_plus  = 0;   /* '+' : always show sign      */
        int flag_space = 0;   /* ' ' : space before positive */
        int flag_hash  = 0;   /* '#' : alt form (0x prefix)  */
        for (;;) {
            if      (*fmt == '-') { flag_left  = 1; fmt++; }
            else if (*fmt == '0') { flag_zero  = 1; fmt++; }
            else if (*fmt == '+') { flag_plus  = 1; fmt++; }
            else if (*fmt == ' ') { flag_space = 1; fmt++; }
            else if (*fmt == '#') { flag_hash  = 1; fmt++; }
            else break;
        }

        /* width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int); fmt++;
            if (width < 0) { flag_left = 1; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }

        /* precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        /* length modifier */
        int lng = 0;  /* 0=int, 1=long, 2=long long */
        if      (*fmt == 'l') { lng = 1; fmt++; if (*fmt == 'l') { lng = 2; fmt++; } }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }
        else if (*fmt == 'z') { lng = 1; fmt++; }  /* size_t */

        char spec = *fmt++;
        if (spec == '%') { cb(ctx, '%'); total++; continue; }

        /* %c */
        if (spec == 'c') {
            char cv = (char)va_arg(ap, int);
            if (!flag_left) for (int i = 1; i < width; i++) { cb(ctx, ' '); total++; }
            cb(ctx, cv); total++;
            if (flag_left)  for (int i = 1; i < width; i++) { cb(ctx, ' '); total++; }
            continue;
        }

        /* %s */
        if (spec == 's') {
            const char *sv = va_arg(ap, const char *);
            if (!sv) sv = "(null)";
            int sl = (prec >= 0) ? (int)strnlen(sv, (size_t)prec) : (int)strlen(sv);
            if (!flag_left) for (int i = sl; i < width; i++) { cb(ctx, ' '); total++; }
            for (int i = 0; i < sl; i++) { cb(ctx, sv[i]); total++; }
            if (flag_left)  for (int i = sl; i < width; i++) { cb(ctx, ' '); total++; }
            continue;
        }

        /* numeric */
        uint64_t uval; int neg = 0;
        if (spec == 'p') {
            uval      = (uint64_t)(uintptr_t)va_arg(ap, void *);
            spec      = 'x';
            flag_hash = 1;
        } else if (spec == 'd' || spec == 'i') {
            int64_t sv = (lng == 2) ? (int64_t)va_arg(ap, long long)
                       : (lng == 1) ? (int64_t)va_arg(ap, long)
                                    : (int64_t)va_arg(ap, int);
            if (sv < 0) { neg = 1; uval = (uint64_t)(-sv); } else uval = (uint64_t)sv;
        } else {
            uval = (lng == 2) ? (uint64_t)va_arg(ap, unsigned long long)
                 : (lng == 1) ? (uint64_t)va_arg(ap, unsigned long)
                              : (uint64_t)va_arg(ap, unsigned int);
        }

        unsigned int base = (spec == 'x' || spec == 'X') ? 16u
                          : (spec == 'o') ? 8u : 10u;
        const char *digs  = (spec == 'X') ? "0123456789ABCDEF"
                                          : "0123456789abcdef";

        /* build digits in reverse */
        char tmp[24]; int tlen = 0;
        if (uval == 0) { tmp[tlen++] = '0'; }
        else { uint64_t v = uval; while (v) { tmp[tlen++] = digs[v % base]; v /= base; } }

        /* sign / prefix */
        char prefix[4]; int plen = 0;
        if (neg)             prefix[plen++] = '-';
        else if (flag_plus)  prefix[plen++] = '+';
        else if (flag_space) prefix[plen++] = ' ';
        if (flag_hash && (spec == 'x' || spec == 'X') && uval != 0)
            { prefix[plen++] = '0'; prefix[plen++] = (spec == 'X') ? 'X' : 'x'; }
        else if (flag_hash && spec == 'o' && (tlen == 0 || tmp[tlen-1] != '0'))
            prefix[plen++] = '0';

        int numw = tlen + plen;
        char pad = (!flag_left && flag_zero) ? '0' : ' ';

        if (!flag_left && pad == ' ')
            for (int i = numw; i < width; i++) { cb(ctx, ' '); total++; }
        for (int i = 0; i < plen; i++) { cb(ctx, prefix[i]); total++; }
        if (!flag_left && pad == '0')
            for (int i = numw; i < width; i++) { cb(ctx, '0'); total++; }
        for (int i = tlen - 1; i >= 0; i--) { cb(ctx, tmp[i]); total++; }
        if (flag_left)
            for (int i = numw; i < width; i++) { cb(ctx, ' '); total++; }
    }

    return total;
}

typedef struct { char *p; size_t rem; } _snbuf;

static void _sn_putc(void *ctx, char c) {
    _snbuf *b = (_snbuf *)ctx;
    if (b->rem > 1) { *b->p++ = c; b->rem--; }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    if (size == 0) return 0;
    _snbuf b = { buf, size };
    int n = slibc_vprintf_cb(_sn_putc, &b, fmt, ap);
    *b.p = '\0';
    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §13  Sorting / searching
 * ═══════════════════════════════════════════════════════════════════════ */

static void _swap_bytes(unsigned char *a, unsigned char *b, size_t sz) {
    for (size_t i = 0; i < sz; i++) {
        unsigned char t = a[i]; a[i] = b[i]; b[i] = t;
    }
}

/* Insertion sort for small sub-arrays (avoids function-call overhead) */
static void _isort(unsigned char *base, size_t nmemb, size_t sz,
                   int (*cmp)(const void *, const void *)) {
    for (size_t i = 1; i < nmemb; i++) {
        size_t j = i;
        while (j > 0 && cmp(base + (j-1)*sz, base + j*sz) > 0) {
            _swap_bytes(base + (j-1)*sz, base + j*sz, sz);
            j--;
        }
    }
}

#define QSORT_STACK 64  /* max recursion depth = 64 levels → 2^64 elements */

void qsort(void *base, size_t nmemb, size_t sz,
           int (*cmp)(const void *, const void *)) {
    if (nmemb < 2 || sz == 0) return;
    unsigned char *b = (unsigned char *)base;
    size_t lo_stk[QSORT_STACK], hi_stk[QSORT_STACK];
    int top = 0;
    lo_stk[top] = 0; hi_stk[top] = nmemb - 1; top++;
    while (top > 0) {
        top--;
        size_t lo = lo_stk[top], hi = hi_stk[top];
        if (hi <= lo) continue;
        /* use insertion sort for small arrays (faster in practice) */
        if (hi - lo < 8) { _isort(b + lo*sz, hi - lo + 1, sz, cmp); continue; }
        /* median-of-three pivot selection */
        size_t mid = lo + (hi - lo) / 2;
        if (cmp(b + lo*sz,  b + mid*sz) > 0) _swap_bytes(b + lo*sz,  b + mid*sz, sz);
        if (cmp(b + lo*sz,  b + hi*sz)  > 0) _swap_bytes(b + lo*sz,  b + hi*sz,  sz);
        if (cmp(b + mid*sz, b + hi*sz)  > 0) _swap_bytes(b + mid*sz, b + hi*sz,  sz);
        _swap_bytes(b + mid*sz, b + (hi-1)*sz, sz);
        unsigned char *pivot = b + (hi-1)*sz;
        size_t i = lo, j = hi - 1;
        for (;;) {
            while (cmp(b + (++i)*sz, pivot) < 0) {}
            while (j > lo && cmp(b + (--j)*sz, pivot) > 0) {}
            if (i >= j) break;
            _swap_bytes(b + i*sz, b + j*sz, sz);
        }
        _swap_bytes(b + i*sz, pivot, sz);
        if (top + 2 < QSORT_STACK) {
            lo_stk[top] = lo;  hi_stk[top] = i - 1; top++;
            lo_stk[top] = i+1; hi_stk[top] = hi;    top++;
        }
    }
}

void *bsearch(const void *key, const void *base,
              size_t nmemb, size_t sz,
              int (*cmp)(const void *, const void *)) {
    const unsigned char *b = (const unsigned char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, b + mid * sz);
        if (r == 0) return (void *)(b + mid * sz);
        if (r < 0) hi = mid;
        else       lo = mid + 1;
    }
    return NULL;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §14  strerror — human-readable error descriptions
 *
 *  Returns a string like "No such file or directory" instead of "-2".
 *  Both positive (POSIX) and negative (Systrix) codes are accepted.
 *
 *  CONTRIBUTOR: to add a new error code:
 *    1. Add the #define to systrix_libc.h §6 with a comment
 *    2. Add a case here with a clear, plain-English description
 *    3. Keep the cases sorted by absolute value (makes diffs easier)
 * ═══════════════════════════════════════════════════════════════════════ */

const char *strerror(int errnum) {
    if (errnum < 0) errnum = -errnum;  /* normalise negative codes */
    switch (errnum) {
    case   1: return "Operation not permitted";
    case   2: return "No such file or directory";
    case   3: return "No such process";
    case   4: return "Interrupted system call";
    case   5: return "I/O error";
    case   9: return "Bad file descriptor";
    case  10: return "No child processes";
    case  11: return "Resource temporarily unavailable (try again)";
    case  12: return "Out of memory";
    case  13: return "Permission denied";
    case  14: return "Bad address (invalid pointer)";
    case  16: return "Device or resource busy";
    case  17: return "File already exists";
    case  19: return "No such device";
    case  20: return "Not a directory";
    case  21: return "Is a directory (expected a file)";
    case  22: return "Invalid argument";
    case  23: return "File table overflow (system open file limit reached)";
    case  24: return "Too many open files (process open file limit reached)";
    case  28: return "No space left on device";
    case  30: return "Read-only file system";
    case  32: return "Broken pipe (reader closed)";
    case  34: return "Numerical result out of range";
    case  36: return "File name too long";
    case  38: return "Function not implemented";
    case  39: return "Directory not empty";
    case  75: return "Value too large for data type (overflow)";
    case  95: return "Operation not supported on this file system or device";
    case 101: return "Network is unreachable";
    case 106: return "Transport endpoint is already connected";
    case 107: return "Transport endpoint is not connected";
    case 110: return "Connection timed out";
    case 111: return "Connection refused";
    case 114: return "Operation already in progress";
    case 115: return "Operation now in progress (non-blocking)";
    default:  return "Unknown error (check errno definition in systrix_libc.h)";
    }
}

/* Alias used by the CHECK_ERR macros (accepts int) */
const char *slibc_strerror_simple(int errnum) {
    return strerror(errnum);
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §15  Integer math & bit manipulation
 * ═══════════════════════════════════════════════════════════════════════ */

uint64_t slibc_pow_u64(uint64_t base, uint32_t exp) {
    uint64_t result = 1;
    while (exp) {
        if (exp & 1) result *= base;
        base *= base; exp >>= 1;
    }
    return result;
}

int64_t slibc_pow_i64(int64_t base, uint32_t exp) {
    int64_t result = 1;
    while (exp) {
        if (exp & 1) result *= base;
        base *= base; exp >>= 1;
    }
    return result;
}

int slibc_log2_u64(uint64_t v) {
    if (v == 0) return -1;
    return 63 - __builtin_clzll(v);
}

int slibc_log10_u64(uint64_t v) {
    if (v == 0) return -1;
    int n = 0;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

uint64_t slibc_round_up_pow2(uint64_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16; v |= v >> 32;
    return v + 1;
}

uint64_t slibc_round_down_pow2(uint64_t v) {
    if (v == 0) return 0;
    return (uint64_t)1 << slibc_log2_u64(v);
}

int slibc_popcount(uint64_t v) {
    /* Portable Hamming-weight without libgcc */
    v = v - ((v >> 1) & 0x5555555555555555ULL);
    v = (v & 0x3333333333333333ULL) + ((v >> 2) & 0x3333333333333333ULL);
    v = (v + (v >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (int)((v * 0x0101010101010101ULL) >> 56);
}

int slibc_clz(uint64_t v) { return v == 0 ? 64 : __builtin_clzll(v); }
int slibc_ctz(uint64_t v) { return v == 0 ? 64 : __builtin_ctzll(v); }
int slibc_parity(uint64_t v) { return __builtin_parityll(v); }

uint64_t slibc_reverse_bits(uint64_t v) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

uint8_t slibc_reverse_byte(uint8_t v) {
    v = (uint8_t)(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
    v = (uint8_t)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
    v = (uint8_t)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
    return v;
}

uint64_t slibc_rotl64(uint64_t v, int n) { n &= 63; return (v << n) | (v >> (64 - n)); }
uint64_t slibc_rotr64(uint64_t v, int n) { n &= 63; return (v >> n) | (v << (64 - n)); }
uint32_t slibc_rotl32(uint32_t v, int n) { n &= 31; return (v << n) | (v >> (32 - n)); }
uint32_t slibc_rotr32(uint32_t v, int n) { n &= 31; return (v >> n) | (v << (32 - n)); }

uint16_t slibc_bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
uint32_t slibc_bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8)
         | ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
uint64_t slibc_bswap64(uint64_t v) {
    return ((uint64_t)slibc_bswap32((uint32_t)(v & 0xFFFFFFFFu)) << 32)
         | (uint64_t)slibc_bswap32((uint32_t)(v >> 32));
}

int slibc_add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out) {
    *out = a + b; return *out < a;
}
int slibc_mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) return 1;
    *out = a * b; return 0;
}
int slibc_add_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    *out = (int64_t)((uint64_t)a + (uint64_t)b);
    if (b > 0 && *out < a) return 1;
    if (b < 0 && *out > a) return 1;
    return 0;
}
int slibc_mul_overflow_i64(int64_t a, int64_t b, int64_t *out) {
    if (a == 0 || b == 0) { *out = 0; return 0; }
    if (a > 0 && b > 0 && a > INT64_MAX / b) return 1;
    if (a < 0 && b < 0 && a < INT64_MAX / b) return 1;
    if (a > 0 && b < 0 && b < INT64_MIN / a) return 1;
    if (a < 0 && b > 0 && a < INT64_MIN / b) return 1;
    *out = a * b; return 0;
}

uint64_t slibc_gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = b; b = a % b; a = t; }
    return a;
}
uint64_t slibc_lcm(uint64_t a, uint64_t b) {
    if (a == 0 || b == 0) return 0;
    uint64_t out;
    if (slibc_mul_overflow_u64(a / slibc_gcd(a, b), b, &out)) return 0;
    return out;
}

uint64_t slibc_sat_add_u64(uint64_t a, uint64_t b) {
    uint64_t r = a + b; return (r < a) ? UINT64_MAX : r;
}
uint64_t slibc_sat_sub_u64(uint64_t a, uint64_t b) { return (a < b) ? 0 : a - b; }
int64_t  slibc_sat_add_i64(int64_t a, int64_t b) {
    int64_t r;
    if (slibc_add_overflow_i64(a, b, &r)) return (b > 0) ? INT64_MAX : INT64_MIN;
    return r;
}
int64_t  slibc_sat_sub_i64(int64_t a, int64_t b) {
    int64_t r;
    if (slibc_add_overflow_i64(a, -b, &r)) return (b < 0) ? INT64_MAX : INT64_MIN;
    return r;
}

int64_t  slibc_div_round_up (int64_t  n, int64_t  d) { return (n + d - 1) / d; }
uint64_t slibc_udiv_round_up(uint64_t n, uint64_t d) { return (n + d - 1) / d; }


/* ═══════════════════════════════════════════════════════════════════════
 *  §16  String extras
 * ═══════════════════════════════════════════════════════════════════════ */

/* malloc forward-declarations — kernel supplies heap_malloc, user links libc */
#ifdef SYSTRIX_KERNEL
extern void *heap_malloc(size_t n);
extern void  heap_free  (void *p);
extern void *heap_realloc(void *p, size_t n);
static inline void *malloc (size_t n)           { return heap_malloc(n); }
static inline void  free   (void *p)            { heap_free(p); }
static inline void *realloc(void *p, size_t n)  { return heap_realloc(p, n); }
#else
extern void *malloc (size_t n);
extern void  free   (void *p);
extern void *realloc(void *p, size_t n);
#endif

char *slibc_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

char *slibc_strndup(const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *d = (char *)malloc(len + 1);
    if (d) { memcpy(d, s, len); d[len] = '\0'; }
    return d;
}

void slibc_strupr(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }
void slibc_strlwr(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }
void slibc_strrev(char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; i++) {
        char t = s[i]; s[i] = s[n-1-i]; s[n-1-i] = t;
    }
}

char *slibc_ltrim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}
void slibc_rtrim(char *s) {
    if (!*s) return;
    char *e = s + strlen(s) - 1;
    while (e >= s && isspace((unsigned char)*e)) *e-- = '\0';
}
void slibc_strtrim(char *s) {
    char *l = slibc_ltrim(s);
    if (l != s) memmove(s, l, strlen(l) + 1);
    slibc_rtrim(s);
}

int slibc_str_starts_with(const char *s, const char *prefix) {
    while (*prefix) if (*s++ != *prefix++) return 0;
    return 1;
}
int slibc_str_ends_with(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (xl > sl) return 0;
    return memcmp(s + sl - xl, suffix, xl) == 0;
}
int slibc_str_is_empty(const char *s)  { return !s || !*s; }
int slibc_str_is_int  (const char *s)  {
    if (!s || !*s) return 0;
    if (*s == '-' || *s == '+') s++;
    if (!*s) return 0;
    while (*s) { if (!isdigit((unsigned char)*s++)) return 0; }
    return 1;
}
int slibc_str_is_uint (const char *s) {
    if (!s || !*s) return 0;
    while (*s) { if (!isdigit((unsigned char)*s++)) return 0; }
    return 1;
}

size_t slibc_str_count(const char *haystack, const char *needle) {
    if (!*needle) return 0;
    size_t count = 0, nl = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle))) { count++; p += nl; }
    return count;
}

int slibc_str_replace(const char *src, const char *from,
                      const char *to, char *buf, size_t bufsz) {
    size_t fl = strlen(from), tl = strlen(to);
    size_t pos = 0;
    while (*src && pos + 1 < bufsz) {
        if (fl && strncmp(src, from, fl) == 0) {
            for (size_t i = 0; i < tl && pos + 1 < bufsz; i++) buf[pos++] = to[i];
            src += fl;
        } else {
            buf[pos++] = *src++;
        }
    }
    buf[pos < bufsz ? pos : bufsz - 1] = '\0';
    return (int)pos;
}

int slibc_str_split(char *src, char delim,
                    char **out_parts, int max_parts) {
    int count = 0;
    if (!src || max_parts <= 0) return 0;
    out_parts[count++] = src;
    for (char *p = src; *p && count < max_parts; p++) {
        if (*p == delim) {
            *p = '\0';
            if (count < max_parts) out_parts[count++] = p + 1;
        }
    }
    return count;
}

int slibc_str_join(const char **parts, int count,
                   char sep, char *buf, size_t bufsz) {
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0 && pos + 1 < bufsz) buf[pos++] = sep;
        for (const char *p = parts[i]; *p && pos + 1 < bufsz; ) buf[pos++] = *p++;
    }
    buf[pos < bufsz ? pos : bufsz - 1] = '\0';
    return (int)pos;
}

void slibc_hex_encode(const uint8_t *src, size_t srclen, char *out) {
    const char *h = "0123456789abcdef";
    for (size_t i = 0; i < srclen; i++) {
        out[i*2]   = h[src[i] >> 4];
        out[i*2+1] = h[src[i] & 0xF];
    }
    out[srclen*2] = '\0';
}

int slibc_hex_decode(const char *hex, uint8_t *out, size_t outlen) {
    size_t written = 0;
    while (*hex && *(hex+1) && written < outlen) {
        uint8_t hi, lo;
        char c = *hex++;
        if      (c >= '0' && c <= '9') hi = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') hi = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') hi = (uint8_t)(c - 'A' + 10);
        else return -1;
        c = *hex++;
        if      (c >= '0' && c <= '9') lo = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') lo = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') lo = (uint8_t)(c - 'A' + 10);
        else return -1;
        out[written++] = (uint8_t)((hi << 4) | lo);
    }
    return (int)written;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §17  Hashing
 * ═══════════════════════════════════════════════════════════════════════ */

uint32_t slibc_fnv1a32(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

uint64_t slibc_fnv1a64(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

uint32_t slibc_djb2(const char *s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) ^ (unsigned char)*s++;
    return h;
}

uint32_t slibc_murmur3_32(const void *data, size_t len, uint32_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = seed;
    size_t nblocks = len / 4;
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    for (size_t i = 0; i < nblocks; i++) {
        uint32_t k; memcpy(&k, p + i*4, 4);
        k *= c1; k = slibc_rotl32(k, 15); k *= c2;
        h ^= k;  h = slibc_rotl32(h, 13);
        h = h * 5 + 0xe6546b64u;
    }
    const uint8_t *tail = p + nblocks * 4;
    uint32_t k = 0;
    switch (len & 3) {
        case 3: k ^= (uint32_t)tail[2] << 16; /* fall through */
        case 2: k ^= (uint32_t)tail[1] << 8;  /* fall through */
        case 1: k ^= tail[0];
                k *= c1; k = slibc_rotl32(k, 15); k *= c2; h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16; h *= 0x85ebca6bu;
    h ^= h >> 13; h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

static uint32_t _crc32_tab[256];
static int      _crc32_ready = 0;

static void _crc32_init(void) {
    if (_crc32_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
        _crc32_tab[i] = c;
    }
    _crc32_ready = 1;
}

uint32_t slibc_crc32_update(uint32_t crc, const void *data, size_t len) {
    _crc32_init();
    const uint8_t *p = (const uint8_t *)data;
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) crc = (crc >> 8) ^ _crc32_tab[(crc ^ p[i]) & 0xFF];
    return crc ^ 0xFFFFFFFFu;
}
uint32_t slibc_crc32(const void *data, size_t len) { return slibc_crc32_update(0, data, len); }

uint32_t slibc_adler32_update(uint32_t adler, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t s1 = adler & 0xFFFF, s2 = (adler >> 16) & 0xFFFF;
    for (size_t i = 0; i < len; i++) { s1 = (s1 + p[i]) % 65521u; s2 = (s2 + s1) % 65521u; }
    return (s2 << 16) | s1;
}
uint32_t slibc_adler32(const void *data, size_t len) { return slibc_adler32_update(1, data, len); }


/* ═══════════════════════════════════════════════════════════════════════
 *  §18  PRNG — splitmix64 + xorshift64
 * ═══════════════════════════════════════════════════════════════════════ */

void slibc_rng_seed(SRng *r, uint64_t seed) {
    /* splitmix64 to spread low-entropy seeds into full state space */
    seed += 0x9e3779b97f4a7c15ULL;
    seed = (seed ^ (seed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    seed = (seed ^ (seed >> 27)) * 0x94d049bb133111ebULL;
    r->state = seed ^ (seed >> 31);
    if (!r->state) r->state = 1;  /* xorshift must not be 0 */
}

uint64_t slibc_rng_next(SRng *r) {
    uint64_t x = r->state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return (r->state = x);
}

uint32_t slibc_rng_u32   (SRng *r) { return (uint32_t)(slibc_rng_next(r) >> 32); }

uint64_t slibc_rng_range(SRng *r, uint64_t lo, uint64_t hi) {
    if (lo >= hi) return lo;
    uint64_t range = hi - lo;
    uint64_t threshold = (uint64_t)(-(int64_t)range) % range;
    uint64_t x;
    do { x = slibc_rng_next(r); } while (x < threshold);
    return lo + x % range;
}

void slibc_rng_fill(SRng *r, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    while (n >= 8) { uint64_t v = slibc_rng_next(r); memcpy(p, &v, 8); p += 8; n -= 8; }
    if (n) { uint64_t v = slibc_rng_next(r); memcpy(p, &v, n); }
}

void slibc_rng_shuffle(SRng *r, void *base, size_t nmemb, size_t sz) {
    if (nmemb < 2 || sz == 0) return;
    uint8_t *b = (uint8_t *)base;
    uint8_t tmp[256];
    for (size_t i = nmemb - 1; i > 0; i--) {
        size_t j = (size_t)slibc_rng_range(r, 0, i + 1);
        if (i != j) {
            if (sz <= 256) {
                memcpy(tmp, b + i*sz, sz); memcpy(b + i*sz, b + j*sz, sz); memcpy(b + j*sz, tmp, sz);
            } else {
                uint8_t *a_ = b + i*sz, *b_ = b + j*sz;
                for (size_t k = 0; k < sz; k++) { uint8_t t = a_[k]; a_[k] = b_[k]; b_[k] = t; }
            }
        }
    }
}

static SRng _global_rng = { 12345678901234567ULL };
void     slibc_srand     (uint64_t seed)              { slibc_rng_seed(&_global_rng, seed); }
uint64_t slibc_rand      (void)                       { return slibc_rng_next(&_global_rng); }
uint64_t slibc_rand_range(uint64_t lo, uint64_t hi)   { return slibc_rng_range(&_global_rng, lo, hi); }


/* ═══════════════════════════════════════════════════════════════════════
 *  §19  Ring buffer
 * ═══════════════════════════════════════════════════════════════════════ */

void slibc_ring_init(SRing *r, void *buf, uint32_t cap, uint32_t elem_size) {
    SLIBC_ASSERT(IS_POWER_OF_2(cap), "ring buffer capacity must be a power of 2");
    r->buf = (uint8_t *)buf; r->cap = cap; r->esize = elem_size;
    r->head = r->tail = 0;
}

int slibc_ring_push(SRing *r, const void *elem) {
    if (slibc_ring_full(r)) return -1;
    uint32_t idx = r->head & (r->cap - 1);
    memcpy(r->buf + (size_t)idx * r->esize, elem, r->esize);
    compiler_barrier();
    r->head++;
    return 0;
}

int slibc_ring_pop(SRing *r, void *elem) {
    if (slibc_ring_empty(r)) return -1;
    uint32_t idx = r->tail & (r->cap - 1);
    memcpy(elem, r->buf + (size_t)idx * r->esize, r->esize);
    compiler_barrier();
    r->tail++;
    return 0;
}

int slibc_ring_peek(const SRing *r, void *elem) {
    if (slibc_ring_empty(r)) return -1;
    uint32_t idx = r->tail & (r->cap - 1);
    memcpy(elem, r->buf + (size_t)idx * r->esize, r->esize);
    return 0;
}

void slibc_ring_clear(SRing *r) { r->head = r->tail = 0; }


/* ═══════════════════════════════════════════════════════════════════════
 *  §20  Bitmap
 * ═══════════════════════════════════════════════════════════════════════ */

void slibc_bm_init(SBitmap *bm, uint64_t *words, size_t nbits) {
    bm->words = words; bm->nbits = nbits;
    memset(words, 0, ((nbits + 63) / 64) * 8);
}
void   slibc_bm_set   (SBitmap *bm, size_t bit) { bm->words[bit/64] |=  ((uint64_t)1 << (bit%64)); }
void   slibc_bm_clear (SBitmap *bm, size_t bit) { bm->words[bit/64] &= ~((uint64_t)1 << (bit%64)); }
int    slibc_bm_test  (const SBitmap *bm, size_t bit) { return !!(bm->words[bit/64] & ((uint64_t)1 << (bit%64))); }
void   slibc_bm_toggle(SBitmap *bm, size_t bit) { bm->words[bit/64] ^=  ((uint64_t)1 << (bit%64)); }
void   slibc_bm_zero  (SBitmap *bm) { memset(bm->words, 0,    ((bm->nbits + 63) / 64) * 8); }
void   slibc_bm_fill  (SBitmap *bm) {
    size_t nw = (bm->nbits + 63) / 64;
    memset(bm->words, 0xFF, nw * 8);
    if (bm->nbits % 64) bm->words[nw-1] = ((uint64_t)1 << (bm->nbits % 64)) - 1;
}

size_t slibc_bm_first_set(const SBitmap *bm, size_t start) {
    for (size_t b = start; b < bm->nbits; ) {
        uint64_t w = bm->words[b/64] >> (b%64);
        if (w) return b + (size_t)slibc_ctz(w);
        b = (b/64 + 1) * 64;
    }
    return bm->nbits;
}

size_t slibc_bm_first_clear(const SBitmap *bm, size_t start) {
    for (size_t b = start; b < bm->nbits; ) {
        uint64_t w = ~bm->words[b/64] >> (b%64);
        if (w) {
            size_t found = b + (size_t)slibc_ctz(w);
            return found < bm->nbits ? found : bm->nbits;
        }
        b = (b/64 + 1) * 64;
    }
    return bm->nbits;
}

size_t slibc_bm_count_set(const SBitmap *bm) {
    size_t n = 0, nw = (bm->nbits + 63) / 64;
    for (size_t i = 0; i < nw; i++) n += (size_t)slibc_popcount(bm->words[i]);
    return n;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §21  Formatting helpers
 * ═══════════════════════════════════════════════════════════════════════ */

void slibc_fmt_bytes(uint64_t bytes, char *buf, size_t bufsz) {
    if (bytes < 1024) {
        snprintf(buf, bufsz, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024ULL * 1024) {
        snprintf(buf, bufsz, "%llu.%02llu KB",
                 (unsigned long long)(bytes / 1024),
                 (unsigned long long)((bytes % 1024) * 100 / 1024));
    } else if (bytes < 1024ULL * 1024 * 1024) {
        uint64_t mb = bytes / (1024 * 1024);
        uint64_t frac = (bytes % (1024ULL * 1024)) * 100 / (1024ULL * 1024);
        snprintf(buf, bufsz, "%llu.%02llu MB", (unsigned long long)mb, (unsigned long long)frac);
    } else {
        uint64_t gb = bytes / (1024ULL * 1024 * 1024);
        uint64_t frac = (bytes % (1024ULL * 1024 * 1024)) * 100 / (1024ULL * 1024 * 1024);
        snprintf(buf, bufsz, "%llu.%02llu GB", (unsigned long long)gb, (unsigned long long)frac);
    }
}

void slibc_fmt_duration_ms(uint64_t ms, char *buf, size_t bufsz) {
    if (ms < 1000) {
        snprintf(buf, bufsz, "%llums", (unsigned long long)ms);
    } else if (ms < 60000) {
        snprintf(buf, bufsz, "%llu.%03llus",
                 (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000));
    } else if (ms < 3600000) {
        snprintf(buf, bufsz, "%llum %02llus",
                 (unsigned long long)(ms / 60000), (unsigned long long)((ms % 60000) / 1000));
    } else {
        snprintf(buf, bufsz, "%lluh %02llum %02llus",
                 (unsigned long long)(ms / 3600000),
                 (unsigned long long)((ms % 3600000) / 60000),
                 (unsigned long long)((ms % 60000) / 1000));
    }
}

void slibc_fmt_zpad(uint64_t v, int width, char *buf) {
    snprintf(buf, (size_t)(width + 1), "%0*llu", width, (unsigned long long)v);
}

void slibc_fmt_ipv4(uint32_t ip_be, char *buf) {
    const uint8_t *b = (const uint8_t *)&ip_be;
    snprintf(buf, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void slibc_fmt_mac(const uint8_t mac[6], char *buf) {
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int slibc_parse_ipv4(const char *s, uint32_t *out_be) {
    unsigned int a, b, c, d;
    int n = 0;
    /* parse N.N.N.N manually */
    while (*s && isdigit((unsigned char)*s)) a = a * 10 + (*s++ - '0'), n++;
    if (*s++ != '.' || n == 0) return -1; n = 0;
    while (*s && isdigit((unsigned char)*s)) b = b * 10 + (*s++ - '0'), n++;
    if (*s++ != '.' || n == 0) return -1; n = 0;
    while (*s && isdigit((unsigned char)*s)) c = c * 10 + (*s++ - '0'), n++;
    if (*s++ != '.' || n == 0) return -1; n = 0;
    while (*s && isdigit((unsigned char)*s)) d = d * 10 + (*s++ - '0'), n++;
    if (n == 0 || a > 255 || b > 255 || c > 255 || d > 255) return -1;
    uint8_t *o = (uint8_t *)out_be;
    o[0] = (uint8_t)a; o[1] = (uint8_t)b; o[2] = (uint8_t)c; o[3] = (uint8_t)d;
    return 0;
}

void slibc_fmt_progress(char *buf, size_t total_width,
                        uint64_t numerator, uint64_t denominator) {
    if (!buf || total_width == 0) return;
    if (denominator == 0) denominator = 1;
    if (numerator > denominator) numerator = denominator;
    size_t filled = (size_t)((numerator * total_width) / denominator);
    if (filled > total_width) filled = total_width;
    buf[0] = '[';
    for (size_t i = 0; i < total_width; i++)
        buf[1 + i] = (i < filled) ? ((i + 1 == filled && filled < total_width) ? '>' : '=') : ' ';
    buf[1 + total_width] = ']';
    buf[2 + total_width] = '\0';
}

size_t slibc_str_repeat(char *buf, char c, size_t count) {
    for (size_t i = 0; i < count; i++) buf[i] = c;
    buf[count] = '\0';
    return count;
}

size_t slibc_str_pad(char *dst, size_t dst_sz,
                     const char *str, size_t width,
                     char pad_char, int left_align) {
    if (!dst || dst_sz == 0) return 0;
    size_t slen = strnlen(str ? str : "", dst_sz);
    if (slen > width) slen = width;
    size_t pad = (width > slen) ? width - slen : 0;
    size_t total = slen + pad;
    if (total + 1 > dst_sz) {
        total = dst_sz - 1;
        if (total >= slen) pad = total - slen;
        else { slen = total; pad = 0; }
    }
    char *p = dst;
    if (!left_align) {
        for (size_t i = 0; i < pad; i++) *p++ = pad_char;
        for (size_t i = 0; i < slen; i++) *p++ = str[i];
    } else {
        for (size_t i = 0; i < slen; i++) *p++ = str[i];
        for (size_t i = 0; i < pad; i++) *p++ = pad_char;
    }
    *p = '\0';
    return (size_t)(p - dst);
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §22  Dynamic string builder (SStrBuf)
 * ═══════════════════════════════════════════════════════════════════════ */

#define SSBUF_INIT_CAP 64

void slibc_sb_init(SStrBuf *sb) { sb->data = NULL; sb->len = sb->cap = 0; }

void slibc_sb_free(SStrBuf *sb) {
    if (sb->data) free(sb->data);
    sb->data = NULL; sb->len = sb->cap = 0;
}

void slibc_sb_reset(SStrBuf *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static int _sb_grow(SStrBuf *sb, size_t need) {
    if (sb->cap >= need) return 0;
    size_t ncap = sb->cap ? sb->cap : SSBUF_INIT_CAP;
    while (ncap < need) ncap *= 2;
    char *nd = (char *)realloc(sb->data, ncap);
    if (!nd) return -1;
    sb->data = nd; sb->cap = ncap;
    return 0;
}

int slibc_sb_appendn(SStrBuf *sb, const char *s, size_t n) {
    if (_sb_grow(sb, sb->len + n + 1) < 0) return -1;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n; sb->data[sb->len] = '\0';
    return 0;
}
int slibc_sb_append (SStrBuf *sb, const char *s) { return slibc_sb_appendn(sb, s, strlen(s)); }
int slibc_sb_appendc(SStrBuf *sb, char c)        { return slibc_sb_appendn(sb, &c, 1); }

int slibc_sb_appendf(SStrBuf *sb, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(tmp)) return slibc_sb_appendn(sb, tmp, (size_t)n);
    /* output was truncated — allocate exact size and retry */
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = slibc_sb_appendn(sb, big, (size_t)n);
    free(big);
    return r;
}

char *slibc_sb_steal(SStrBuf *sb) {
    char *p = sb->data;
    if (!p) {
        p = (char *)malloc(1);
        if (p) p[0] = '\0';
    }
    sb->data = NULL; sb->len = sb->cap = 0;
    return p;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §23  Path utilities
 * ═══════════════════════════════════════════════════════════════════════ */

int slibc_path_join(char *dst, size_t dst_sz,
                    const char *base, const char *name) {
    if (!dst || dst_sz == 0) return -1;
    dst[0] = '\0';
    if (name && name[0] == '/') {
        size_t n = strlcpy(dst, name, dst_sz);
        return (n < dst_sz) ? 0 : -1;
    }
    size_t blen = base ? strlcpy(dst, base, dst_sz) : 0;
    if (blen >= dst_sz) return -1;
    if (name && name[0]) {
        if (blen > 0 && dst[blen - 1] != '/') {
            if (blen + 1 >= dst_sz) return -1;
            dst[blen++] = '/'; dst[blen] = '\0';
        }
        size_t nlen = strlcpy(dst + blen, name, dst_sz - blen);
        if (nlen >= dst_sz - blen) return -1;
    }
    return 0;
}

char *slibc_path_normalize(char *path) {
    if (!path || !path[0]) return path;
    int abs = (path[0] == '/');
    char *src = path, *dst = path;
    while (*src) {
        if (*src == '/') {
            if (dst == path || *(dst - 1) != '/') *dst++ = '/';
            src++; continue;
        }
        if (src[0] == '.') {
            if (src[1] == '/' || src[1] == '\0') {
                src += (src[1] == '/') ? 2 : 1; continue;
            }
            if (src[1] == '.' && (src[2] == '/' || src[2] == '\0')) {
                src += (src[2] == '/') ? 3 : 2;
                if (dst > path + abs) dst--;
                while (dst > path + abs && *(dst - 1) != '/') dst--;
                if (abs && dst == path) { *dst++ = '/'; }
                continue;
            }
        }
        while (*src && *src != '/') *dst++ = *src++;
    }
    if (dst > path + abs && *(dst - 1) == '/') dst--;
    *dst = '\0';
    if (dst == path) { path[0] = abs ? '/' : '.'; path[1] = '\0'; }
    return path;
}

const char *slibc_path_basename(const char *path) {
    if (!path || !path[0]) return ".";
    const char *end = path + strlen(path) - 1;
    while (end > path && *end == '/') end--;
    if (end == path && *path == '/') return "/";
    const char *p = end;
    while (p > path && *(p - 1) != '/') p--;
    return p;
}

int slibc_path_dirname(const char *path, char *dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) return -1;
    if (!path || !path[0]) { strlcpy(dst, ".", dst_sz); return 0; }
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    size_t last = len;
    while (last > 0 && path[last - 1] != '/') last--;
    if (last == 0) { strlcpy(dst, ".", dst_sz); return 0; }
    if (last > 1) last--;
    size_t n = strlcpy(dst, path, (last + 1 < dst_sz) ? last + 1 : dst_sz);
    if (last < dst_sz) dst[last] = '\0';
    return (n <= last) ? 0 : -1;
}

int slibc_path_has_ext(const char *path, const char *ext) {
    if (!path || !ext) return 0;
    const char *dot = NULL;
    for (const char *p = path; *p; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    return strcasecmp(dot + 1, ext) == 0;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §24  UUID v4 generation
 *
 *  Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 *  where x = random hex, y ∈ {8, 9, a, b}
 * ═══════════════════════════════════════════════════════════════════════ */

void slibc_uuid_v4(SRng *rng, char *buf) {
    uint8_t b[16];
    slibc_rng_fill(rng, b, 16);
    b[6] = (b[6] & 0x0F) | 0x40;   /* version 4 */
    b[8] = (b[8] & 0x3F) | 0x80;   /* variant 1 (RFC 4122) */
    snprintf(buf, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
        b[10],b[11],b[12],b[13],b[14],b[15]);
}
