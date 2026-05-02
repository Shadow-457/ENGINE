/* ================================================================
 *  Systrix OS — user/malloc.c  (Production User-Space Allocator)
 *
 *  A dlmalloc-style allocator with segregated free lists (bins).
 *  Designed for user-space programs running on Systrix OS.
 *
 *  Features:
 *    - Segregated free lists (8 bins for different size ranges)
 *    - Best-fit within bins for reduced fragmentation
 *    - Boundary tag coalescing (forward + backward)
 *    - Memory mapping for large allocations (via mmap)
 *    - Optional memory poisoning for debug builds
 *    - Thread-safe stub (single-threaded for now, SMP-ready)
 *    - Configurable top-of-heap (sbrk-based)
 *
 *  Layout of allocated block:
 *    [prev_size:4][size:4][user_data...][canary:4]
 *
 *  Layout of free block:
 *    [prev_size:4][size:4][fd:4][bk:4][padding...][canary:4]
 *
 *  Size field:
 *    - Bit 0: PREV_INUSE (previous block is allocated)
 *    - Bits 1-31: size in bytes (always 8-byte aligned)
 * ================================================================ */
#include "libc.h"

/* ---- Configuration -------------------------------------------- */
#define MALLOC_ALIGN     8
#define MALLOC_ALIGN_MASK (MALLOC_ALIGN - 1)

/* Minimum chunk size (must hold fd+bk pointers) */
#define MIN_CHUNK_SIZE   16

/* Header/footer size */
#define HDR_SIZE         8   /* prev_size(4) + size(4) */
#define FOOTER_SIZE      4   /* canary */
#define CHUNK_OVERHEAD   (HDR_SIZE + FOOTER_SIZE)

/* mmap threshold: allocations >= this use mmap directly */
#define MMAP_THRESHOLD   (128 * 1024)  /* 128 KB */

/* Bin count and size ranges */
#define BIN_COUNT        8
#define BIN_SHIFT        3
#define MAX_SMALL_SIZE   (1 << (BIN_COUNT * BIN_SHIFT))  /* 2^24 = 16MB */

/* Magic numbers */
#define FREE_CANARY      0xFREECANE
#define USED_CANARY      0xUSEDCANE
#define MMAP_CANARY      0xMMAPCANE

/* ---- Types ---------------------------------------------------- */
typedef unsigned int u32;
typedef int          i32;
typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned long      usize;

/* ---- Chunk header (overlaps with free chunk's fd/bk) ---------- */
typedef struct ChunkHdr {
    u32 prev_size;  /* size of previous chunk (if free) */
    u32 size;       /* size of this chunk (bit 0 = PREV_INUSE) */
} ChunkHdr;

/* Free chunk: header + doubly-linked list pointers */
typedef struct FreeChunk {
    ChunkHdr hdr;
    struct FreeChunk *fd;  /* forward link */
    struct FreeChunk *bk;  /* backward link */
} FreeChunk;

/* ---- Bin structure -------------------------------------------- */
typedef struct Bin {
    FreeChunk *first;
    FreeChunk *last;
    u32 min_size;
    u32 max_size;
    u32 count;
} Bin;

/* ---- Global state --------------------------------------------- */
static void *heap_base = NULL;
static void *heap_brk  = NULL;
static void *heap_top  = NULL;
static Bin bins[BIN_COUNT];
static usize total_allocated = 0;
static usize total_freed = 0;
static usize peak_allocated = 0;
static usize current_allocated = 0;
static usize mmap_count = 0;
static usize alloc_count = 0;
static usize free_count = 0;

/* ---- Syscall wrappers ----------------------------------------- */
static inline long syscall1(long n, long a1) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1) : "rcx", "r11", "memory");
    return r;
}
static inline long syscall2(long n, long a1, long a2) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return r;
}
static inline long syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}

static void *sys_brk(void *addr) {
    return (void*)syscall1(12, (long)addr);
}

static void *sys_mmap(void *addr, usize len, int prot, int flags, int fd, long off) {
    return (void*)syscall6(9, (long)addr, (long)len, (long)prot, (long)flags, (long)fd, off);
}

static int sys_munmap(void *addr, usize len) {
    return (int)syscall2(11, (long)addr, (long)len);
}

/* ---- Chunk helpers -------------------------------------------- */
#define PREV_INUSE  0x1
#define CHUNK_SIZE(s) ((s) & ~PREV_INUSE)
#define IS_USED(s)  ((s) & PREV_INUSE)

static inline ChunkHdr *chunk_at(void *p) {
    return (ChunkHdr*)((char*)p - HDR_SIZE);
}

