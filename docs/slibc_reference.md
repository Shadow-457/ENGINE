# Systrix slibc — Syntax Reference

> **Include:** `#include "libc/systrix_libc.h"` (kernel) or pulled in automatically via `user/libc.h` (user space)
> **Compiles with:** `-ffreestanding -nostdlib -nostdinc` — no host C library needed

---

## Table of Contents

1. [Types & Constants](#1-types--constants)
2. [Utility Macros](#2-utility-macros)
3. [Memory Functions](#3-memory-functions)
4. [String Functions](#4-string-functions)
5. [Character Classification](#5-character-classification)
6. [Number Conversion](#6-number-conversion)
7. [Formatted Output](#7-formatted-output)
8. [Integer-to-String Helpers](#8-integer-to-string-helpers)
9. [Sorting & Searching](#9-sorting--searching)
10. [setjmp / longjmp](#10-setjmp--longjmp)
11. [Integer Math & Bit Operations](#11-integer-math--bit-operations)
12. [String Extras](#12-string-extras)
13. [Hashing](#13-hashing)
14. [Random Number Generation](#14-random-number-generation)
15. [Ring Buffer](#15-ring-buffer-sring)
16. [Bitmap](#16-bitmap-sbitmap)
17. [Formatting Helpers](#17-formatting-helpers)
18. [Sorting Extras](#18-sorting-extras)
19. [Memory Pool](#19-memory-pool-spool)
20. [Config Parser](#20-config-parser-scfg)
21. [Base64](#21-base64)
22. [UUID](#22-uuid-suuid)
23. [Debug Utilities](#23-debug-utilities)
24. [Linked List](#24-linked-list-slistnode)
25. [Error Codes](#25-error-codes)
26. [strerror](#26-strerror)
27. [Path Utilities](#27-path-utilities)
28. [String Padding](#28-string-padding)
29. [Dynamic String Builder](#29-dynamic-string-builder-sstrbuf)

---

## 1. Types & Constants

### Primitive Types

| Type | Description |
|------|-------------|
| `uint8_t` | unsigned 8-bit |
| `uint16_t` | unsigned 16-bit |
| `uint32_t` | unsigned 32-bit |
| `uint64_t` | unsigned 64-bit |
| `int8_t` | signed 8-bit |
| `int16_t` | signed 16-bit |
| `int32_t` | signed 32-bit |
| `int64_t` | signed 64-bit |
| `size_t` | unsigned 64-bit (unsigned long long) |
| `ssize_t` | signed 64-bit |
| `uintptr_t` | unsigned pointer-sized integer |
| `intptr_t` | signed pointer-sized integer |
| `ptrdiff_t` | signed pointer difference |

### Kernel Short Aliases

| Alias | Maps to |
|-------|---------|
| `u8` | `uint8_t` |
| `u16` | `uint16_t` |
| `u32` | `uint32_t` |
| `u64` | `uint64_t` |
| `i64` | `int64_t` |
| `usize` | `size_t` |

### Constants

```c
NULL    // ((void*)0)
EOF     // (-1)
true    // 1
false   // 0

SEEK_SET  // 0
SEEK_CUR  // 1
SEEK_END  // 2
```

### Limits

```c
INT8_MIN    INT8_MAX    UINT8_MAX
INT16_MIN   INT16_MAX   UINT16_MAX
INT32_MIN   INT32_MAX   UINT32_MAX
INT64_MIN   INT64_MAX   UINT64_MAX
INT_MIN     INT_MAX     UINT_MAX
LONG_MIN    LONG_MAX    SIZE_MAX
```

---

## 2. Utility Macros

```c
ALIGN_UP(v, a)        // round v up to next multiple of a (a must be power of 2)
ALIGN_DOWN(v, a)      // round v down to multiple of a

BIT(n)                // 1UL << n
IS_POWER_OF_2(n)      // 1 if n is a power of two, else 0

UNUSED(x)             // suppress unused-variable warning
ARRAY_SIZE(a)         // number of elements in a static array

MIN(a, b)             // smaller of a, b
MAX(a, b)             // larger of a, b
CLAMP(v, lo, hi)      // clamp v to [lo, hi]
```

**Examples:**
```c
size_t aligned = ALIGN_UP(ptr, 16);     // align to 16 bytes
u32 mask = BIT(3) | BIT(7);            // 0x88
int count = ARRAY_SIZE(my_array);      // no sizeof math needed
int x = CLAMP(raw, 0, 255);            // keep in byte range
```

---

## 3. Memory Functions

```c
void *memset (void *dst, int c, size_t n);
void *memcpy (void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);   // safe for overlapping regions
int   memcmp (const void *a, const void *b, size_t n); // <0, 0, >0
void *memchr (const void *s, int c, size_t n);         // find byte, NULL if not found
```

**Examples:**
```c
memset(buf, 0, sizeof(buf));           // zero a buffer
memcpy(dst, src, 64);                  // copy 64 bytes
memmove(buf, buf + 4, len - 4);       // shift left, safe overlap
if (memcmp(a, b, 16) == 0) { ... }    // compare 16 bytes
u8 *p = memchr(data, 0xFF, len);      // find 0xFF byte
```

---

## 4. String Functions

### Length

```c
size_t strlen (const char *s);
size_t strnlen(const char *s, size_t maxlen);   // won't read past maxlen bytes
```

### Comparison

```c
int strcmp (const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp (const char *a, const char *b);   // case-insensitive
int strncasecmp(const char *a, const char *b, size_t n);
```

All return: `< 0` if a < b, `0` if equal, `> 0` if a > b.

### Copy

```c
char  *strcpy (char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
size_t strlcpy(char *dst, const char *src, size_t sz);  // PREFERRED — always NUL-terminates
                                                          // returns source length
```

> **Prefer `strlcpy` over `strcpy` / `strncpy`.**
> If return value >= sz, the string was truncated.

### Concatenation

```c
char  *strcat (char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
size_t strlcat(char *dst, const char *src, size_t sz);  // PREFERRED — always NUL-terminates
```

### Search

```c
char  *strchr (const char *s, int c);             // first occurrence of c
char  *strrchr(const char *s, int c);             // last  occurrence of c
char  *strstr (const char *haystack, const char *needle);
char  *strpbrk(const char *s, const char *accept); // first char in accept set
size_t strspn (const char *s, const char *accept); // length of leading accept chars
size_t strcspn(const char *s, const char *reject); // length before first reject char
```

### Tokenising

```c
char *strtok  (char *s, const char *delim);              // NOT re-entrant
char *strtok_r(char *s, const char *delim, char **saveptr); // re-entrant version
```

**Example:**
```c
char line[] = "foo:bar:baz";
char *save;
char *tok = strtok_r(line, ":", &save);
while (tok) {
    kprintf("%s\n", tok);
    tok = strtok_r(NULL, ":", &save);
}
```

---

## 5. Character Classification

All take an `int` (pass `(unsigned char)c` to avoid sign issues).
Return non-zero if true, zero if false.

```c
int isdigit (int c);   // '0'..'9'
int isxdigit(int c);   // '0'..'9', 'a'..'f', 'A'..'F'
int isalpha (int c);   // 'a'..'z', 'A'..'Z'
int isalnum (int c);   // isalpha || isdigit
int isspace (int c);   // space, \t, \n, \r, \f, \v
int isupper (int c);   // 'A'..'Z'
int islower (int c);   // 'a'..'z'
int isprint (int c);   // printable (0x20..0x7E)
int ispunct (int c);   // printable, not alnum, not space
int iscntrl (int c);   // control character

int toupper (int c);   // convert to uppercase
int tolower (int c);   // convert to lowercase
```

**Example:**
```c
while (isspace((unsigned char)*p)) p++;   // skip whitespace
if (isdigit((unsigned char)*p)) { ... }   // check digit
```

---

## 6. Number Conversion

### String to integer (simple)

```c
int       atoi (const char *s);
long      atol (const char *s);
long long atoll(const char *s);
```

### String to integer (with control)

```c
long               strtol  (const char *s, char **endptr, int base);
long long          strtoll (const char *s, char **endptr, int base);
unsigned long      strtoul (const char *s, char **endptr, int base);
unsigned long long strtoull(const char *s, char **endptr, int base);
```

`base` = 0 auto-detects (0x → hex, 0 → octal, else decimal).
`endptr` is set to the first character not consumed. Pass `NULL` to ignore.

### Absolute value

```c
int       abs  (int x);
long      labs (long x);
long long llabs(long long x);
```

**Examples:**
```c
int n = atoi("42");
char *end;
u64 addr = strtoull("0xDEADBEEF", &end, 0);  // base 0 auto-detects hex
long x = strtol("-123", NULL, 10);
```

---

## 7. Formatted Output

> No floating point — `%f`, `%e`, `%g` are **not supported**.

```c
int snprintf (char *buf, size_t size, const char *fmt, ...);
int sprintf  (char *buf,              const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vsprintf (char *buf,              const char *fmt, va_list ap);
```

**Supported format specifiers:**

| Specifier | Meaning |
|-----------|---------|
| `%d` / `%i` | signed decimal int |
| `%u` | unsigned decimal |
| `%x` / `%X` | hex lowercase / uppercase |
| `%o` | octal |
| `%c` | character |
| `%s` | string |
| `%p` | pointer (hex with `0x`) |
| `%%` | literal `%` |
| `%ld` / `%lu` / `%lx` | long variants |
| `%lld` / `%llu` / `%llx` | long long variants |

**Flags and width:**

| Syntax | Effect |
|--------|--------|
| `%5d` | right-align in field of 5 |
| `%-10s` | left-align in field of 10 |
| `%08x` | zero-pad to 8 hex digits |
| `%+d` | always show sign |
| `%.3s` | max 3 chars of string |

**Examples:**
```c
char buf[64];
snprintf(buf, sizeof(buf), "pid=%d addr=0x%016llx", pid, addr);
snprintf(buf, sizeof(buf), "[%-15s] %08x", name, value);
```

### Callback-based printf (no buffer)

```c
typedef void (*slibc_putc_fn)(void *ctx, char c);
int slibc_vprintf_cb(slibc_putc_fn cb, void *ctx, const char *fmt, va_list ap);
```

Used internally by `kprintf` to write directly to VGA without an intermediate buffer.

---

## 8. Integer-to-String Helpers

```c
int slibc_u64_to_dec(uint64_t v, char *buf);  // buf >= 20 bytes
int slibc_u64_to_hex(uint64_t v, char *buf);  // buf >= 17 bytes, lowercase
int slibc_u64_to_HEX(uint64_t v, char *buf);  // buf >= 17 bytes, uppercase
```

Return the number of characters written (not including NUL).

```c
char *slibc_itoa(int64_t  v, char *buf);   // decimal, buf >= 22
char *slibc_utoa(uint64_t v, char *buf);   // decimal, buf >= 22
char *slibc_xtoa(uint64_t v, char *buf);   // lowercase hex, no "0x" prefix
char *slibc_Xtoa(uint64_t v, char *buf);   // uppercase hex
char *slibc_otoa(uint64_t v, char *buf);   // octal
char *slibc_btoa(uint64_t v, char *buf);   // binary, buf >= 65
```

All return `buf` for convenience.

---

## 9. Sorting & Searching

```c
void  qsort  (void *base, size_t nmemb, size_t size,
               int (*cmp)(const void *, const void *));

void *bsearch(const void *key, const void *base,
               size_t nmemb, size_t size,
               int (*cmp)(const void *, const void *));
```

`cmp` must return `< 0`, `0`, or `> 0`. `bsearch` requires a sorted array; returns `NULL` if not found.

**Example:**
```c
int cmp_int(const void *a, const void *b) {
    return *(int*)a - *(int*)b;
}
int arr[] = {5, 2, 8, 1, 9};
qsort(arr, 5, sizeof(int), cmp_int);
int key = 8;
int *found = bsearch(&key, arr, 5, sizeof(int), cmp_int);
```

---

## 10. setjmp / longjmp

```c
typedef unsigned long long jmp_buf[8];

int  setjmp (jmp_buf env);                    // returns 0 on first call
void longjmp(jmp_buf env, int val) __noreturn; // jumps back to setjmp site, returns val
```

`longjmp(env, 0)` actually returns 1 (zero is reserved for the initial `setjmp` return).

**Example:**
```c
jmp_buf rescue;
if (setjmp(rescue) != 0) {
    kprintf("recovered from error\n");
    return;
}
risky_operation();  // can call longjmp(rescue, 1) on error
```

---

## 11. Integer Math & Bit Operations

### Power / Log

```c
uint64_t slibc_pow_u64(uint64_t base, uint32_t exp);
int64_t  slibc_pow_i64(int64_t  base, uint32_t exp);
int      slibc_log2_u64(uint64_t v);    // floor(log2(v)), -1 if v==0
int      slibc_log10_u64(uint64_t v);   // floor(log10(v)), -1 if v==0
```

### Rounding to Powers of Two

```c
uint64_t slibc_round_up_pow2  (uint64_t v);  // next power of 2 >= v
uint64_t slibc_round_down_pow2(uint64_t v);  // largest power of 2 <= v
```

### Bit Operations

```c
int      slibc_popcount    (uint64_t v);      // count set bits
int      slibc_clz         (uint64_t v);      // count leading zeros
int      slibc_ctz         (uint64_t v);      // count trailing zeros
int      slibc_parity      (uint64_t v);      // 1 if odd number of set bits
uint64_t slibc_reverse_bits(uint64_t v);      // bit-reverse 64-bit word
uint8_t  slibc_reverse_byte(uint8_t v);       // bit-reverse a byte
uint64_t slibc_rotl64      (uint64_t v, int n);
uint64_t slibc_rotr64      (uint64_t v, int n);
uint32_t slibc_rotl32      (uint32_t v, int n);
uint32_t slibc_rotr32      (uint32_t v, int n);
```

### Byte Swap (Endian)

```c
uint16_t slibc_bswap16(uint16_t v);
uint32_t slibc_bswap32(uint32_t v);
uint64_t slibc_bswap64(uint64_t v);
```

**Endian macros** (assumes little-endian host):

```c
slibc_htobe16(x)  slibc_be16toh(x)   // host ↔ big-endian 16
slibc_htole16(x)  slibc_le16toh(x)   // host ↔ little-endian 16
slibc_htobe32(x)  slibc_be32toh(x)   // 32-bit variants
slibc_htobe64(x)  slibc_be64toh(x)   // 64-bit variants
```

### Overflow-Safe Arithmetic

```c
int slibc_add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out); // 1=overflow
int slibc_mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *out);
int slibc_add_overflow_i64(int64_t  a, int64_t  b, int64_t  *out);
int slibc_mul_overflow_i64(int64_t  a, int64_t  b, int64_t  *out);
```

### Saturating Arithmetic

```c
uint64_t slibc_sat_add_u64(uint64_t a, uint64_t b); // clamps at UINT64_MAX
uint64_t slibc_sat_sub_u64(uint64_t a, uint64_t b); // clamps at 0
int64_t  slibc_sat_add_i64(int64_t  a, int64_t  b);
int64_t  slibc_sat_sub_i64(int64_t  a, int64_t  b);
```

### GCD / LCM / Division

```c
uint64_t slibc_gcd(uint64_t a, uint64_t b);
uint64_t slibc_lcm(uint64_t a, uint64_t b);  // returns 0 on overflow

int64_t  slibc_div_round_up (int64_t  n, int64_t  d);  // ceil(n/d), d > 0
uint64_t slibc_udiv_round_up(uint64_t n, uint64_t d);
```

---

## 12. String Extras

### Duplication (needs malloc)

```c
char *slibc_strdup (const char *s);
char *slibc_strndup(const char *s, size_t n);
```

### In-place Transformation

```c
void  slibc_strupr  (char *s);   // uppercase in-place
void  slibc_strlwr  (char *s);   // lowercase in-place
void  slibc_strrev  (char *s);   // reverse in-place
void  slibc_strtrim (char *s);   // strip leading + trailing whitespace
char *slibc_ltrim   (char *s);   // return pointer past leading whitespace (non-destructive)
void  slibc_rtrim   (char *s);   // strip trailing whitespace in-place
```

### Predicates

```c
int slibc_str_starts_with(const char *s, const char *prefix);  // 1 or 0
int slibc_str_ends_with  (const char *s, const char *suffix);
int slibc_str_is_empty   (const char *s);   // 1 if NULL or ""
int slibc_str_is_int     (const char *s);   // 1 if valid signed decimal
int slibc_str_is_uint    (const char *s);   // 1 if valid unsigned decimal
```

### Search / Replace / Split / Join

```c
size_t slibc_str_count  (const char *haystack, const char *needle);

// Replace first or all occurrences of from→to in src, write to buf
int slibc_str_replace(const char *src, const char *from,
                      const char *to, char *buf, size_t bufsz);

// Split src by delimiter (modifies src in-place with NULs)
// Returns number of parts found
int slibc_str_split(char *src, char delim,
                    char **out_parts, int max_parts);

// Join parts array with sep separator into buf
int slibc_str_join(const char **parts, int count,
                   char sep, char *buf, size_t bufsz);
```

**Split example:**
```c
char line[] = "alpha,beta,gamma";
char *parts[8];
int n = slibc_str_split(line, ',', parts, 8);  // n == 3
// parts[0]="alpha", parts[1]="beta", parts[2]="gamma"
```

### Hex Encode / Decode

```c
void slibc_hex_encode(const uint8_t *src, size_t srclen, char *out);
// out must be 2*srclen+1 bytes

int  slibc_hex_decode(const char *hex, uint8_t *out, size_t outlen);
// returns bytes written, -1 on bad input
```

---

## 13. Hashing

```c
// FNV-1a (fast, non-cryptographic)
uint32_t slibc_fnv1a32(const void *data, size_t len);
uint64_t slibc_fnv1a64(const void *data, size_t len);

// DJB2 (simple string hash)
uint32_t slibc_djb2(const char *s);

// MurmurHash3 (32-bit output)
uint32_t slibc_murmur3_32(const void *data, size_t len, uint32_t seed);

// CRC-32 (ISO 3309 / Ethernet polynomial)
uint32_t slibc_crc32       (const void *data, size_t len);
uint32_t slibc_crc32_update(uint32_t crc, const void *data, size_t len);

// Adler-32 (zlib compatible)
uint32_t slibc_adler32       (const void *data, size_t len);
uint32_t slibc_adler32_update(uint32_t adler, const void *data, size_t len);
```

**CRC-32 streaming example:**
```c
uint32_t crc = slibc_crc32_update(0, chunk1, len1);
crc = slibc_crc32_update(crc, chunk2, len2);
```

---

## 14. Random Number Generation

### RNG State Type

```c
typedef struct { uint64_t state; } SRng;
```

### Per-instance API

```c
void     slibc_rng_seed   (SRng *r, uint64_t seed);
uint64_t slibc_rng_next   (SRng *r);                        // raw 64-bit
uint32_t slibc_rng_u32    (SRng *r);                        // uniform 32-bit
uint64_t slibc_rng_range  (SRng *r, uint64_t lo, uint64_t hi); // [lo, hi)
void     slibc_rng_fill   (SRng *r, void *buf, size_t n);   // random bytes
void     slibc_rng_shuffle(SRng *r, void *base, size_t nmemb, size_t sz); // Fisher-Yates
```

### Global convenience (not thread-safe)

```c
void     slibc_srand     (uint64_t seed);
uint64_t slibc_rand      (void);
uint64_t slibc_rand_range(uint64_t lo, uint64_t hi);
```

**Example:**
```c
SRng rng;
slibc_rng_seed(&rng, 0xDEADBEEF);
uint64_t roll = slibc_rng_range(&rng, 1, 7);  // dice roll [1,6]
slibc_rng_shuffle(&rng, arr, 52, sizeof(Card)); // shuffle deck
```

---

## 15. Ring Buffer (SRing)

Single-producer / single-consumer, lock-free on x86. Capacity must be a power of 2.

```c
typedef struct {
    uint8_t  *buf;
    uint32_t  cap;    // must be power of 2
    uint32_t  esize;  // element size in bytes
    uint32_t  head;
    uint32_t  tail;
} SRing;
```

```c
void slibc_ring_init (SRing *r, void *buf, uint32_t cap, uint32_t elem_size);
int  slibc_ring_push (SRing *r, const void *elem); // 0=ok, -1=full
int  slibc_ring_pop  (SRing *r, void *elem);        // 0=ok, -1=empty
int  slibc_ring_peek (const SRing *r, void *elem);  // read without consuming
void slibc_ring_clear(SRing *r);

// Inline helpers
int      slibc_ring_empty(const SRing *r);
int      slibc_ring_full (const SRing *r);
uint32_t slibc_ring_count(const SRing *r);
```

**Example:**
```c
uint8_t storage[4096];  // 64 elements × 64 bytes
SRing ring;
slibc_ring_init(&ring, storage, 64, 64);

MyEvent ev = { .type = EV_KEY, .code = 'A' };
slibc_ring_push(&ring, &ev);

MyEvent out;
if (slibc_ring_pop(&ring, &out) == 0) { /* process out */ }
```

---

## 16. Bitmap (SBitmap)

Compact bit array. Backing storage is a caller-supplied `uint64_t` array.

```c
typedef struct {
    uint64_t *words;
    size_t    nbits;
} SBitmap;
```

```c
// words must be an array of ceil(nbits/64) uint64_t values
void   slibc_bm_init  (SBitmap *bm, uint64_t *words, size_t nbits);
void   slibc_bm_set   (SBitmap *bm, size_t bit);
void   slibc_bm_clear (SBitmap *bm, size_t bit);
int    slibc_bm_test  (const SBitmap *bm, size_t bit);   // 0 or 1
void   slibc_bm_toggle(SBitmap *bm, size_t bit);
void   slibc_bm_zero  (SBitmap *bm);   // clear all
void   slibc_bm_fill  (SBitmap *bm);   // set all

// Find first set/clear bit at or after 'start'. Returns nbits if none found.
size_t slibc_bm_first_set  (const SBitmap *bm, size_t start);
size_t slibc_bm_first_clear(const SBitmap *bm, size_t start);
size_t slibc_bm_count_set  (const SBitmap *bm);
```

**Example:**
```c
uint64_t words[4];  // 256 bits
SBitmap bm;
slibc_bm_init(&bm, words, 256);
slibc_bm_set(&bm, 42);
size_t next_free = slibc_bm_first_clear(&bm, 0);
```

---

## 17. Formatting Helpers

```c
// "1.23 MB" — buf must be >= 16 bytes
void slibc_fmt_bytes(uint64_t bytes, char *buf, size_t bufsz);

// "2h 03m 47s" or "347ms" — buf must be >= 24 bytes
void slibc_fmt_duration_ms(uint64_t ms, char *buf, size_t bufsz);

// Zero-pad integer: slibc_fmt_zpad(42, 5, buf) → "00042"
void slibc_fmt_zpad(uint64_t v, int width, char *buf);

// IPv4 from 32-bit big-endian word: "192.168.1.1" — buf >= 16 bytes
void slibc_fmt_ipv4(uint32_t ip_be, char *buf);

// MAC from 6-byte array: "aa:bb:cc:dd:ee:ff" — buf >= 18 bytes
void slibc_fmt_mac(const uint8_t mac[6], char *buf);

// Parse "192.168.1.1" → big-endian u32. Returns 0 ok, -1 error.
int  slibc_parse_ipv4(const char *s, uint32_t *out_be);
```

### Checksums

```c
// Internet checksum (RFC 1071) — used by IP/TCP/UDP headers
uint16_t slibc_inet_checksum       (const void *data, size_t len);
uint16_t slibc_inet_checksum_update(uint16_t acc, const void *data, size_t len);

// Simple XOR checksum
uint8_t slibc_xor_checksum(const void *data, size_t len);
```

---

## 18. Sorting Extras

```c
// Stable merge sort — O(n log n)
void slibc_msort(void *base, size_t nmemb, size_t sz,
                 int (*cmp)(const void *, const void *));

// Insertion sort — best for n < 16, in-place, stable
void slibc_isort(void *base, size_t nmemb, size_t sz,
                 int (*cmp)(const void *, const void *));
```

---

## 19. Memory Pool (SPool)

Fixed-size slab allocator — no fragmentation, O(1) alloc/free.

```c
typedef struct SPool {
    uint8_t *mem;
    size_t   obj_size;
    size_t   capacity;
    size_t   free_head;
} SPool;
```

```c
// mem must be obj_size * capacity bytes, aligned to 8
void  slibc_pool_init (SPool *p, void *mem, size_t obj_size, size_t capacity);
void *slibc_pool_alloc(SPool *p);           // NULL if full
void  slibc_pool_free (SPool *p, void *obj);
void  slibc_pool_reset(SPool *p);           // free all at once
size_t slibc_pool_used(const SPool *p);     // number of allocated objects
```

**Example:**
```c
static uint8_t  pool_mem[64 * 128];  // 64 objects of 128 bytes
static SPool    pool;

slibc_pool_init(&pool, pool_mem, 128, 64);

MyThing *t = slibc_pool_alloc(&pool);
// ... use t ...
slibc_pool_free(&pool, t);
```

---

## 20. Config Parser (SCfg)

Parses `key=value` text files. Lines starting with `#` are comments.

```c
#define SLIBC_CFG_MAX_KEY   64
#define SLIBC_CFG_MAX_VAL   256
#define SLIBC_CFG_MAX_PAIRS 128

typedef struct { char key[64]; char val[256]; } SCfgPair;
typedef struct { SCfgPair pairs[128]; int count; } SCfg;
```

```c
int         slibc_cfg_parse  (SCfg *cfg, const char *text);
const char *slibc_cfg_get    (const SCfg *cfg, const char *key);
const char *slibc_cfg_get_or (const SCfg *cfg, const char *key, const char *fallback);
int         slibc_cfg_get_int(const SCfg *cfg, const char *key, int fallback);
```

**Example:**
```c
const char *text = "width=800\nheight=600\n# comment\nfullscreen=1\n";
SCfg cfg;
slibc_cfg_parse(&cfg, text);
int w = slibc_cfg_get_int(&cfg, "width", 1024);   // 800
const char *mode = slibc_cfg_get_or(&cfg, "mode", "windowed");
```

---

## 21. Base64

```c
// Encode — out must be >= (len+2)/3*4 + 1 bytes
size_t slibc_base64_encode(const uint8_t *src, size_t len, char *out);

// Decode — out must be >= len*3/4 bytes
// Returns decoded byte count, or -1 on bad input
ssize_t slibc_base64_decode(const char *src, size_t len, uint8_t *out);
```

---

## 22. UUID (SUUID)

Version 4 (random) UUID.

```c
typedef struct { uint8_t b[16]; } SUUID;

void slibc_uuid4   (SRng *rng, SUUID *out);
void slibc_uuid_str(const SUUID *u, char buf[37]);  // "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"
```

**Example:**
```c
SRng rng;
slibc_rng_seed(&rng, tsc_read());
SUUID id;
slibc_uuid4(&rng, &id);
char str[37];
slibc_uuid_str(&id, str);
kprintf("id = %s\n", str);
```

---

## 23. Debug Utilities

### Static Assert

```c
SLIBC_STATIC_ASSERT(sizeof(MyStruct) == 16, "MyStruct must be 16 bytes");
```

Compile-time failure if condition is false. Works in C99 and C11.

### container_of

Get the outer struct from a pointer to a member:

```c
#define slibc_container_of(ptr, type, member)

// Example:
typedef struct { int x; SListNode node; } MyItem;
SListNode *n = ...;
MyItem *item = slibc_container_of(n, MyItem, node);
```

### Branch Hints

```c
slibc_likely(x)    // hint: x is usually true
slibc_unlikely(x)  // hint: x is usually false
```

### Other

```c
slibc_barrier()      // compiler memory barrier (no CPU fence)
slibc_unreachable()  // marks unreachable code path
```

---

## 24. Linked List (SListNode)

Intrusive doubly-linked circular list. Embed `SListNode` in your own struct.

```c
typedef struct SListNode { struct SListNode *prev, *next; } SListNode;
```

```c
void       slibc_list_init      (SListNode *head);
int        slibc_list_empty     (const SListNode *head);
void       slibc_list_push_back (SListNode *head, SListNode *node);
void       slibc_list_push_front(SListNode *head, SListNode *node);
void       slibc_list_remove    (SListNode *node);
SListNode *slibc_list_pop_front (SListNode *head);  // NULL if empty
SListNode *slibc_list_pop_back  (SListNode *head);  // NULL if empty
size_t     slibc_list_count     (const SListNode *head);
```

### Iteration Macros

```c
// Basic iteration (do NOT remove nodes during this loop)
slibc_list_foreach(node, &head) {
    MyItem *item = slibc_container_of(node, MyItem, list_node);
}

// Safe iteration (allows removing the current node)
slibc_list_foreach_safe(node, tmp, &head) {
    MyItem *item = slibc_container_of(node, MyItem, list_node);
    if (should_remove(item)) slibc_list_remove(node);
}
```

**Full example:**
```c
typedef struct { int val; SListNode node; } MyItem;

SListNode head;
slibc_list_init(&head);

MyItem a = { .val = 1 }, b = { .val = 2 };
slibc_list_push_back(&head, &a.node);
slibc_list_push_back(&head, &b.node);

slibc_list_foreach(n, &head) {
    MyItem *it = slibc_container_of(n, MyItem, node);
    kprintf("val = %d\n", it->val);
}
```

---

## 25. Error Codes

All error codes are **negative `long long`** values.

| Code | Value | Meaning |
|------|-------|---------|
| `EPERM` | -1 | Operation not permitted |
| `ENOENT` | -2 | No such file or directory |
| `ESRCH` | -3 | No such process |
| `EINTR` | -4 | Interrupted system call |
| `EIO` | -5 | I/O error |
| `EBADF` | -9 | Bad file descriptor |
| `ECHILD` | -10 | No child processes |
| `EAGAIN` | -11 | Try again |
| `EWOULDBLOCK` | -11 | Same as EAGAIN |
| `ENOMEM` | -12 | Out of memory |
| `EACCES` | -13 | Permission denied |
| `EFAULT` | -14 | Bad address |
| `EBUSY` | -16 | Device busy |
| `EEXIST` | -17 | File exists |
| `ENODEV` | -19 | No such device |
| `ENOTDIR` | -20 | Not a directory |
| `EISDIR` | -21 | Is a directory |
| `EINVAL` | -22 | Invalid argument |
| `ENFILE` | -23 | File table overflow |
| `EMFILE` | -24 | Too many open files |
| `ENOSPC` | -28 | No space left on device |
| `EROFS` | -30 | Read-only file system |
| `EPIPE` | -32 | Broken pipe |
| `ERANGE` | -34 | Result out of range |
| `ENAMETOOLONG` | -36 | File name too long |
| `ENOSYS` | -38 | Function not implemented |
| `ENOTEMPTY` | -39 | Directory not empty |
| `EOVERFLOW` | -75 | Value too large |
| `EOPNOTSUPP` | -95 | Operation not supported |
| `ENETUNREACH` | -101 | Network unreachable |
| `EISCONN` | -106 | Already connected |
| `ENOTCONN` | -107 | Not connected |
| `ETIMEDOUT` | -110 | Connection timed out |
| `ECONNREFUSED` | -111 | Connection refused |
| `EALREADY` | -114 | Operation already in progress |
| `EINPROGRESS` | -115 | Operation now in progress |

**Usage pattern:**
```c
i64 fd = vfs_open(path);
if (fd < 0) {
    kprintf("open: %s\n", strerror((int)fd));
    return fd;
}
```

---

## 26. strerror

```c
const char *strerror(int errnum);
```

Converts a Systrix error code (positive or negative) to a string.
Returns a pointer to a **string literal** — do not free or modify it.
Unknown codes return `"Unknown error"`.

```c
kprintf("%s\n", strerror(ENOENT));    // "No such file or directory"
kprintf("%s\n", strerror(-12));       // "Out of memory"
kprintf("%s\n", strerror((int)ret));  // works with any i64 return value
```

---

## 27. Path Utilities

All path functions treat `/` as the separator. No heap is used — all work on caller-supplied stack buffers.

```c
// Join base + name into dst. If name is absolute, replaces base entirely.
// Returns 0 on success, -1 if result would be truncated.
int slibc_path_join(char *dst, size_t dst_sz, const char *base, const char *name);

// Normalize a path in-place:
//   /foo//bar    → /foo/bar
//   /foo/./bar   → /foo/bar
//   /foo/bar/../baz → /foo/baz
// Returns path pointer for convenience.
char *slibc_path_normalize(char *path);

// Return pointer to last component (no allocation, no modification).
//   "/foo/bar"  → "bar"
//   "/foo/bar/" → "bar"
//   "/"         → "/"
const char *slibc_path_basename(const char *path);

// Copy directory portion into dst.
//   "/foo/bar"  → "/foo"
//   "/foo"      → "/"
//   "foo"       → "."
// Returns 0 on success, -1 if truncated.
int slibc_path_dirname(const char *path, char *dst, size_t dst_sz);

// Inline: 1 if path starts with '/', else 0
int slibc_path_is_absolute(const char *path);

// Case-insensitive extension check
// slibc_path_has_ext("README.MD", "md") → 1
int slibc_path_has_ext(const char *path, const char *ext);
```

**Examples:**
```c
char out[256];
slibc_path_join(out, sizeof(out), "/home/user", "docs/file.txt");
// out = "/home/user/docs/file.txt"

slibc_path_join(out, sizeof(out), "/home/user", "/etc/passwd");
// out = "/etc/passwd"  (absolute name replaces base)

char path[] = "/foo/bar/../baz";
slibc_path_normalize(path);
// path = "/foo/baz"

const char *base = slibc_path_basename("/foo/bar.txt");
// base = "bar.txt"

slibc_path_dirname("/foo/bar.txt", out, sizeof(out));
// out = "/foo"
```

---

## 28. String Padding

```c
// Fill buf with 'count' copies of character c, NUL-terminate.
// buf must be count+1 bytes. Returns count.
size_t slibc_str_repeat(char *buf, char c, size_t count);

// Pad str to fixed width into dst.
// left_align=0: right-align (leading pad chars)
// left_align≠0: left-align  (trailing pad chars)
// Returns number of characters written.
size_t slibc_str_pad(char *dst, size_t dst_sz,
                     const char *str, size_t width,
                     char pad_char, int left_align);

// Write a progress bar into buf: "[=====>   ]"
// buf must be >= total_width + 3 bytes
void slibc_fmt_progress(char *buf, size_t total_width,
                        uint64_t numerator, uint64_t denominator);
```

**Examples:**
```c
char pad[16];
slibc_str_repeat(pad, ' ', 10);         // "          "
slibc_str_repeat(pad, '-', 10);         // "----------"

char col[20];
slibc_str_pad(col, sizeof(col), "hello", 10, ' ', 1); // "hello     " (left)
slibc_str_pad(col, sizeof(col), "hello", 10, ' ', 0); // "     hello" (right)
slibc_str_pad(col, sizeof(col), "42",    6,  '0', 0); // "    42"

char bar[32];
slibc_fmt_progress(bar, 20, 3, 4);   // "[===============>    ]" (75%)
```

---

## 29. Dynamic String Builder (SStrBuf)

Growable string — requires `malloc`/`realloc`/`free`. Use in user space or kernel code that provides those.

```c
typedef struct {
    char   *data;   // NUL-terminated string (NULL until first append)
    size_t  len;    // current length (not counting NUL)
    size_t  cap;    // allocated capacity
} SStrBuf;
```

```c
void  slibc_sb_init   (SStrBuf *sb);
void  slibc_sb_free   (SStrBuf *sb);
void  slibc_sb_reset  (SStrBuf *sb);            // clear, keep allocation

int   slibc_sb_append (SStrBuf *sb, const char *s);
int   slibc_sb_appendn(SStrBuf *sb, const char *s, size_t n);
int   slibc_sb_appendc(SStrBuf *sb, char c);
int   slibc_sb_appendf(SStrBuf *sb, const char *fmt, ...); // printf-style

// All append functions return 0 on success, -1 on allocation failure.

char *slibc_sb_steal(SStrBuf *sb); // caller owns the returned buffer; sb is reset
```

**Example:**
```c
SStrBuf sb;
slibc_sb_init(&sb);

slibc_sb_append (&sb, "Hello");
slibc_sb_appendc(&sb, ' ');
slibc_sb_appendf(&sb, "pid=%d name=%s", pid, name);

kprintf("%s\n", sb.data);  // "Hello pid=42 name=init"

char *result = slibc_sb_steal(&sb);  // take ownership
// ... use result ...
free(result);

slibc_sb_free(&sb);  // or free if not stolen
```

---

## Quick Cheat Sheet

| Task | Use |
|------|-----|
| Zero a buffer | `memset(buf, 0, size)` |
| Copy bytes | `memcpy(dst, src, n)` |
| Copy overlapping | `memmove(dst, src, n)` |
| Copy string (safe) | `strlcpy(dst, src, sizeof(dst))` |
| Append string (safe) | `strlcat(dst, src, sizeof(dst))` |
| String length | `strlen(s)` |
| Compare strings | `strcmp(a, b) == 0` |
| Find char in string | `strchr(s, c)` |
| Find last char | `strrchr(s, c)` |
| Find substring | `strstr(haystack, needle)` |
| Skip whitespace | `slibc_ltrim(s)` or `while(isspace(*p)) p++` |
| Check digit | `isdigit((unsigned char)c)` |
| String to int | `atoi(s)` or `strtol(s, NULL, 10)` |
| Formatted string | `snprintf(buf, sizeof(buf), "x=%d", x)` |
| Build path | `slibc_path_join(dst, sizeof(dst), base, name)` |
| Clean path | `slibc_path_normalize(path)` |
| Get filename | `slibc_path_basename(path)` |
| Get directory | `slibc_path_dirname(path, dst, sizeof(dst))` |
| Error message | `strerror((int)err_code)` |
| Count set bits | `slibc_popcount(v)` |
| Safe add | `slibc_add_overflow_u64(a, b, &out)` |
| Hash string | `slibc_fnv1a32(s, strlen(s))` |
| Format bytes | `slibc_fmt_bytes(n, buf, sizeof(buf))` |
| Pad column | `slibc_str_pad(dst, sz, str, width, ' ', 1)` |
| Sort array | `qsort(arr, n, sizeof(T), cmp)` |
| Binary search | `bsearch(&key, arr, n, sizeof(T), cmp)` |
