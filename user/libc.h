/* ================================================================
 *  Systrix OS — user/libc.h
 *  Basic C library for user programs running on Systrix OS.
 *
 *  Usage:
 *    #include "libc.h"
 *
 *  Link with libc.o and crt0.o:
 *    ld -m elf_x86_64 -static -nostdlib -Ttext=0x400000 \
 *       -o MYPROG crt0.o libc.o myprog.o
 * ================================================================ */

#pragma once

/* ── Basic types ─────────────────────────────────────────────── */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
typedef unsigned long long  size_t;
typedef unsigned long long  uintptr_t;
typedef signed long long    ssize_t;
typedef signed long long    off_t;
typedef int                 pid_t;
typedef unsigned int        socklen_t;

#define NULL    ((void*)0)
#define EOF     (-1)
#define true    1
#define false   0

/* ── Limits ──────────────────────────────────────────────────── */
#define INT_MAX   0x7fffffff
#define INT_MIN   (-INT_MAX - 1)
#define UINT_MAX  0xffffffffU
#define LONG_MAX  0x7fffffffffffffffLL
#define SIZE_MAX  0xffffffffffffffffULL

/* ── File descriptors ────────────────────────────────────────── */
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

/* ── Error codes (errno-compatible subset) ───────────────────── */
#define EPERM    1
#define ENOENT   2
#define EINTR    4
#define EIO      5
#define ENOMEM   12
#define EACCES   13
#define EBUSY    16
#define EINVAL   22
#define ENOSYS   38

extern int errno;