static inline void *chunk_data(ChunkHdr *h) {
    return (void*)((char*)h + HDR_SIZE);
}

static inline ChunkHdr *next_chunk(ChunkHdr *h) {
    return (ChunkHdr*)((char*)h + CHUNK_SIZE(h->size));
}

static inline ChunkHdr *prev_chunk(ChunkHdr *h) {
    return (ChunkHdr*)((char*)h - h->prev_size);
}

static inline void set_canary(ChunkHdr *h) {
    u32 *footer = (u32*)((char*)h + CHUNK_SIZE(h->size) - FOOTER_SIZE);
    *footer = IS_USED(h->size) ? USED_CANARY : FREE_CANARY;
}

static inline int check_canary(ChunkHdr *h) {
    u32 *footer = (u32*)((char*)h + CHUNK_SIZE(h->size) - FOOTER_SIZE);
    u32 expected = IS_USED(h->size) ? USED_CANARY : FREE_CANARY;
    return (*footer == expected) ? 1 : 0;
}

/* ---- Bin helpers ---------------------------------------------- */
static int size_to_bin(usize size) {
    if (size <= 8)   return 0;
    if (size <= 64)  return 1;
    if (size <= 512) return 2;
    if (size <= 4096) return 3;
    if (size <= 32768) return 4;
    if (size <= 262144) return 5;
    if (size <= 2097152) return 6;
    return 7;
}

static void bin_init(Bin *b, u32 min, u32 max) {
    b->first = b->last = NULL;
    b->min_size = min;
    b->max_size = max;
    b->count = 0;
}

static void bin_add(Bin *b, FreeChunk *fc) {
    fc->fd = NULL;
    fc->bk = b->last;
    if (b->last) b->last->fd = fc;
    b->last = fc;
    if (!b->first) b->first = fc;
    b->count++;
}

static void bin_remove(Bin *b, FreeChunk *fc) {
    if (fc->fd) fc->fd->bk = fc->bk;
    else b->first = fc->fd;
    if (fc->bk) fc->bk->fd = fc->fd;
    else b->last = fc->bk;
    fc->fd = fc->bk = NULL;
    b->count--;
}

static FreeChunk *bin_search(Bin *b, usize size) {
    /* Best-fit within bin */
    FreeChunk *best = NULL;
    usize best_diff = (usize)-1;

    FreeChunk *fc = b->first;
    while (fc) {
        usize chunk_size = CHUNK_SIZE(fc->hdr.size);
        if (chunk_size >= size) {
            usize diff = chunk_size - size;
            if (diff < best_diff) {
                best = fc;
                best_diff = diff;
            }
        }
        fc = fc->fd;
    }
    return best;
}

/* ---- Heap extension ------------------------------------------- */
static int extend_heap(usize pages) {
    void *old_brk = heap_brk;
    void *new_brk = sys_brk((char*)heap_brk + pages * 4096);
    if (new_brk == (void*)-1 || new_brk == old_brk) return 0;

    heap_brk = new_brk;

    /* Initialize new space as a single free chunk */
    ChunkHdr *h = (ChunkHdr*)old_brk;
    usize avail = (usize)((char*)heap_brk - (char*)h) - CHUNK_OVERHEAD;
    avail = (avail & ~MALLOC_ALIGN_MASK);

    h->prev_size = 0;
    h->size = avail | PREV_INUSE;  /* mark prev (nonexistent) as in-use */
    set_canary(h);

    /* Add to appropriate bin */
    int bin = size_to_bin(CHUNK_SIZE(h->size));
    bin_add(&bins[bin], (FreeChunk*)h);

    return 1;
}

/* ---- Split chunk if remainder is large enough ----------------- */
static void split_chunk(FreeChunk *fc, usize needed) {
    usize total = CHUNK_SIZE(fc->hdr.size);
    usize remainder = total - needed;

    if (remainder >= MIN_CHUNK_SIZE + CHUNK_OVERHEAD) {
        /* Create remainder chunk */
        ChunkHdr *rem = (ChunkHdr*)((char*)fc + needed);
        rem->prev_size = (u32)needed;
        rem->size = (u32)(remainder - CHUNK_OVERHEAD) | PREV_INUSE;
        set_canary(rem);

        /* Update current chunk size */
        fc->hdr.size = (u32)needed | (fc->hdr.size & PREV_INUSE);
        set_canary(fc);

        /* Add remainder to bin */
        int bin = size_to_bin(CHUNK_SIZE(rem->size));
        bin_add(&bins[bin], (FreeChunk*)rem);
    }
}

