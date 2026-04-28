/* ═══════════════════════════════════════════════════════════════════════
 *  SystrixOS — libc/systrix_libc.h
 *  The Systrix C library — works in BOTH kernel and user space.
 *
 *  ┌─────────────────────────────────────────────────────────────────┐
 *  │  CONTRIBUTOR QUICK-START                                        │
 *  │                                                                 │
 *  │  1. This header declares everything.                            │
 *  │     The matching .c file implements everything.                 │
 *  │                                                                 │
 *  │  2. Adding a new function?                                      │
 *  │     • Declare it here with a /// doc-comment above it.          │
 *  │     • Implement it in systrix_libc.c under the right section.   │
 *  │     • Keep the section order the same in both files.            │
 *  │                                                                 │
 *  │  3. Build constraints (DO NOT break these):                     │
 *  │     - No #include of external headers here.                     │
 *  │     - No floating-point anywhere (freestanding env).            │
 *  │     - Compiles with: -ffreestanding -nostdlib -nostdinc         │
 *  │     - Compiles with: plain gcc (for user programs).             │
 *  │                                                                 │
 *  │  4. Debug output:                                               │
 *  │     Use SLIBC_ASSERT(cond, "human message") instead of assert.  │
 *  │     Use SLIBC_PANIC("reason") to halt with a readable message.  │
 *  │     Use SLIBC_DBG("fmt", ...) for debug prints (strips in prod).│
 *  │     Use SLIBC_TODO("note") to mark unfinished code loudly.      │
 *  │                                                                 │
 *  │  5. Error handling:                                             │
 *  │     Error codes are NEGATIVE numbers (e.g. -EINVAL).            │
 *  │     SLIBC_ERR_STR(code) → "Invalid argument" (not a number).   │
 *  │     SLIBC_CHECK_ERR(ret, "context") → prints & returns on fail. │
 *  └─────────────────────────────────────────────────────────────────┘
 *
 *  Sections in this file (search for the ═══ banner to jump):
 *    1.  Compiler / VA-list
 *    2.  Basic types  (u8, u16, u32, u64, size_t, …)
 *    3.  Constants  (NULL, EOF, true, false, limits)
 *    4.  Utility macros  (MIN, MAX, ALIGN_UP, BIT, …)
 *    5.  ★ Debug & Assert macros  ← new, read this!
 *    6.  Error codes  (EPERM, ENOENT, EINVAL, …)
 *    7.  Error helpers  (SLIBC_ERR_STR, SLIBC_CHECK_ERR, …)
 *    8.  Memory  (memset, memcpy, memmove, memcmp, memchr)
 *    9.  String  (strlen, strcpy, strcmp, strtok, …)
 *   10.  Character classification  (isdigit, toupper, …)
 *   11.  Integer conversion  (atoi, strtol, …)
 *   12.  Numeric helpers  (abs, labs, llabs)
 *   13.  Formatted output  (snprintf, sprintf, vsnprintf)
 *   14.  Integer-to-string helpers  (slibc_u64_to_dec, …)
 *   15.  Sorting / searching  (qsort, bsearch)
 *   16.  setjmp / longjmp
 *   17.  Integer math & bit operations
 *   18.  String extras  (strdup, trim, split, join, hex, …)
 *   19.  Hashing  (FNV-1a, MurmurHash3, CRC-32, Adler-32)
 *   20.  PRNG  (xorshift64 + splitmix64)
 *   21.  Ring buffer  (lock-free SPSC)
 *   22.  Bitmap
 *   23.  Formatting helpers  (bytes, duration, IP, MAC)
 *   24.  Intrusive linked list
 *   25.  Dynamic string builder  (SStrBuf)
 *   26.  Path utilities
 *   27.  UUID
 * ═══════════════════════════════════════════════════════════════════════ */

#pragma once

/* ═══════════════════════════════════════════════════════════════════════
 *  §1  Compiler built-ins — va_list
 * ═══════════════════════════════════════════════════════════════════════ */

typedef __builtin_va_list va_list;
#define va_start(v,l)  __builtin_va_start(v,l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v,l)    __builtin_va_arg(v,l)


/* ═══════════════════════════════════════════════════════════════════════
 *  §2  Basic types
 *
 *  Kernel-friendly short aliases (u8, u16, u32, u64) and POSIX-style
 *  names (uint8_t … uint64_t) are BOTH defined here so code that only
 *  includes this header compiles in either environment.
 * ═══════════════════════════════════════════════════════════════════════ */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed   char       int8_t;
typedef signed   short      int16_t;
typedef signed   int        int32_t;
typedef signed   long long  int64_t;
typedef unsigned long long  size_t;
typedef signed   long long  ssize_t;
typedef unsigned long long  uintptr_t;
typedef signed   long long  intptr_t;
typedef signed   long long  ptrdiff_t;

#ifndef _SYSTRIX_KERNEL_TYPES_DEFINED
#define _SYSTRIX_KERNEL_TYPES_DEFINED
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int64_t   i64;
typedef size_t    usize;
#endif


/* ═══════════════════════════════════════════════════════════════════════
 *  §3  Constants
 * ═══════════════════════════════════════════════════════════════════════ */

#define NULL    ((void*)0)
#define EOF     (-1)
#define true    1
#define false   0

/* Integer limits */
#define INT8_MIN    (-128)
#define INT8_MAX    (127)
#define UINT8_MAX   (255U)
#define INT16_MIN   (-32768)
#define INT16_MAX   (32767)
#define UINT16_MAX  (65535U)
#define INT32_MIN   (-2147483648)
#define INT32_MAX   (2147483647)
#define UINT32_MAX  (4294967295U)
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT64_MAX   (9223372036854775807LL)
#define UINT64_MAX  (18446744073709551615ULL)
#define INT_MIN     INT32_MIN
#define INT_MAX     INT32_MAX
#define UINT_MAX    UINT32_MAX
#define LONG_MAX    INT64_MAX
#define LONG_MIN    INT64_MIN
#define SIZE_MAX    UINT64_MAX

/* Seek constants (used by FILE* layer in user/libc.h) */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2


/* ═══════════════════════════════════════════════════════════════════════
 *  §4  Utility macros
 *
 *  These are the everyday helpers. All are side-effect-safe unless the
 *  name has a note saying otherwise.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Alignment (v and a must be integers; a must be a power of 2) */
#define ALIGN_UP(v, a)     (((v) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(v, a)   ((v) & ~((a) - 1))

/* Bit manipulation */
#define BIT(n)             (1UL << (n))          /* single bit mask */
#define IS_POWER_OF_2(n)   ((n) && !((n) & ((n) - 1)))