/* ── Syscall wrappers (low level) ────────────────────────────── */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     open(const char *path, int flags);
int     close(int fd);
void    exit(int code) __attribute__((noreturn));
void   *malloc(size_t n);
void    free(void *ptr);
void   *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int     munmap(void *addr, size_t len);
void   *sbrk(ssize_t increment);
pid_t   getpid(void);
pid_t   fork(void);
int     execve(const char *path, char **argv, char **envp);
int     waitpid(pid_t pid, int *wstatus, int options);
int     pipe(int pipefd[2]);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     kill(pid_t pid, int sig);
void   *signal(int signum, void *handler);
pid_t   wait(int *wstatus);
pid_t   clone(int (*fn)(void*), void *stack, int flags, void *arg);
int     socket(int domain, int type, int protocol);
int     bind(int sockfd, const void *addr, socklen_t addrlen);
int     connect(int sockfd, const void *addr, socklen_t addrlen);
int     listen(int sockfd, int backlog);
int     accept(int sockfd, void *addr, socklen_t *addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
int     shutdown(int sockfd, int how);

/* ── String functions ────────────────────────────────────────── */
size_t  strlen(const char *s);
int     strcmp(const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy(char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strcat(char *dst, const char *src);
char   *strncat(char *dst, const char *src, size_t n);
char   *strchr(const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr(const char *haystack, const char *needle);
char   *strdup(const char *s);

/* ── Memory functions ────────────────────────────────────────── */
void   *memcpy(void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset(void *dst, int c, size_t n);
int     memcmp(const void *a, const void *b, size_t n);
void   *memchr(const void *s, int c, size_t n);

/* ── Character classification ────────────────────────────────── */
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int toupper(int c);
int tolower(int c);

/* ── Number conversion ───────────────────────────────────────── */
int          atoi(const char *s);
long         atol(const char *s);
long long    atoll(const char *s);
long         strtol(const char *s, char **endptr, int base);
long long    strtoll(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);

/* ── Formatted output ────────────────────────────────────────── */
/*
 * printf / fprintf / sprintf / snprintf
 *
 * Supported format specifiers:
 *   %d  %i  — signed decimal int
 *   %u      — unsigned decimal
 *   %x  %X  — hex (lower / upper)
 *   %o      — octal
 *   %c      — character
 *   %s      — string
 *   %p      — pointer (hex with 0x prefix)
 *   %ld %lu %lx %lld %llu %llx  — long / long long variants
 *   %%      — literal %
 *
 * Width and precision (e.g. %5d, %-10s, %08x) are supported.
 * %f / %e / %g are NOT supported (no FPU in kernel mode anyway).
 */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* va_list support (compiler built-ins) */
typedef __builtin_va_list va_list;
#define va_start(v,l)  __builtin_va_start(v,l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v,l)    __builtin_va_arg(v,l)

int vprintf(const char *fmt, va_list ap);
int vfprintf(int fd, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* ── Simple I/O helpers ──────────────────────────────────────── */
int     putchar(int c);
int     puts(const char *s);
int     getchar(void);
char   *gets_s(char *buf, size_t n);   /* safe line read, replaces gets() */

/* ── Utility ─────────────────────────────────────────────────── */
void   *calloc(size_t nmemb, size_t size);
void   *realloc(void *ptr, size_t size);
void    abort(void) __attribute__((noreturn));
int     abs(int x);
long    labs(long x);

/* ── FILE* stdio layer ───────────────────────────────────────── */
/*
 * Thin wrapper over raw fd syscalls.  Each FILE holds an fd, a
 * small r/w buffer, and flags.  Compatible with the subset of
 * stdio that DOOM actually calls.
 */
#define FOPEN_MAX   16
#define BUFSIZ      4096

typedef struct _FILE {
    int     fd;          /* underlying file descriptor, -1 = closed */
    int     flags;       /* _FILE_READ | _FILE_WRITE | _FILE_ERR | _FILE_EOF */
    /* write buffer */
    unsigned char wbuf[BUFSIZ];
    int     wpos;
    /* read buffer */
    unsigned char rbuf[BUFSIZ];
    int     rpos;
    int     rlen;
} FILE;

#define _FILE_READ   (1 << 0)
#define _FILE_WRITE  (1 << 1)
#define _FILE_ERR    (1 << 2)
#define _FILE_EOF    (1 << 3)
#define _FILE_STRM   (1 << 4)   /* stream (unbuffered stdout/stderr) */

/* The three standard streams — defined in libc.c */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* Open/close */
FILE   *fopen(const char *path, const char *mode);
int     fclose(FILE *f);
int     fflush(FILE *f);

/* Read */
size_t  fread(void *ptr, size_t size, size_t nmemb, FILE *f);
int     fgetc(FILE *f);
char   *fgets(char *buf, int n, FILE *f);
#define getc(f)  fgetc(f)

/* Write */
size_t  fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int     fputc(int c, FILE *f);
int     fputs(const char *s, FILE *f);
int     fprint(FILE *f, const char *fmt, ...) __attribute__((format(printf,2,3)));
int     vfprint(FILE *f, const char *fmt, __builtin_va_list ap);
#define putc(c,f) fputc(c,f)

/* Seek / tell */
int     fseek(FILE *f, long offset, int whence);
long    ftell(FILE *f);
void    rewind(FILE *f);

/* State */
int     feof(FILE *f);
int     ferror(FILE *f);
void    clearerr(FILE *f);

/* Rename / remove (thin wrappers over syscalls) */
int     remove(const char *path);
int     rename_file(const char *old, const char *nw);  /* rename() clashes with syscall name */

/* Seek constants */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* ── Timer — millisecond precision ──────────────────────────── */
/*
 * gettime_ms() — milliseconds since kernel boot.
 * PIT is programmed at 1000 Hz so pit_ticks == milliseconds.
 * Suitable for frame timing, timeouts, DOOM's ticcount.
 */
static inline long long gettime_ms(void) {
    long long r;
    __asm__ volatile("syscall"
        : "=a"(r) : "0"(327LL) : "rcx","r11","memory");
    return r;
}

/* ── Mouse relative / grabbed mode ──────────────────────────── */
/*
 * mouse_setrelative(1) — grab mouse for FPS mouse-look.
 *   Hides the GUI cursor; poll_mouse() returns raw deltas with no
 *   cursor clamping.  The cursor won't drift to a screen edge.
 * mouse_setrelative(0) — release; restore GUI cursor mode.
 */
static inline void mouse_setrelative(int on) {
    __asm__ volatile("syscall"
        : : "a"(328LL), "D"((long long)on) : "rcx","r11","memory");
}

static inline void watchdog_pet(void) {
    __asm__ volatile("syscall"
        : : "a"(353LL) : "rcx","r11","memory");
}

/* ── setjmp / longjmp ────────────────────────────────────────── */
/*
 * jmp_buf saves the 6 callee-saved registers + rsp + rip (return
 * address) needed to restore execution context.
 * Layout (8 bytes each, 8 slots = 64 bytes):
 *   [0] rbx  [1] rbp  [2] r12  [3] r13  [4] r14  [5] r15
 *   [6] rsp  [7] rip  (rip = return address from setjmp call site)
 */
typedef unsigned long long jmp_buf[8];

/*
 * setjmp(env) — save CPU state into env.
 * Returns 0 when called directly.
 * Returns the nonzero val passed to longjmp() when jumped back to.
 */
static inline int setjmp(jmp_buf env) {
    int r;
    __asm__ volatile(
        "mov  %%rbx,    (%1)\n\t"
        "mov  %%rbp,   8(%1)\n\t"
        "mov  %%r12,  16(%1)\n\t"
        "mov  %%r13,  24(%1)\n\t"
        "mov  %%r14,  32(%1)\n\t"
        "mov  %%r15,  40(%1)\n\t"
        "lea  8(%%rsp), %%rax\n\t"   /* rsp at call site (before ret addr) */
        "mov  %%rax,  48(%1)\n\t"
        "mov  (%%rsp), %%rax\n\t"    /* return address = rip at call site  */
        "mov  %%rax,  56(%1)\n\t"
        "xor  %0, %0\n\t"            /* return 0 */
        : "=r"(r)
        : "r"(env)
        : "rax", "memory"
    );
    return r;
}

/*
 * longjmp(env, val) — restore CPU state from env and jump back.
 * val is returned by the corresponding setjmp(); val=0 becomes 1.
 * Does not return.
 */
static inline void longjmp(jmp_buf env, int val) __attribute__((noreturn));
static inline void longjmp(jmp_buf env, int val) {
    if (val == 0) val = 1;
    __asm__ volatile(
        "mov    (%0), %%rbx\n\t"
        "mov   8(%0), %%rbp\n\t"
        "mov  16(%0), %%r12\n\t"
        "mov  24(%0), %%r13\n\t"
        "mov  32(%0), %%r14\n\t"
        "mov  40(%0), %%r15\n\t"
        "mov  48(%0), %%rsp\n\t"
        "mov  56(%0), %%rax\n\t"   /* saved rip (return address) */
        "mov  %1,     %%edi\n\t"   /* val → first return register */
        "jmp  *%%rax\n\t"
        :
        : "r"(env), "r"(val)
        : /* everything is clobbered — doesn't matter, noreturn */
    );
    __builtin_unreachable();
}

/* ── Phase 1: Input & Controls ───────────────────────────────── */

/* KeyEvent — returned by poll_keys() */
typedef struct {
    unsigned char  scancode;  /* raw PS/2 make-code                  */
    unsigned char  ascii;     /* translated ASCII, 0 if non-printable */
    unsigned char  mods;      /* INPUT_MOD_* bitmask                 */
    unsigned char  _pad;
} KeyEvent;

#define INPUT_MOD_SHIFT  (1 << 0)
#define INPUT_MOD_CTRL   (1 << 1)
#define INPUT_MOD_ALT    (1 << 2)
#define INPUT_MOD_CAPS   (1 << 3)

/* MouseEvent — returned by poll_mouse() */
typedef struct {
    long long dx;         /* relative X (positive = right)  */
    long long dy;         /* relative Y (positive = up)     */
    unsigned char buttons; /* INPUT_BTN_* bitmask            */
    unsigned char _pad[7];
} MouseEvent;

#define INPUT_BTN_LEFT   (1 << 0)
#define INPUT_BTN_RIGHT  (1 << 1)
#define INPUT_BTN_MIDDLE (1 << 2)

/* PadState — returned by poll_pad() */
typedef struct {
    long long axis_x;      /* -128 .. 127                    */
    long long axis_y;
    unsigned short buttons; /* bitmask of up to 16 buttons   */
    unsigned char  connected;
    unsigned char  _pad[5];
} PadState;

/* poll_keys(buf, max) — drain up to max KeyEvents; returns count */
static inline long poll_keys(KeyEvent *buf, unsigned long max) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(300UL), "D"((unsigned long)buf), "S"(max)
        : "rcx","r11","memory");
    return r;
}

/* poll_mouse(buf, max) — drain up to max MouseEvents; returns count */
static inline long poll_mouse(MouseEvent *buf, unsigned long max) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(301UL), "D"((unsigned long)buf), "S"(max)
        : "rcx","r11","memory");
    return r;
}

/* poll_pad(buf) — snapshot current pad state; returns 1 if connected */
static inline long poll_pad(PadState *buf) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(302UL), "D"((unsigned long)buf)
        : "rcx","r11","memory");
    return r;
}