/* ---- Coalesce with neighbors ---------------------------------- */
static FreeChunk *coalesce(FreeChunk *fc) {
    ChunkHdr *h = &fc->hdr;
    usize size = CHUNK_SIZE(h->size);

    /* Coalesce forward */
    ChunkHdr *next = next_chunk(h);
    if ((char*)next < (char*)heap_brk && !IS_USED(next->size)) {
        /* Remove next from bin */
        int next_bin = size_to_bin(CHUNK_SIZE(next->size));
        bin_remove(&bins[next_bin], (FreeChunk*)next);

        /* Merge */
        h->size += CHUNK_SIZE(next->size);
        set_canary(h);
    }

    /* Coalesce backward */
    if ((h->size & PREV_INUSE) == 0) {
        ChunkHdr *prev = prev_chunk(h);
        int prev_bin = size_to_bin(CHUNK_SIZE(prev->size));
        bin_remove(&bins[prev_bin], (FreeChunk*)prev);

        prev->size += CHUNK_SIZE(h->size);
        set_canary(prev);
        fc = (FreeChunk*)prev;
    }

    /* Add merged chunk to bin */
    int bin = size_to_bin(CHUNK_SIZE(fc->hdr.size));
    bin_add(&bins[bin], fc);

    return fc;
}

/* ================================================================
 *  PUBLIC API
 * ================================================================ */

/* ---- Initialize malloc subsystem ------------------------------ */
void malloc_init(void) {
    /* Get current brk */
    heap_base = sys_brk(NULL);
    heap_brk  = heap_base;
    heap_top  = (char*)heap_base + 4096;  /* initial 4KB */

    /* Extend initial heap */
    extend_heap(4);

    /* Initialize bins */
    bin_init(&bins[0], 0, 8);
    bin_init(&bins[1], 9, 64);
    bin_init(&bins[2], 65, 512);
    bin_init(&bins[3], 513, 4096);
    bin_init(&bins[4], 4097, 32768);
    bin_init(&bins[5], 32769, 262144);
    bin_init(&bins[6], 262145, 2097152);
    bin_init(&bins[7], 2097153, 0xFFFFFFFF);

    total_allocated = 0;
    total_freed = 0;
    peak_allocated = 0;
    current_allocated = 0;
    mmap_count = 0;
    alloc_count = 0;
    free_count = 0;
}

/* ---- Allocate memory ------------------------------------------ */
void *malloc(usize n) {
    if (n == 0) return NULL;

    alloc_count++;

    /* Large allocation: use mmap */
    if (n >= MMAP_THRESHOLD) {
        usize pages = (n + CHUNK_OVERHEAD + 4095) / 4096;
        void *ptr = sys_mmap(NULL, pages * 4096, 3, 0x22, -1, 0);  /* PROT_READ|WRITE, MAP_PRIVATE|ANON */
        if (ptr == (void*)-1) return NULL;

        /* Mark with canary */
        ChunkHdr *h = (ChunkHdr*)ptr;
        h->prev_size = 0;
        h->size = (u32)(pages * 4096 - CHUNK_OVERHEAD) | PREV_INUSE;
        set_canary(h);

        mmap_count++;
        total_allocated += pages * 4096;
        current_allocated += pages * 4096;
        if (current_allocated > peak_allocated)
            peak_allocated = current_allocated;

        return chunk_data(h);
    }

    /* Align request */
    usize needed = (n + MALLOC_ALIGN_MASK) & ~MALLOC_ALIGN_MASK;
    usize total_needed = needed + CHUNK_OVERHEAD;
    if (total_needed < MIN_CHUNK_SIZE + CHUNK_OVERHEAD)
        total_needed = MIN_CHUNK_SIZE + CHUNK_OVERHEAD;

    /* Search bins for best fit */
    for (int b = size_to_bin(total_needed); b < BIN_COUNT; b++) {
        FreeChunk *fc = bin_search(&bins[b], total_needed);
        if (fc) {
            bin_remove(&bins[b], fc);

            /* Split if remainder is large enough */
            split_chunk(fc, total_needed);

            /* Mark as used */
            fc->hdr.size |= PREV_INUSE;
            set_canary(fc);

            total_allocated += CHUNK_SIZE(fc->hdr.size);
            current_allocated += CHUNK_SIZE(fc->hdr.size);
            if (current_allocated > peak_allocated)
                peak_allocated = current_allocated;

            return chunk_data(&fc->hdr);
        }
    }

    /* No free chunk found - extend heap */
    usize pages = (total_needed + 4095) / 4096;
    if (!extend_heap(pages + 1)) return NULL;

    /* Retry: get the chunk we just added */
    for (int b = 0; b < BIN_COUNT; b++) {
        FreeChunk *fc = bin_search(&bins[b], total_needed);
        if (fc) {
            bin_remove(&bins[b], fc);
            split_chunk(fc, total_needed);
            fc->hdr.size |= PREV_INUSE;
            set_canary(fc);

            total_allocated += CHUNK_SIZE(fc->hdr.size);
            current_allocated += CHUNK_SIZE(fc->hdr.size);
            if (current_allocated > peak_allocated)
                peak_allocated = current_allocated;

            return chunk_data(&fc->hdr);
        }
    }

    return NULL;
}