/* Suppress "unused variable" warnings cleanly */
#define UNUSED(x)          ((void)(x))

/* Number of elements in a stack-allocated array */
#define ARRAY_SIZE(a)      (sizeof(a) / sizeof((a)[0]))

/* Min / max / clamp — WARNING: arguments are evaluated twice */
#define MIN(a, b)          ((a) < (b) ? (a) : (b))
#define MAX(a, b)          ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi)   ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* Swap two values of the same type using a temp (both evaluated once) */
#define SWAP(a, b)  do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while(0)

/* Get the struct that contains 'ptr' as the member named 'member' */
#define container_of(ptr, type, member) \
    ((type *)((uint8_t *)(ptr) - __builtin_offsetof(type, member)))

/* Branch-prediction hints — tell the CPU which path is hot */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* Compiler memory barrier — stops the compiler reordering memory ops */
#define compiler_barrier() __asm__ volatile("" ::: "memory")

/* Mark a code path as logically unreachable (UB if actually reached) */
#define unreachable() __builtin_unreachable()

/* Static (compile-time) assert — works in C99 and C11 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#else
#  define STATIC_ASSERT(cond, msg) \
       typedef char _slibc_sa_##__LINE__[(cond) ? 1 : -1]
#endif

/* Stringify a token (useful for compile-time messages) */
#define SLIBC_STRINGIFY(x)   #x
#define SLIBC_TOSTRING(x)    SLIBC_STRINGIFY(x)


/* ═══════════════════════════════════════════════════════════════════════
 *  §5  ★ Debug, Assert, and Panic macros
 *
 *  CONTRIBUTOR NOTE:
 *  ─────────────────
 *  These macros give human-readable error output, NOT register dumps.
 *  Use them throughout the codebase instead of bare if-checks or
 *  custom error printing.
 *
 *  Production builds: define SLIBC_NDEBUG to strip SLIBC_DBG output.
 *  Kernel builds:     define SYSTRIX_KERNEL so SLIBC_PUTS uses kputs.
 *  User builds:       SLIBC_PUTS uses the write() syscall (fd 2).
 *
 *  Quick reference:
 *  ┌──────────────────────────────────────────────────────────────────┐
 *  │  SLIBC_PANIC("reason")          → print & halt, never returns   │
 *  │  SLIBC_ASSERT(cond, "msg")      → panic if cond is false        │
 *  │  SLIBC_ASSERT_NOT_NULL(ptr)     → panic if ptr is NULL          │
 *  │  SLIBC_DBG("fmt", ...)          → debug print (stripped if NDEBUG)│
 *  │  SLIBC_TODO("note")             → compile-time warning + runtime │
 *  │  SLIBC_WARN("fmt", ...)         → always-on warning print       │
 *  │  SLIBC_CHECK_ERR(ret, "ctx")    → if ret<0, print & return ret  │
 *  │  SLIBC_ERR_STR(code)            → "Invalid argument" not "-22"  │
 *  └──────────────────────────────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Low-level output used by all debug macros.
 * Defined in systrix_libc.c — outputs to stderr (fd 2) in user space,
 * or calls kputs() in kernel space.
 */
void slibc_debug_puts(const char *s);
void slibc_debug_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/*
 * SLIBC_PANIC — print a readable message and halt.
 *
 * Output format:
 *   PANIC at myfile.c:42 in my_function():
 *     something went wrong
 *
 * Never returns. In kernel builds, halts the CPU.
 * In user builds, calls _exit(1) (or an equivalent syscall).
 */
void slibc_panic_impl(const char *file, int line,
                      const char *func, const char *msg)
    __attribute__((noreturn));

#define SLIBC_PANIC(msg) \
    slibc_panic_impl(__FILE__, __LINE__, __func__, (msg))

/*
 * SLIBC_ASSERT — check a condition; panic with a clear message if false.
 *
 * Output on failure:
 *   ASSERT FAILED at myfile.c:42 in my_function():
 *     Condition: ptr != NULL
 *     Message:   pointer must not be null here
 *
 * Usage:
 *   SLIBC_ASSERT(ptr != NULL, "pointer must not be null here");
 *   SLIBC_ASSERT(size > 0, "buffer size must be positive");
 */
void slibc_assert_impl(const char *file, int line, const char *func,
                       const char *cond_str, const char *msg)
    __attribute__((noreturn));