/* ── environ / getenv / setenv ───────────────────────────────── */
#define ENV_MAX     128
#define ENV_KEY_MAX  64
#define ENV_VAL_MAX 256

typedef struct { char key[ENV_KEY_MAX]; char val[ENV_VAL_MAX]; } _EnvEntry;

static _EnvEntry   _env_table[ENV_MAX];
static int         _env_count = 0;
/* POSIX-compatible environ pointer array (NULL-terminated, "KEY=VAL") */
static char       *_environ_ptrs[ENV_MAX + 1];
static char        _environ_bufs[ENV_MAX][ENV_KEY_MAX + ENV_VAL_MAX + 2];
extern char      **environ;   /* defined in libc.c */

static inline void _env_rebuild(void) {
    for (int i = 0; i < _env_count; i++) {
        char *b = _environ_bufs[i];
        int  ki = 0, vi = 0, bi = 0;
        while (_env_table[i].key[ki]) { b[bi++] = _env_table[i].key[ki++]; }
        b[bi++] = '=';
        while (_env_table[i].val[vi]) { b[bi++] = _env_table[i].val[vi++]; }
        b[bi] = '\0';
        _environ_ptrs[i] = b;
    }
    _environ_ptrs[_env_count] = (char*)0;
}

static inline char *getenv(const char *name) {
    if (!name) return (char*)0;
    for (int i = 0; i < _env_count; i++) {
        const char *a = name, *b = _env_table[i].key;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return _env_table[i].val;
    }
    return (char*)0;
}