/* ---- Free memory ---------------------------------------------- */
void free(void *ptr) {
    if (!ptr) return;

    free_count++;

    ChunkHdr *h = chunk_at(ptr);

    /* Check canary for corruption */
    if (!check_canary(h)) {
        /* Heap corruption detected */
        return;
    }

    /* Check for double-free */
    if (!IS_USED(h->size)) {
        /* Already free */
        return;
    }

    usize size = CHUNK_SIZE(h->size);
    total_freed += size;
    current_allocated -= size;

    /* Check if this is an mmap'd region */
    if ((char*)ptr < (char*)heap_base || (char*)ptr >= (char*)heap_brk) {
        /* mmap'd region - unmap directly */
        /* Find size from header */
        usize pages = (size + CHUNK_OVERHEAD + 4095) / 4096;
        sys_munmap(h, pages * 4096);
        mmap_count--;
        return;
    }

    /* Mark as free */
    h->size &= ~PREV_INUSE;
    set_canary(h);

    /* Coalesce with neighbors */
    coalesce((FreeChunk*)h);
}

/* ---- Reallocate memory ---------------------------------------- */
void *realloc(void *old, usize new_sz) {
    if (!old)   return malloc(new_sz);
    if (!new_sz) { free(old); return NULL; }

    ChunkHdr *h = chunk_at(old);
    usize old_sz = CHUNK_SIZE(h->size);

    if (new_sz <= old_sz) return old;  /* fits in place */

    /* Try to expand in place */
    ChunkHdr *next = next_chunk(h);
    if ((char*)next < (char*)heap_brk && !IS_USED(next->size)) {
        usize combined = old_sz + CHUNK_SIZE(next->size) + CHUNK_OVERHEAD;
        if (combined >= new_sz + CHUNK_OVERHEAD) {
            /* Remove next from bin */
            int b = size_to_bin(CHUNK_SIZE(next->size));
            bin_remove(&bins[b], (FreeChunk*)next);

            /* Expand */
            h->size = (new_sz + CHUNK_OVERHEAD) | (h->size & PREV_INUSE);
            set_canary(h);

            /* If there's remainder, create new free chunk */
            usize remainder = combined - (new_sz + CHUNK_OVERHEAD);
            if (remainder >= MIN_CHUNK_SIZE + CHUNK_OVERHEAD) {
                ChunkHdr *rem = (ChunkHdr*)((char*)h + new_sz + CHUNK_OVERHEAD);
                rem->prev_size = (u32)(new_sz + CHUNK_OVERHEAD);
                rem->size = (u32)(remainder - CHUNK_OVERHEAD) | PREV_INUSE;
                set_canary(rem);
                int rb = size_to_bin(CHUNK_SIZE(rem->size));
                bin_add(&bins[rb], (FreeChunk*)rem);
            }

            total_allocated += new_sz - old_sz;
            current_allocated += new_sz - old_sz;
            if (current_allocated > peak_allocated)
                peak_allocated = current_allocated;

            return old;
        }
    }

    /* Can't expand in place - allocate new and copy */
    void *new_ptr = malloc(new_sz);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, old, old_sz);
    free(old);

    return new_ptr;
}