#define SLIBC_ASSERT(cond, msg) \
    do { if (unlikely(!(cond))) \
        slibc_assert_impl(__FILE__, __LINE__, __func__, #cond, (msg)); \
    } while(0)

/*
 * SLIBC_ASSERT_NOT_NULL — shorthand for the very common null-check.
 *
 * Output on failure:
 *   ASSERT FAILED at myfile.c:42 in my_function():
 *     Condition: buf != NULL
 *     Message:   NULL pointer dereference would occur
 */
#define SLIBC_ASSERT_NOT_NULL(ptr) \
    SLIBC_ASSERT((ptr) != NULL, "NULL pointer dereference would occur")

/*
 * SLIBC_ASSERT_RANGE — assert a value is within [lo, hi].
 *
 * Output on failure:
 *   ASSERT FAILED at myfile.c:42 in my_function():
 *     Condition: value in range [0, 255]
 *     Message:   value 300 is out of range [0, 255]
 */
void slibc_assert_range_impl(const char *file, int line, const char *func,
                             const char *name, long long val,
                             long long lo, long long hi)
    __attribute__((noreturn));

#define SLIBC_ASSERT_RANGE(val, lo, hi) \
    do { long long _v = (long long)(val); \
         if (unlikely(_v < (lo) || _v > (hi))) \
             slibc_assert_range_impl(__FILE__, __LINE__, __func__, \
                                     #val, _v, (lo), (hi)); \
    } while(0)

/*
 * SLIBC_DBG — debug print, stripped in production (SLIBC_NDEBUG).
 *
 * Output format:
 *   [DBG myfile.c:42] format string with args
 *
 * Usage:
 *   SLIBC_DBG("opening file: %s", path);
 *   SLIBC_DBG("value = %d, ptr = %p", val, ptr);
 */
#ifdef SLIBC_NDEBUG
#  define SLIBC_DBG(fmt, ...)  ((void)0)
#else
#  define SLIBC_DBG(fmt, ...) \
     slibc_debug_printf("[DBG %s:%d] " fmt "\n", \
                        __FILE__, __LINE__, ##__VA_ARGS__)
#endif

/*
 * SLIBC_WARN — always-on warning print (not stripped in production).
 *
 * Output format:
 *   [WARN myfile.c:42 my_function()] format string with args
 */
#define SLIBC_WARN(fmt, ...) \
    slibc_debug_printf("[WARN %s:%d %s()] " fmt "\n", \
                       __FILE__, __LINE__, __func__, ##__VA_ARGS__)

/*
 * SLIBC_TODO — marks unfinished code with a compile-time warning.
 *
 * At compile time: warns "TODO: your note here"
 * At runtime (debug mode): prints a warning when the code is reached.
 * At runtime (production): silent.
 *
 * Usage:
 *   SLIBC_TODO("implement error path for short writes");
 */
#define SLIBC_TODO(note) \
    do { \
        _Pragma("message(\"TODO: \" note \" [\" __FILE__ \":\" SLIBC_TOSTRING(__LINE__) \"]\")") \
        SLIBC_WARN("TODO reached: " note); \
    } while(0)

/*
 * SLIBC_CHECK_ERR — after a function call, check for an error return.
 * If the return value is negative, print a human-readable description
 * and return it from the current function.
 *
 * Usage:
 *   int fd = open_file(path);
 *   SLIBC_CHECK_ERR(fd, "opening config file");
 *   // if fd < 0, we already returned; safe to use fd here
 *
 * Output on error (example):
 *   ERROR in my_function() [myfile.c:42]: opening config file
 *     → No such file or directory (error -2)
 */
#define SLIBC_CHECK_ERR(ret, context) \
    do { long long _r = (long long)(ret); \
         if (unlikely(_r < 0)) { \
             slibc_debug_printf( \
                 "ERROR in %s() [%s:%d]: %s\n  → %s (error %lld)\n", \
                 __func__, __FILE__, __LINE__, (context), \
                 slibc_strerror_simple((int)_r), _r); \
             return (ret); \
         } \
    } while(0)

/*
 * SLIBC_CHECK_ERR_MSG — like SLIBC_CHECK_ERR but with a custom message.
 * Returns a specific value instead of the error code.
 *
 * Usage:
 *   SLIBC_CHECK_ERR_MSG(ret, "read failed", NULL);
 */
#define SLIBC_CHECK_ERR_MSG(ret, context, retval) \
    do { long long _r = (long long)(ret); \
         if (unlikely(_r < 0)) { \
             slibc_debug_printf( \
                 "ERROR in %s() [%s:%d]: %s\n  → %s (error %lld)\n", \
                 __func__, __FILE__, __LINE__, (context), \
                 slibc_strerror_simple((int)_r), _r); \
             return (retval); \
         } \
    } while(0)

/*
 * SLIBC_TRACE — trace function entry/exit for debugging (debug builds only).
 * Place at the very top of a function to log when it is called.
 *
 * Usage:
 *   int my_func(int x) {
 *       SLIBC_TRACE();
 *       ...
 *   }
 *
 * Output:
 *   [TRACE] → my_func() called at myfile.c:55
 */
#ifdef SLIBC_NDEBUG
#  define SLIBC_TRACE()  ((void)0)
#else
#  define SLIBC_TRACE() \
     slibc_debug_printf("[TRACE] → %s() called at %s:%d\n", \
                        __func__, __FILE__, __LINE__)
#endif

/*
 * SLIBC_HEXDUMP — dump a buffer as hex + ASCII (debug builds only).
 * Useful for debugging binary protocols and memory corruption.
 *
 * Usage:
 *   SLIBC_HEXDUMP("packet received", buf, len);
 *
 * Output:
 *   [HEXDUMP myfile.c:42] packet received (16 bytes):
 *   0000: 48 65 6c 6c 6f 20 57 6f  72 6c 64 0a 00 00 00 00  Hello Wo rld.....
 */
#ifdef SLIBC_NDEBUG
#  define SLIBC_HEXDUMP(label, buf, len)  ((void)0)
#else
#  define SLIBC_HEXDUMP(label, buf, len) \
     slibc_hexdump_impl(__FILE__, __LINE__, (label), (buf), (len))
#endif

/* Internal implementation for SLIBC_HEXDUMP */
void slibc_hexdump_impl(const char *file, int line, const char *label,
                        const void *buf, size_t len);


/* ═══════════════════════════════════════════════════════════════════════
 *  §6  Error codes
 *
 *  Error codes are NEGATIVE integers. Functions return a negative error
 *  code on failure, or a non-negative value on success.
 *
 *  Example:
 *    int fd = sys_open(path, flags);
 *    if (fd == ENOENT) { ... }   // file not found
 *    if (fd < 0)       { ... }   // any error
 *
 *  Use SLIBC_ERR_STR(code) or strerror(code) to get "No such file or
 *  directory" instead of "-2".
 * ═══════════════════════════════════════════════════════════════════════ */

#ifndef _SYSTRIX_ERRNO_DEFINED
#define _SYSTRIX_ERRNO_DEFINED

#define EPERM          (-1LL)   /* Operation not permitted              */
#define ENOENT         (-2LL)   /* No such file or directory            */
#define ESRCH          (-3LL)   /* No such process                      */
#define EINTR          (-4LL)   /* Interrupted system call              */
#define EIO            (-5LL)   /* I/O error                            */
#define EBADF          (-9LL)   /* Bad file descriptor                  */
#define ECHILD         (-10LL)  /* No child processes                   */
#define EAGAIN         (-11LL)  /* Try again (resource unavailable)     */
#define EWOULDBLOCK    (-11LL)  /* Same as EAGAIN                       */
#define ENOMEM         (-12LL)  /* Out of memory                        */
#define EACCES         (-13LL)  /* Permission denied                    */
#define EFAULT         (-14LL)  /* Bad address                          */
#define EBUSY          (-16LL)  /* Device or resource busy              */
#define EEXIST         (-17LL)  /* File already exists                  */
#define ENODEV         (-19LL)  /* No such device                       */
#define ENOTDIR        (-20LL)  /* Not a directory                      */
#define EISDIR         (-21LL)  /* Is a directory                       */
#define EINVAL         (-22LL)  /* Invalid argument                     */
#define ENFILE         (-23LL)  /* File table overflow (system limit)   */
#define EMFILE         (-24LL)  /* Too many open files (process limit)  */
#define ENOSPC         (-28LL)  /* No space left on device              */
#define EROFS          (-30LL)  /* Read-only file system                */
#define EPIPE          (-32LL)  /* Broken pipe                          */
#define ERANGE         (-34LL)  /* Result out of range                  */
#define ENAMETOOLONG   (-36LL)  /* File name too long                   */
#define ENOSYS         (-38LL)  /* Function not implemented             */
#define ENOTEMPTY      (-39LL)  /* Directory not empty                  */
#define EOVERFLOW      (-75LL)  /* Value too large for data type        */
#define EOPNOTSUPP     (-95LL)  /* Operation not supported              */
#define ENETUNREACH    (-101LL) /* Network unreachable                  */
#define EISCONN        (-106LL) /* Already connected                    */
#define ENOTCONN       (-107LL) /* Not connected                        */
#define ETIMEDOUT      (-110LL) /* Connection timed out                 */
#define ECONNREFUSED   (-111LL) /* Connection refused                   */
#define EALREADY       (-114LL) /* Operation already in progress        */
#define EINPROGRESS    (-115LL) /* Operation now in progress            */

#endif /* _SYSTRIX_ERRNO_DEFINED */


/* ═══════════════════════════════════════════════════════════════════════
 *  §7  Error helpers
 *
 *  These turn error codes into readable strings and provide convenient
 *  macros for the most common error-handling patterns.
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * strerror — convert an error code to a human-readable string.
 *
 * Accepts both negative (Systrix convention) and positive (POSIX style).
 * Returns a pointer to a string literal — do NOT free or modify it.
 * Unknown codes return "Unknown error (check errno definition)".
 *
 * Examples:
 *   strerror(-22) → "Invalid argument"
 *   strerror(-2)  → "No such file or directory"
 *   strerror(-12) → "Out of memory"
 */
const char *strerror(int errnum);

/*
 * slibc_strerror_simple — same as strerror but accepts a long long.
 * Used internally by the CHECK_ERR macros.
 */
const char *slibc_strerror_simple(int errnum);

/*
 * SLIBC_ERR_STR — get the error string for a return value inline.
 *
 * Usage:
 *   int ret = do_something();
 *   if (ret < 0)
 *       printf("failed: %s\n", SLIBC_ERR_STR(ret));
 */
#define SLIBC_ERR_STR(code)  strerror((int)(code))

/*
 * SLIBC_IS_ERR — check if a return value is an error.
 * Returns true (1) if ret represents an error condition.
 */
#define SLIBC_IS_ERR(ret)    ((long long)(ret) < 0)

/*
 * SLIBC_RETURN_IF_ERR — propagate an error code upward.
 * If the expression is an error (< 0), return it from the current function.
 *
 * Usage:
 *   SLIBC_RETURN_IF_ERR(do_setup());
 *   // only reaches here if do_setup() succeeded
 */
#define SLIBC_RETURN_IF_ERR(expr) \
    do { long long _e = (long long)(expr); \
         if (unlikely(_e < 0)) return (_e); \
    } while(0)


/* ═══════════════════════════════════════════════════════════════════════
 *  §8  Memory
 *
 *  Standard memory manipulation. These are the exact POSIX signatures.
 *  All are implemented in the .c file using simple byte loops
 *  (no compiler intrinsics) so they work freestanding.
 * ═══════════════════════════════════════════════════════════════════════ */

/// Fill 'n' bytes of 'dst' with the byte value 'c'. Returns dst.
void  *memset (void *dst, int c, size_t n);

/// Copy 'n' bytes from 'src' to 'dst'. Regions must NOT overlap. Returns dst.
void  *memcpy (void *dst, const void *src, size_t n);

/// Copy 'n' bytes from 'src' to 'dst'. Handles overlapping regions. Returns dst.
void  *memmove(void *dst, const void *src, size_t n);

/// Compare 'n' bytes. Returns <0, 0, >0 like strcmp.
int    memcmp (const void *a, const void *b, size_t n);

/// Find byte 'c' in the first 'n' bytes of 's'. Returns pointer or NULL.
void  *memchr (const void *s, int c, size_t n);


/* ═══════════════════════════════════════════════════════════════════════
 *  §9  String
 *
 *  Standard string functions. Implementations are in the .c file.
 *  Functions marked "safe" always NUL-terminate and don't overflow.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Length */
/// Return the number of bytes in 's' before the NUL terminator.
size_t  strlen (const char *s);

/// Like strlen but stops at maxlen (safe for unterminated buffers).
size_t  strnlen(const char *s, size_t maxlen);

/* Comparison */
/// Lexicographic comparison. Returns <0, 0, >0.
int     strcmp (const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
int     strcasecmp (const char *a, const char *b);   /* case-insensitive */
int     strncasecmp(const char *a, const char *b, size_t n);

/* Copy — prefer strlcpy over strcpy/strncpy */
char   *strcpy (char *dst, const char *src);          /* UNSAFE: no bound check */
char   *strncpy(char *dst, const char *src, size_t n);/* pads with NULs, may not NUL-term */

/// SAFE copy: always NUL-terminates; returns strlen(src). Use this one.
size_t  strlcpy(char *dst, const char *src, size_t sz);

/* Concatenation — prefer strlcat */
char   *strcat (char *dst, const char *src);           /* UNSAFE: no bound check */
char   *strncat(char *dst, const char *src, size_t n);

/// SAFE concatenation: always NUL-terminates; returns total desired length. Use this one.
size_t  strlcat(char *dst, const char *src, size_t sz);

/* Search */
/// Find first occurrence of 'c' in 's'. Returns pointer or NULL.
char   *strchr (const char *s, int c);
char   *strrchr(const char *s, int c);  /* last occurrence */
char   *strstr (const char *haystack, const char *needle);
char   *strpbrk(const char *s, const char *accept);  /* any char in accept */
size_t  strspn (const char *s, const char *accept);  /* leading span of accept chars */
size_t  strcspn(const char *s, const char *reject);  /* leading span not in reject */

/* Tokenisation */
/// Split string by delimiters. NOT re-entrant — use strtok_r instead.
char   *strtok  (char *s, const char *delim);
/// Re-entrant version of strtok. Safe to use in multi-threaded/recursive code.
char   *strtok_r(char *s, const char *delim, char **saveptr);


/* ═══════════════════════════════════════════════════════════════════════
 *  §10  Character classification
 * ═══════════════════════════════════════════════════════════════════════ */

int isdigit (int c);  /* '0'..'9' */
int isxdigit(int c);  /* '0'..'9', 'a'..'f', 'A'..'F' */
int isalpha (int c);  /* 'a'..'z', 'A'..'Z' */
int isalnum (int c);  /* isalpha or isdigit */
int isspace (int c);  /* space, tab, newline, carriage return, FF, VT */
int isupper (int c);  /* 'A'..'Z' */
int islower (int c);  /* 'a'..'z' */
int isprint (int c);  /* printable, including space (0x20..0x7e) */
int ispunct (int c);  /* printable, not alnum, not space */
int iscntrl (int c);  /* control chars (< 0x20 or 0x7f) */
int toupper (int c);  /* convert lowercase to uppercase */
int tolower (int c);  /* convert uppercase to lowercase */


/* ═══════════════════════════════════════════════════════════════════════
 *  §11  Integer conversion
 * ═══════════════════════════════════════════════════════════════════════ */

/// Parse signed decimal integer from string. Stops at first non-digit.
int           atoi  (const char *s);
long          atol  (const char *s);
long long     atoll (const char *s);

/// Full conversion with endptr and base (2..36 or 0 for auto-detect).
/// base 0: "0x" → hex, "0" → octal, otherwise decimal.
long          strtol  (const char *s, char **endptr, int base);
long long     strtoll (const char *s, char **endptr, int base);
unsigned long strtoul (const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);


/* ═══════════════════════════════════════════════════════════════════════
 *  §12  Numeric helpers
 * ═══════════════════════════════════════════════════════════════════════ */

int       abs  (int x);
long      labs (long x);
long long llabs(long long x);


/* ═══════════════════════════════════════════════════════════════════════
 *  §13  Formatted output
 *
 *  No floating-point support (%f/%e/%g not implemented).
 *
 *  Supported format specifiers:
 *    %d %i      — signed decimal int
 *    %u         — unsigned decimal int
 *    %x %X      — hex (lower/upper case)
 *    %o         — octal
 *    %c         — character
 *    %s         — string
 *    %p         — pointer (hex with 0x prefix)
 *    %%         — literal percent
 *    %ld %lu %lx  — long variants
 *    %lld %llu %llx — long long variants
 *    Width: %5d   — right-align in 5 columns
 *    Precision: %.3s — truncate string to 3 chars
 *    Flags: %-10s  — left-align, %08x — zero-pad, %+d — force sign
 * ═══════════════════════════════════════════════════════════════════════ */

int snprintf (char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int sprintf  (char *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vsprintf (char *buf, const char *fmt, va_list ap);

/*
 * slibc_vprintf_cb — low-level printf with a callback per character.
 * Used by the kernel's kprintf to write directly to the terminal
 * without needing a buffer. You can use this to implement custom
 * output sinks (e.g., write to a ring buffer).
 *
 * cb(ctx, c) is called once per output character.
 */
typedef void (*slibc_putc_fn)(void *ctx, char c);
int slibc_vprintf_cb(slibc_putc_fn cb, void *ctx,
                     const char *fmt, va_list ap);


/* ═══════════════════════════════════════════════════════════════════════
 *  §14  Integer-to-string helpers
 *
 *  Fast conversions without full printf overhead.
 *  Each buf must be large enough (see comment on each function).
 * ═══════════════════════════════════════════════════════════════════════ */

/// Write decimal digits of v into buf (must be ≥ 20 bytes). Returns char count.
int slibc_u64_to_dec(uint64_t v, char *buf);

/// Write lowercase hex digits of v into buf (must be ≥ 17 bytes). Returns char count.
int slibc_u64_to_hex(uint64_t v, char *buf);

/// Write uppercase hex digits of v into buf (must be ≥ 17 bytes). Returns char count.
int slibc_u64_to_HEX(uint64_t v, char *buf);


/* ═══════════════════════════════════════════════════════════════════════
 *  §15  Sorting and searching
 * ═══════════════════════════════════════════════════════════════════════ */

/// Sort 'nmemb' elements of size 'size' using comparator 'cmp'. In-place.
void   qsort  (void *base, size_t nmemb, size_t size,
               int (*cmp)(const void *, const void *));

/// Binary search for 'key' in sorted array. Returns pointer to match or NULL.
void  *bsearch(const void *key, const void *base,
               size_t nmemb, size_t size,
               int (*cmp)(const void *, const void *));


/* ═══════════════════════════════════════════════════════════════════════
 *  §16  setjmp / longjmp
 *
 *  Used for non-local exits (e.g., error recovery without unwinding).
 *
 *  jmp_buf layout (x86-64, 8 × 8 bytes = 64 bytes):
 *    [0] rbx  [1] rbp  [2] r12  [3] r13  [4] r14  [5] r15
 *    [6] rsp  [7] rip  (return address saved by setjmp call site)
 *
 *  WARNING: these are inline asm — only call from functions, not
 *  inside inline expressions. setjmp must be called from the function
 *  that will eventually call longjmp or a parent of it.
 * ═══════════════════════════════════════════════════════════════════════ */

typedef unsigned long long jmp_buf[8];

static inline int setjmp(jmp_buf env) {
    int r;
    __asm__ volatile(
        "mov  %%rbx,    (%1)\n\t"
        "mov  %%rbp,   8(%1)\n\t"
        "mov  %%r12,  16(%1)\n\t"
        "mov  %%r13,  24(%1)\n\t"
        "mov  %%r14,  32(%1)\n\t"
        "mov  %%r15,  40(%1)\n\t"
        "lea  8(%%rsp), %%rax\n\t"
        "mov  %%rax,  48(%1)\n\t"
        "mov  (%%rsp), %%rax\n\t"
        "mov  %%rax,  56(%1)\n\t"
        "xor  %0, %0\n\t"
        : "=r"(r) : "r"(env) : "rax", "memory");
    return r;
}

static inline void longjmp(jmp_buf env, int val)
    __attribute__((noreturn));
static inline void longjmp(jmp_buf env, int val) {
    if (val == 0) val = 1;   /* longjmp(env, 0) must return 1 from setjmp */
    __asm__ volatile(
        "mov    (%0), %%rbx\n\t"
        "mov   8(%0), %%rbp\n\t"
        "mov  16(%0), %%r12\n\t"
        "mov  24(%0), %%r13\n\t"
        "mov  32(%0), %%r14\n\t"
        "mov  40(%0), %%r15\n\t"
        "mov  48(%0), %%rsp\n\t"
        "mov  56(%0), %%rax\n\t"
        "mov  %1,     %%edi\n\t"
        "jmp  *%%rax\n\t"
        : : "r"(env), "r"(val) :);
    __builtin_unreachable();
}


/* ═══════════════════════════════════════════════════════════════════════
 *  §17  Integer math & bit manipulation
 * ═══════════════════════════════════════════════════════════════════════ */

/* Powers and logarithms */
uint64_t slibc_pow_u64   (uint64_t base, uint32_t exp);
int64_t  slibc_pow_i64   (int64_t  base, uint32_t exp);
int      slibc_log2_u64  (uint64_t v);  /* floor(log2(v)),  -1 if v==0 */
int      slibc_log10_u64 (uint64_t v);  /* floor(log10(v)), -1 if v==0 */

/* Rounding to powers of two */
uint64_t slibc_round_up_pow2  (uint64_t v);  /* next power of 2 ≥ v */
uint64_t slibc_round_down_pow2(uint64_t v);  /* largest power of 2 ≤ v */

/* Bit counting */
int      slibc_popcount    (uint64_t v);  /* count of set bits */
int      slibc_clz         (uint64_t v);  /* count leading zeros (63..0) */
int      slibc_ctz         (uint64_t v);  /* count trailing zeros */
int      slibc_parity      (uint64_t v);  /* 1 if odd number of set bits */
uint64_t slibc_reverse_bits(uint64_t v);  /* bit-reverse a 64-bit word */
uint8_t  slibc_reverse_byte(uint8_t  v);  /* bit-reverse a byte */
uint64_t slibc_rotl64(uint64_t v, int n); /* rotate left */
uint64_t slibc_rotr64(uint64_t v, int n); /* rotate right */
uint32_t slibc_rotl32(uint32_t v, int n);
uint32_t slibc_rotr32(uint32_t v, int n);

/* Byte-swap (endian conversion) */
uint16_t slibc_bswap16(uint16_t v);
uint32_t slibc_bswap32(uint32_t v);
uint64_t slibc_bswap64(uint64_t v);

/* Host ↔ big/little endian (only correct on LE hosts like x86-64) */
#define slibc_htobe16(x)  slibc_bswap16(x)
#define slibc_htole16(x)  (x)
#define slibc_be16toh(x)  slibc_bswap16(x)
#define slibc_le16toh(x)  (x)
#define slibc_htobe32(x)  slibc_bswap32(x)
#define slibc_htole32(x)  (x)
#define slibc_be32toh(x)  slibc_bswap32(x)
#define slibc_le32toh(x)  (x)
#define slibc_htobe64(x)  slibc_bswap64(x)
#define slibc_htole64(x)  (x)
#define slibc_be64toh(x)  slibc_bswap64(x)
#define slibc_le64toh(x)  (x)

/* Overflow-safe arithmetic — return 1 on overflow, 0 on success */
int slibc_add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out);
int slibc_mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *out);
int slibc_add_overflow_i64(int64_t  a, int64_t  b, int64_t  *out);
int slibc_mul_overflow_i64(int64_t  a, int64_t  b, int64_t  *out);

/* Greatest common divisor / least common multiple */
uint64_t slibc_gcd(uint64_t a, uint64_t b);
uint64_t slibc_lcm(uint64_t a, uint64_t b);  /* returns 0 on overflow */

/* Saturating arithmetic (clamp instead of overflow) */
uint64_t slibc_sat_add_u64(uint64_t a, uint64_t b);
uint64_t slibc_sat_sub_u64(uint64_t a, uint64_t b);
int64_t  slibc_sat_add_i64(int64_t  a, int64_t  b);
int64_t  slibc_sat_sub_i64(int64_t  a, int64_t  b);

/* Division helpers */
int64_t  slibc_div_round_up (int64_t  n, int64_t  d);  /* ceil(n/d) for d>0 */
uint64_t slibc_udiv_round_up(uint64_t n, uint64_t d);


/* ═══════════════════════════════════════════════════════════════════════
 *  §18  String extras
 *
 *  Higher-level string utilities beyond the POSIX set.
 *  Functions that allocate use malloc() — kernel must supply it.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Allocation — caller frees with free() */
/// Duplicate string. Returns newly allocated copy, or NULL on OOM.
char *slibc_strdup (const char *s);
/// Duplicate at most n chars. Always NUL-terminates. Returns NULL on OOM.
char *slibc_strndup(const char *s, size_t n);

/* In-place transforms */
void  slibc_strupr  (char *s);   /* ASCII uppercase in-place */
void  slibc_strlwr  (char *s);   /* ASCII lowercase in-place */
void  slibc_strrev  (char *s);   /* reverse string in-place */
void  slibc_strtrim (char *s);   /* strip leading + trailing whitespace */
char *slibc_ltrim   (char *s);   /* return pointer past leading whitespace */
void  slibc_rtrim   (char *s);   /* strip trailing whitespace in-place */

/* Predicates */
int   slibc_str_starts_with(const char *s, const char *prefix);
int   slibc_str_ends_with  (const char *s, const char *suffix);
int   slibc_str_is_empty   (const char *s);   /* true if NULL or "" */
int   slibc_str_is_int     (const char *s);   /* valid signed decimal */
int   slibc_str_is_uint    (const char *s);   /* valid unsigned decimal */

/* Search */
/// Count occurrences of needle in haystack.
size_t slibc_str_count(const char *haystack, const char *needle);

/* Replace */
/// Replace first/all occurrences of 'from' with 'to' in 'src', write to buf.
/// Returns 0 on success, -1 if buf is too small.
int slibc_str_replace(const char *src, const char *from,
                      const char *to, char *buf, size_t bufsz);

/* Split / join */
/// Split 'src' by 'delim' into out_parts[]. Modifies src (inserts NULs).
/// Returns number of parts found (≤ max_parts).
int slibc_str_split(char *src, char delim,
                    char **out_parts, int max_parts);

/// Join array of strings with separator into buf. Returns 0 or -1 on truncation.
int slibc_str_join(const char **parts, int count,
                   char sep, char *buf, size_t bufsz);

/* Hex encoding */
/// Encode 'srclen' bytes as lowercase hex into 'out' (must be 2*srclen+1 bytes).
void slibc_hex_encode(const uint8_t *src, size_t srclen, char *out);

/// Decode hex string into 'out' (must be ≥ srclen/2 bytes).
/// Returns bytes written, or -1 on invalid input.
int  slibc_hex_decode(const char *hex, uint8_t *out, size_t outlen);


/* ═══════════════════════════════════════════════════════════════════════
 *  §19  Hashing
 * ═══════════════════════════════════════════════════════════════════════ */

/// FNV-1a 32-bit — fast, non-cryptographic hash. Good for hash tables.
uint32_t slibc_fnv1a32(const void *data, size_t len);
/// FNV-1a 64-bit variant.
uint64_t slibc_fnv1a64(const void *data, size_t len);

/// DJB2 — classic simple string hash.
uint32_t slibc_djb2(const char *s);

/// MurmurHash3 (32-bit output). Better distribution than DJB2.
uint32_t slibc_murmur3_32(const void *data, size_t len, uint32_t seed);

/// CRC-32 (ISO 3309 / Ethernet polynomial 0xEDB88320).
uint32_t slibc_crc32(const void *data, size_t len);
/// Incremental CRC-32: call slibc_crc32(data1,len1), then pass result as 'crc' here.
uint32_t slibc_crc32_update(uint32_t crc, const void *data, size_t len);

/// Adler-32 (zlib compatible).
uint32_t slibc_adler32(const void *data, size_t len);
uint32_t slibc_adler32_update(uint32_t adler, const void *data, size_t len);


/* ═══════════════════════════════════════════════════════════════════════
 *  §20  Pseudo-random number generation  (xorshift64 + splitmix64)
 *
 *  Thread-safety: each thread should have its own SRng.
 *  Global convenience functions (slibc_srand / slibc_rand) are NOT
 *  thread-safe.
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct { uint64_t state; } SRng;

/// Seed a PRNG state. Different seeds produce different sequences.
void     slibc_rng_seed  (SRng *r, uint64_t seed);
/// Generate raw 64-bit pseudo-random value.
uint64_t slibc_rng_next  (SRng *r);
/// Generate uniform 32-bit value.
uint32_t slibc_rng_u32   (SRng *r);
/// Generate value in [lo, hi). Panics if lo >= hi.
uint64_t slibc_rng_range (SRng *r, uint64_t lo, uint64_t hi);
/// Fill buf with n random bytes.
void     slibc_rng_fill  (SRng *r, void *buf, size_t n);
/// Fisher-Yates shuffle of an array in-place.
void     slibc_rng_shuffle(SRng *r, void *base, size_t nmemb, size_t sz);

/* Global (not thread-safe) convenience API */
void     slibc_srand     (uint64_t seed);
uint64_t slibc_rand      (void);
uint64_t slibc_rand_range(uint64_t lo, uint64_t hi);


/* ═══════════════════════════════════════════════════════════════════════
 *  §21  Ring buffer  (single-producer / single-consumer, lock-free on x86)
 *
 *  Use this for producer/consumer communication without locks, as long
 *  as there is exactly ONE producer and ONE consumer thread.
 *
 *  Example:
 *    uint8_t storage[sizeof(MyMsg) * 64];
 *    SRing ring;
 *    slibc_ring_init(&ring, storage, 64, sizeof(MyMsg));
 *
 *    MyMsg m = { .id = 1 };
 *    slibc_ring_push(&ring, &m);  // producer side
 *
 *    MyMsg out;
 *    slibc_ring_pop(&ring, &out); // consumer side
 * ═══════════════════════════════════════════════════════════════════════ */

#define SLIBC_RING_MAX_POW2  (1u << 24)  /* 16M entries maximum */

typedef struct {
    uint8_t  *buf;    /* storage (must be elem_size * capacity bytes) */
    uint32_t  cap;    /* capacity — MUST be a power of 2 */
    uint32_t  esize;  /* element size in bytes */
    uint32_t  head;   /* producer write index */
    uint32_t  tail;   /* consumer read index  */
} SRing;

/// Initialise ring buffer. buf must be pre-allocated; cap must be a power of 2.
void slibc_ring_init (SRing *r, void *buf, uint32_t cap, uint32_t elem_size);

/// Push one element. Returns 0 on success, -1 if ring is full.
int  slibc_ring_push (SRing *r, const void *elem);

/// Pop one element into *elem. Returns 0 on success, -1 if empty.
int  slibc_ring_pop  (SRing *r, void *elem);

/// Peek at next element without consuming it. Returns 0 or -1 if empty.
int  slibc_ring_peek (const SRing *r, void *elem);

/// Discard all elements.
void slibc_ring_clear(SRing *r);

static inline int      slibc_ring_empty(const SRing *r) { return r->head == r->tail; }
static inline int      slibc_ring_full (const SRing *r) { return (r->head - r->tail) == r->cap; }
static inline uint32_t slibc_ring_count(const SRing *r) { return r->head - r->tail; }


/* ═══════════════════════════════════════════════════════════════════════
 *  §22  Bitmap  (compact bit array)
 *
 *  Example:
 *    uint64_t words[4];                          // 256 bits
 *    SBitmap bm;
 *    slibc_bm_init(&bm, words, 256);
 *    slibc_bm_set(&bm, 42);
 *    if (slibc_bm_test(&bm, 42)) { ... }
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t *words;  /* storage — ceil(nbits/64) uint64_t values */
    size_t    nbits;  /* total number of bits */
} SBitmap;

/// Initialise bitmap with pre-allocated words array of ceil(nbits/64) uint64_t.
void   slibc_bm_init   (SBitmap *bm, uint64_t *words, size_t nbits);
void   slibc_bm_set    (SBitmap *bm, size_t bit);
void   slibc_bm_clear  (SBitmap *bm, size_t bit);
int    slibc_bm_test   (const SBitmap *bm, size_t bit);
void   slibc_bm_toggle (SBitmap *bm, size_t bit);
void   slibc_bm_zero   (SBitmap *bm);  /* clear all bits */
void   slibc_bm_fill   (SBitmap *bm);  /* set all bits */
/// Find first set bit at or after 'start'. Returns nbits if none found.
size_t slibc_bm_first_set  (const SBitmap *bm, size_t start);
/// Find first clear bit at or after 'start'. Returns nbits if none found.
size_t slibc_bm_first_clear(const SBitmap *bm, size_t start);
size_t slibc_bm_count_set  (const SBitmap *bm);


/* ═══════════════════════════════════════════════════════════════════════
 *  §23  Fixed-width formatting helpers  (no heap, stack-only)
 * ═══════════════════════════════════════════════════════════════════════ */

/// Format byte count as human-readable: "1.23 MB", "456 B". buf ≥ 16 bytes.
void slibc_fmt_bytes(uint64_t bytes, char *buf, size_t bufsz);

/// Format duration in ms: "2h 03m 47s" or "347ms". buf ≥ 24 bytes.
void slibc_fmt_duration_ms(uint64_t ms, char *buf, size_t bufsz);

/// Zero-pad an integer to width digits: slibc_fmt_zpad(42, 5) → "00042".
void slibc_fmt_zpad(uint64_t v, int width, char *buf);

/// Format IPv4 address from 32-bit big-endian: "192.168.1.1". buf ≥ 16.
void slibc_fmt_ipv4(uint32_t ip_be, char *buf);

/// Format MAC address from 6-byte array: "aa:bb:cc:dd:ee:ff". buf ≥ 18.
void slibc_fmt_mac(const uint8_t mac[6], char *buf);

/// Parse IPv4 dotted-decimal string into big-endian 32-bit. Returns 0 or -1.
int  slibc_parse_ipv4(const char *s, uint32_t *out_be);

/// Write a text progress bar "[=====>   ]" into buf. buf ≥ total_width+3.
void slibc_fmt_progress(char *buf, size_t total_width,
                        uint64_t numerator, uint64_t denominator);

/// Left-pad or right-pad str to width using pad_char. Returns chars written.
size_t slibc_str_pad(char *dst, size_t dst_sz,
                     const char *str, size_t width,
                     char pad_char, int left_align);

/// Write 'count' copies of 'c' into buf (buf ≥ count+1). Returns count.
size_t slibc_str_repeat(char *buf, char c, size_t count);


/* ═══════════════════════════════════════════════════════════════════════
 *  §24  Intrusive doubly-linked list
 *
 *  Embed an SListNode into your struct, then manipulate it with the
 *  macros below. The list head is itself an SListNode (sentinel node).
 *
 *  Example:
 *    typedef struct {
 *        int      value;
 *        SListNode node;     // embed the link node here
 *    } MyItem;
 *
 *    SListNode head;
 *    slibc_list_init(&head);
 *
 *    MyItem item = { .value = 42 };
 *    slibc_list_push_back(&head, &item.node);
 *
 *    // iterate:
 *    slibc_list_foreach(n, &head) {
 *        MyItem *it = container_of(n, MyItem, node);
 *        // use it->value
 *    }
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct SListNode {
    struct SListNode *prev;
    struct SListNode *next;
} SListNode;

static inline void slibc_list_init(SListNode *head) {
    head->prev = head; head->next = head;
}
static inline int slibc_list_empty(const SListNode *head) {
    return head->next == head;
}
static inline void _slibc_list_insert(SListNode *prev, SListNode *next,
                                       SListNode *node) {
    node->prev = prev; node->next = next;
    prev->next = node; next->prev = node;
}
static inline void slibc_list_push_back (SListNode *head, SListNode *node) {
    _slibc_list_insert(head->prev, head, node);
}
static inline void slibc_list_push_front(SListNode *head, SListNode *node) {
    _slibc_list_insert(head, head->next, node);
}
static inline void slibc_list_remove(SListNode *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = node;
}
static inline SListNode *slibc_list_pop_front(SListNode *head) {
    if (slibc_list_empty(head)) return NULL;
    SListNode *n = head->next; slibc_list_remove(n); return n;
}
static inline SListNode *slibc_list_pop_back(SListNode *head) {
    if (slibc_list_empty(head)) return NULL;
    SListNode *n = head->prev; slibc_list_remove(n); return n;
}
static inline size_t slibc_list_count(const SListNode *head) {
    size_t n = 0;
    const SListNode *c = head->next;
    while (c != head) { n++; c = c->next; }
    return n;
}

/// Iterate all nodes. 'node' is set to each SListNode* in turn.
/// Do NOT remove nodes during iteration — use slibc_list_foreach_safe instead.
#define slibc_list_foreach(node, head) \
    for (SListNode *(node) = (head)->next; \
         (node) != (head); \
         (node) = (node)->next)

/// Iterate all nodes safely — allows removing 'node' during the loop.
#define slibc_list_foreach_safe(node, tmp, head) \
    for (SListNode *(node) = (head)->next, \
                   *(tmp)  = (node)->next; \
         (node) != (head); \
         (node) = (tmp), (tmp) = (tmp)->next)


/* ═══════════════════════════════════════════════════════════════════════
 *  §25  Dynamic string builder  (SStrBuf)
 *
 *  A growable string that automatically reallocates as you append.
 *  Uses malloc/realloc/free — kernel must supply them.
 *
 *  Example:
 *    SStrBuf sb;
 *    slibc_sb_init(&sb);
 *    slibc_sb_appendf(&sb, "Hello, %s!\n", name);
 *    slibc_sb_appendf(&sb, "You have %d messages.\n", count);
 *    do_something_with(sb.data);
 *    slibc_sb_free(&sb);
 *
 *  Or steal the buffer (caller owns it afterward):
 *    char *s = slibc_sb_steal(&sb);   // sb is reset, s must be freed
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    char   *data;  /* NUL-terminated string (NULL before first append) */
    size_t  len;   /* current length, not including NUL */
    size_t  cap;   /* allocated capacity including NUL slot */
} SStrBuf;

void  slibc_sb_init   (SStrBuf *sb);
void  slibc_sb_free   (SStrBuf *sb);
void  slibc_sb_reset  (SStrBuf *sb);   /* reset to empty, keep storage */

int   slibc_sb_append (SStrBuf *sb, const char *s);
int   slibc_sb_appendn(SStrBuf *sb, const char *s, size_t n);
int   slibc_sb_appendc(SStrBuf *sb, char c);
int   slibc_sb_appendf(SStrBuf *sb, const char *fmt, ...)
          __attribute__((format(printf, 2, 3)));

/// Transfer ownership of the internal buffer to the caller.
/// After this call, sb is reset. The returned pointer must be freed.
char *slibc_sb_steal  (SStrBuf *sb);


/* ═══════════════════════════════════════════════════════════════════════
 *  §26  Path utilities  (no heap — all use caller-supplied buffers)
 *
 *  '/' is the path separator. All functions handle:
 *    - Absolute paths: /foo/bar
 *    - Relative paths: foo/bar
 *    - Trailing slashes: /foo/bar/  → /foo/bar
 *    - Double slashes:   /foo//bar  → /foo/bar
 *    - Dot components:   /foo/./bar → /foo/bar
 *    - Dot-dot:          /foo/bar/../baz → /foo/baz
 * ═══════════════════════════════════════════════════════════════════════ */

/// Join base + "/" + name into dst. If name is absolute it replaces base.
/// Returns 0 on success, -1 if truncated.
int   slibc_path_join     (char *dst, size_t dst_sz,
                           const char *base, const char *name);

/// Normalise path in-place. Returns path for convenience.
char *slibc_path_normalize(char *path);

/// Return pointer to the last component of path (no allocation needed).
/// "/foo/bar" → "bar", "/" → "/"
const char *slibc_path_basename(const char *path);

/// Copy the directory part of path into dst. Returns 0 or -1 on truncation.
/// "/foo/bar" → "/foo", "foo" → "."
int   slibc_path_dirname  (const char *path, char *dst, size_t dst_sz);

/// Returns 1 if path is absolute (starts with '/'), else 0.
static inline int slibc_path_is_absolute(const char *path) {
    return path && path[0] == '/';
}

/// Returns 1 if path has the given file extension (case-insensitive).
/// slibc_path_has_ext("foo.TXT", "txt") → 1
int   slibc_path_has_ext  (const char *path, const char *ext);


/* ═══════════════════════════════════════════════════════════════════════
 *  §27  UUID generation  (random, version 4)
 *
 *  Requires an SRng seeded with a good source of entropy.
 *  Output format: "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
 *                  where x = random hex, y = 8, 9, a, or b
 * ═══════════════════════════════════════════════════════════════════════ */

/// Generate a UUID v4 string into buf (must be ≥ 37 bytes).
void slibc_uuid_v4(SRng *rng, char *buf);