static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!name || name[0] == '\0') return -1;
    for (int i = 0; i < _env_count; i++) {
        const char *a = name, *b = _env_table[i].key;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            if (!overwrite) return 0;
            int vi = 0;
            while (value && value[vi] && vi < ENV_VAL_MAX - 1)
                { _env_table[i].val[vi] = value[vi]; vi++; }
            _env_table[i].val[vi] = '\0';
            _env_rebuild();
            return 0;
        }
    }
    if (_env_count >= ENV_MAX) return -1;
    int ki = 0;
    while (name[ki] && ki < ENV_KEY_MAX - 1)
        { _env_table[_env_count].key[ki] = name[ki]; ki++; }
    _env_table[_env_count].key[ki] = '\0';
    int vi = 0;
    while (value && value[vi] && vi < ENV_VAL_MAX - 1)
        { _env_table[_env_count].val[vi] = value[vi]; vi++; }
    _env_table[_env_count].val[vi] = '\0';
    _env_count++;
    _env_rebuild();
    return 0;
}

static inline int unsetenv(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < _env_count; i++) {
        const char *a = name, *b = _env_table[i].key;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') {
            _env_table[i] = _env_table[_env_count - 1];
            _env_count--;
            _env_rebuild();
            return 0;
        }
    }
    return 0;
}

static inline int putenv(char *str) {
    if (!str) return -1;
    char key[ENV_KEY_MAX]; int ki = 0;
    while (str[ki] && str[ki] != '=' && ki < ENV_KEY_MAX - 1)
        { key[ki] = str[ki]; ki++; }
    key[ki] = '\0';
    const char *val = (str[ki] == '=') ? str + ki + 1 : "";
    return setenv(key, val, 1);
}

static inline int clearenv(void) {
    _env_count = 0;
    _environ_ptrs[0] = (char*)0;
    return 0;
}

/* defined in libc.c — called from crt0.S before main() */
void env_init_defaults(void);

/* ── Locale ────────────────────────────────────────────────────── */
#define LC_ALL       0
#define LC_COLLATE   1
#define LC_CTYPE     2
#define LC_MONETARY  3
#define LC_NUMERIC   4
#define LC_TIME      5
#define LC_MESSAGES  6

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char  int_frac_digits;
    char  frac_digits;
    char  p_cs_precedes;
    char  p_sep_by_space;
    char  n_cs_precedes;
    char  n_sep_by_space;
    char  p_sign_posn;
    char  n_sign_posn;
    char  int_p_cs_precedes;
    char  int_n_cs_precedes;
    char  int_p_sep_by_space;
    char  int_n_sep_by_space;
    char  int_p_sign_posn;
    char  int_n_sign_posn;
};