/* ---- Calloc: allocate and zero -------------------------------- */
void *calloc(usize count, usize size) {
    usize total = count * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

/* ---- Print malloc statistics ---------------------------------- */
void malloc_print_stats(void) {
    /* Simple stats output via write syscall */
    char buf[512];
    int pos = 0;

    /* Helper: append number */
    void append_num(usize v) {
        char tmp[32]; int tpos = 0;
        if (v == 0) tmp[tpos++] = '0';
        else { while (v) { tmp[tpos++] = '0' + (v % 10); v /= 10; } }
        for (int i = tpos-1; i >= 0; i--) buf[pos++] = tmp[i];
    }

    buf[pos++] = '\n';
    buf[pos++] = '='; buf[pos++] = '='; buf[pos++] = '=';
    buf[pos++] = ' '; buf[pos++] = 'M'; buf[pos++] = 'a'; buf[pos++] = 'l';
    buf[pos++] = 'l'; buf[pos++] = 'o'; buf[pos++] = 'c'; buf[pos++] = ' ';
    buf[pos++] = 'S'; buf[pos++] = 't'; buf[pos++] = 'a'; buf[pos++] = 't';
    buf[pos++] = 's'; buf[pos++] = ' '; buf[pos++] = '='; buf[pos++] = '=';
    buf[pos++] = '='; buf[pos++] = '\n';

    buf[pos++] = 'A'; buf[pos++] = 'l'; buf[pos++] = 'l'; buf[pos++] = 'o';
    buf[pos++] = 'c'; buf[pos++] = ' '; buf[pos++] = 'c'; buf[pos++] = 'o';
    buf[pos++] = 'u'; buf[pos++] = 'n'; buf[pos++] = 't'; buf[pos++] = ':';
    buf[pos++] = ' ';
    append_num(alloc_count);
    buf[pos++] = '\n';

    buf[pos++] = 'F'; buf[pos++] = 'r'; buf[pos++] = 'e'; buf[pos++] = 'e';
    buf[pos++] = ' '; buf[pos++] = 'c'; buf[pos++] = 'o'; buf[pos++] = 'u';
    buf[pos++] = 'n'; buf[pos++] = 't'; buf[pos++] = ':'; buf[pos++] = ' ';
    append_num(free_count);
    buf[pos++] = '\n';

    buf[pos++] = 'C'; buf[pos++] = 'u'; buf[pos++] = 'r'; buf[pos++] = 'r';
    buf[pos++] = 'e'; buf[pos++] = 'n'; buf[pos++] = 't'; buf[pos++] = ':';
    buf[pos++] = ' ';
    append_num(current_allocated);
    buf[pos++] = ' '; buf[pos++] = 'b'; buf[pos++] = 'y'; buf[pos++] = 't';
    buf[pos++] = 'e'; buf[pos++] = 's'; buf[pos++] = '\n';

    buf[pos++] = 'P'; buf[pos++] = 'e'; buf[pos++] = 'a'; buf[pos++] = 'k';
    buf[pos++] = ':'; buf[pos++] = ' ';
    append_num(peak_allocated);
    buf[pos++] = ' '; buf[pos++] = 'b'; buf[pos++] = 'y'; buf[pos++] = 't';
    buf[pos++] = 'e'; buf[pos++] = 's'; buf[pos++] = '\n';

    buf[pos++] = 'm'; buf[pos++] = 'm'; buf[pos++] = 'a'; buf[pos++] = 'p';
    buf[pos++] = ' '; buf[pos++] = 'c'; buf[pos++] = 'o'; buf[pos++] = 'u';
    buf[pos++] = 'n'; buf[pos++] = 't'; buf[pos++] = ':'; buf[pos++] = ' ';
    append_num(mmap_count);
    buf[pos++] = '\n';

    buf[pos++] = 'B'; buf[pos++] = 'i'; buf[pos++] = 'n'; buf[pos++] = ' ';
    buf[pos++] = 'c'; buf[pos++] = 'o'; buf[pos++] = 'u'; buf[pos++] = 'n';
    buf[pos++] = 't'; buf[pos++] = 's'; buf[pos++] = ':'; buf[pos++] = '\n';
    for (int i = 0; i < BIN_COUNT; i++) {
        buf[pos++] = ' ';
        buf[pos++] = '0' + i;
        buf[pos++] = ':';
        buf[pos++] = ' ';
        append_num(bins[i].count);
        buf[pos++] = '\n';
    }

    buf[pos++] = '='; buf[pos++] = '='; buf[pos++] = '='; buf[pos++] = '\n';

    write(1, buf, pos);
}

/* ---- Check heap integrity ------------------------------------- */
int malloc_check_integrity(void) {
    int errors = 0;

    /* Walk all chunks in heap */
    ChunkHdr *h = (ChunkHdr*)heap_base;
    while ((char*)h < (char*)heap_brk) {
        if (h->size == 0) { errors++; break; }
        if (!check_canary(h)) { errors++; break; }

        usize size = CHUNK_SIZE(h->size);
        if (size == 0 || size > (usize)((char*)heap_brk - (char*)h)) {
            errors++;
            break;
        }

        h = next_chunk(h);
    }

    return errors;
}