char        *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

/* ================================================================
 *  POSIX types and structs for Lynx / browser support
 * ================================================================ */

/* stat */
struct stat {
    unsigned long  st_dev;
    unsigned long  st_ino;
    unsigned int   st_mode;
    unsigned int   st_nlink;
    unsigned int   st_uid;
    unsigned int   st_gid;
    unsigned long  st_rdev;
    long           st_size;
    long           st_blksize;
    long           st_blocks;
    long           st_atime;
    long           st_mtime;
    long           st_ctime;
    unsigned char  _pad[24];
};

/* timespec */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

/* timeval */
struct timeval {
    long tv_sec;
    long tv_usec;
};

/* sigset */
typedef unsigned long sigset_t;
#define SIG_DFL ((void*)0)
#define SIG_IGN ((void*)1)
#define SIG_ERR ((void*)-1)

#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGWINCH 28

/* sigaction */
struct sigaction {
    void    (*sa_handler)(int);
    unsigned long sa_flags;
    void    (*sa_restorer)(void);
    sigset_t sa_mask;
};

/* rlimit */
struct rlimit {
    unsigned long rlim_cur;
    unsigned long rlim_max;
};

#define RLIMIT_NOFILE   7
#define RLIMIT_STACK    3
#define RLIMIT_AS      9

/* tms / times() */
typedef long clock_t;
struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

/* rusage */
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss;
    long ru_minflt, ru_majflt, ru_nswap, ru_inblock;
    long ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals;
    long ru_nvcsw, ru_nivcsw;
};

/* utsname */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* sysinfo */
struct sysinfo {
    long  uptime;
    unsigned long loads[3];
    unsigned long totalram, freeram, sharedram, bufferram;
    unsigned long totalswap, freeswap;
    unsigned short procs;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;
    unsigned char _pad[20];
};

/* termios */
struct termios {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_cc[19];
    unsigned char _pad;
    unsigned int c_ispeed, c_ospeed;
};

/* dirent + DIR */
struct dirent {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[256];
};

typedef struct {
    int fd;
    int buf_pos;
    int buf_len;
    unsigned char buf[2048];
} DIR;

/* inet */
struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;
    unsigned int   sin_addr;
    unsigned char  sin_zero[8];
};

#define AF_INET   2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

/* F_* for fcntl */
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 0x800
#define FD_CLOEXEC 1

/* mode bits */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)

/* access() mode bits */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

/* Function declarations */
int          nanosleep(const struct timespec *req, struct timespec *rem);
unsigned int sleep(unsigned int seconds);
int          usleep(unsigned int us);
int          chdir(const char *path);
char        *getcwd(char *buf, size_t size);
int          access(const char *path, int mode);
ssize_t      readlink(const char *path, char *buf, size_t bufsz);
int          lstat(const char *path, struct stat *buf);
int          stat(const char *path, struct stat *buf);
int          fstat(int fd, struct stat *buf);
int          ioctl(int fd, unsigned long req, void *arg);
int          sysinfo(struct sysinfo *info);
pid_t        getppid(void);
int          sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int          sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int          sigemptyset(sigset_t *set);
int          sigfillset(sigset_t *set);
int          sigaddset(sigset_t *set, int signum);
int          sigdelset(sigset_t *set, int signum);
int          sigismember(const sigset_t *set, int signum);
int          getrlimit(int resource, struct rlimit *rlim);
int          setrlimit(int resource, const struct rlimit *rlim);
clock_t      times(struct tms *buf);
pid_t        gettid(void);
int          getrusage(int who, struct rusage *usage);
int          fcntl(int fd, int cmd, long arg);
int          uname(struct utsname *buf);
int          isatty(int fd);
int          link(const char *oldpath, const char *newpath);
int          symlink(const char *target, const char *linkpath);
int          unlinkat(int dirfd, const char *path, int flags);
int          mkdir(const char *path, int mode);
int          rmdir(const char *path);
int          rename(const char *old, const char *newp);
DIR         *opendir(const char *path);
struct dirent *readdir(DIR *d);
int          closedir(DIR *d);
unsigned int  inet_addr(const char *cp);
unsigned short htons(unsigned short v);
unsigned short ntohs(unsigned short v);
unsigned int   htonl(unsigned int v);
unsigned int   ntohl(unsigned int v);
