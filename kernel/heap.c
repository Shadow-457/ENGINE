/* ================================================================
 *  Systrix OS — kernel/heap.c  (v2: Slab Allocator)
 *
 *  Replaces the old first-fit free-list with a two-tier system
 *  matching Linux's kmalloc/slab design:
 *
 *  TIER 1 — SLAB CACHES for small, fixed-size allocations
 *  --------------------------------------------------------
 *  Size classes (powers of 2):  16, 32, 64, 128, 256, 512, 1024, 2048
 *
 *  Each slab cache manages a chain of slab pages.  A slab page is
 *  a 4 KB page whose first sizeof(SlabHdr) bytes hold metadata;
 *  the rest is divided into fixed-size slots.
 *
 *  Layout of one slab page:
 *    [ SlabHdr | slot0 | slot1 | ... | slotN ]
 *
 *  Free slots form a singly-linked list through the first 4 bytes
 *  of each slot.  Allocation is O(1); freeing is O(1).
 *
 *  TIER 2 — BLOCK ALLOCATOR for large objects (> SLAB_MAX_SIZE)
 *  ---------------------------------------------------------------
 *  Falls back to the original boundary-tag first-fit heap that
 *  lives in HEAP_BASE..HEAP_BASE+HEAP_SIZE.  Objects here get a
 *  16-byte header: [size:8][flags:8].
 *
 *  heap_malloc(n)  — returns zeroed memory.
 *  heap_free(ptr)  — determines tier from ptr address.
 *  heap_realloc(p,n) — grow/shrink with content copy.
 * ================================================================ */
#include "../include/kernel.h"

/* ================================================================
 *  SLAB TIER
 * ================================================================ */

#define SLAB_NUM_CLASSES  8
static const u32 slab_sizes[SLAB_NUM_CLASSES] = {16,32,64,128,256,512,1024,2048};
#define SLAB_MAX_SIZE     2048

/* ---- Slab page header (lives at the start of each slab page) -- */
typedef struct SlabHdr {
    u32 free_head;     /* index of first free slot (SLAB_NIL if none) */
    u32 free_count;    /* number of free slots */
    u32 total;         /* total slots on this page */
    u32 obj_size;      /* slot size (bytes) */
    u32 class_idx;     /* which cache this belongs to */
    u32 next_slab;     /* physical address of next slab page / 0 */
} SlabHdr;

#define SLAB_NIL  0xFFFFFFFFu
#define SLAB_HDR_SZ  ((sizeof(SlabHdr) + 15) & ~15u)  /* rounded to 16 */

/* One cache per size class: head slab physical address */
static u64 slab_cache[SLAB_NUM_CLASSES];  /* 0 = no slab yet */

/* ---- Compute slot address from slab page + slot index --------- */
static inline void *slot_ptr(u64 slab_phys, u32 obj_size, u32 idx) {
    return (u8*)slab_phys + SLAB_HDR_SZ + (usize)idx * obj_size;
}

/* ---- Initialise a fresh slab page for a given size class ------- */
static void slab_page_init(u64 phys, u32 class_idx) {
    u32 obj_size = slab_sizes[class_idx];
    memset((void*)phys, 0, PAGE_SIZE);

    SlabHdr *h   = (SlabHdr*)phys;
    u32 data_sz  = PAGE_SIZE - SLAB_HDR_SZ;
    u32 n        = data_sz / obj_size;

    h->obj_size  = obj_size;
    h->class_idx = class_idx;
    h->total     = n;
    h->free_count = n;
    h->next_slab = 0;

    /* Build the free list: each free slot's first u32 = next index */
    for (u32 i = 0; i < n; i++) {
        u32 *slot = (u32*)slot_ptr(phys, obj_size, i);
        *slot = (i + 1 < n) ? (i + 1) : SLAB_NIL;
    }
    h->free_head = 0;
}

/* ---- Allocate from slab tier ----------------------------------- */
static void *slab_alloc(u32 class_idx) {
    u32 obj_size = slab_sizes[class_idx];

    /* Walk slab list to find a page with free slots */
    u64 phys = slab_cache[class_idx];
    while (phys) {
        SlabHdr *h = (SlabHdr*)phys;
        if (h->free_count > 0) goto found;
        phys = h->next_slab;
    }

    /* No free slab — allocate a new page */
    phys = pmm_alloc();
    if (!phys) return NULL;
    slab_page_init(phys, class_idx);
    ((SlabHdr*)phys)->next_slab = (u32)slab_cache[class_idx];
    slab_cache[class_idx] = phys;

found: {
    SlabHdr *h = (SlabHdr*)phys;
    u32 idx    = h->free_head;
    u32 *slot  = (u32*)slot_ptr(phys, obj_size, idx);
    h->free_head = *slot;
    h->free_count--;
    memset(slot, 0, obj_size);   /* zero on allocation, like kmalloc */
    return slot;
}
}

/* Determine whether ptr lives inside a slab page.
 * Slab pages are whole pmm pages; the SlabHdr at page start has a
 * recognisable obj_size in the slab_sizes table. */
static int ptr_in_slab(void *ptr, u32 *class_out, u64 *slab_phys_out) {
    u64 phys = (u64)(usize)ptr & ~(u64)(PAGE_SIZE - 1);  /* page base */
    SlabHdr *h = (SlabHdr*)phys;

    /* Validate: obj_size must be one of our slab classes */
    for (u32 c = 0; c < SLAB_NUM_CLASSES; c++) {
        if (h->obj_size == slab_sizes[c] && h->class_idx == c) {
            *class_out      = c;
            *slab_phys_out  = phys;
            return 1;
        }
    }
    return 0;
}

static void slab_free(void *ptr, u32 class_idx, u64 slab_phys) {
    SlabHdr *h   = (SlabHdr*)slab_phys;
    u32 obj_size = h->obj_size;
    u32 idx      = (u32)((u8*)ptr - ((u8*)slab_phys + SLAB_HDR_SZ)) / obj_size;
    *(u32*)ptr   = h->free_head;
    h->free_head = idx;
    h->free_count++;
}

/* ================================================================
 *  BLOCK (LARGE OBJECT) TIER
 *  Original boundary-tag allocator -- unchanged semantics.
 * ================================================================ */

#define BLOCK_HDR  16
#define FLAG_FREE  1UL

typedef struct Block { u64 size; u64 flags; } Block;

void heap_init(void) {
    /* Zero the block heap */
    Block *b = (Block*)HEAP_BASE;
    b->size  = HEAP_SIZE - BLOCK_HDR;
    b->flags = FLAG_FREE;
    memset((u8*)HEAP_BASE + BLOCK_HDR, 0, HEAP_SIZE - BLOCK_HDR);

    /* Init slab caches */
    for (u32 c = 0; c < SLAB_NUM_CLASSES; c++) slab_cache[c] = 0;
}

static void *block_malloc(usize n) {
    n = (n + 15) & ~15UL;
    Block *b = (Block*)HEAP_BASE;
    while ((u64)b < HEAP_BASE + HEAP_SIZE) {
        if ((b->flags & FLAG_FREE) && b->size >= n) {
            if (b->size >= n + BLOCK_HDR + 16) {
                Block *next = (Block*)((u8*)b + BLOCK_HDR + n);
                next->size  = b->size - n - BLOCK_HDR;
                next->flags = FLAG_FREE;
                b->size = n;
            }
            b->flags = 0;
            memset((u8*)b + BLOCK_HDR, 0, b->size);
            return (u8*)b + BLOCK_HDR;
        }
        b = (Block*)((u8*)b + BLOCK_HDR + b->size);
    }
    return NULL;
}

static void block_free(void *ptr) {
    if (!ptr) return;
    Block *b = (Block*)((u8*)ptr - BLOCK_HDR);
    if ((u64)b < HEAP_BASE || (u64)b >= HEAP_BASE + HEAP_SIZE) return;
    b->flags = FLAG_FREE;

    /* Coalesce backward: walk from heap start to find the predecessor.
     * The original code only coalesced forward, so interleaved alloc/free
     * patterns would gradually fragment the heap into unrecoverable slivers.
     * This O(n) walk is acceptable — the block heap is only used for large
     * objects (> SLAB_MAX_SIZE = 2 KB), so there are few blocks total. */
    Block *prev = NULL;
    Block *cur  = (Block*)HEAP_BASE;
    while ((u64)cur < (u64)b) {
        prev = cur;
        cur  = (Block*)((u8*)cur + BLOCK_HDR + cur->size);
    }
    if (prev && (prev->flags & FLAG_FREE)) {
        /* Merge b into prev */
        prev->size += BLOCK_HDR + b->size;
        b = prev;   /* continue forward-coalescing from the merged block */
    }

    /* Coalesce forward */
    while (1) {
        Block *next = (Block*)((u8*)b + BLOCK_HDR + b->size);
        if ((u64)next >= HEAP_BASE + HEAP_SIZE) break;
        if (!(next->flags & FLAG_FREE)) break;
        b->size += BLOCK_HDR + next->size;
    }
}

/* ================================================================
 *  PUBLIC API
 * ================================================================ */

void *heap_malloc(usize n) {
    if (n == 0) return NULL;

    /* Route to slab for small allocations */
    for (u32 c = 0; c < SLAB_NUM_CLASSES; c++) {
        if (n <= slab_sizes[c]) return slab_alloc(c);
    }

    /* Large allocation: block heap */
    return block_malloc(n);
}

void heap_free(void *ptr) {
    if (!ptr) return;

    /* Determine which tier owns this pointer */
    u32 class_idx;
    u64 slab_phys;
    if (ptr_in_slab(ptr, &class_idx, &slab_phys)) {
        slab_free(ptr, class_idx, slab_phys);
        return;
    }

    /* Block heap */
    if ((u64)(usize)ptr >= HEAP_BASE && (u64)(usize)ptr < HEAP_BASE + HEAP_SIZE)
        block_free(ptr);
}

void *heap_realloc(void *old, usize new_sz) {
    if (!old)   return heap_malloc(new_sz);
    if (!new_sz) { heap_free(old); return NULL; }

    /* Determine old size */
    usize old_sz = 0;
    u32 class_idx; u64 slab_phys;
    if (ptr_in_slab(old, &class_idx, &slab_phys)) {
        old_sz = slab_sizes[class_idx];
    } else {
        Block *b = (Block*)((u8*)old - BLOCK_HDR);
        if ((u64)(usize)b >= HEAP_BASE && (u64)(usize)b < HEAP_BASE + HEAP_SIZE)
            old_sz = b->size;
    }

    if (new_sz <= old_sz) return old;   /* fits in place */
    void *n = heap_malloc(new_sz);
    if (!n) return NULL;
    memcpy(n, old, old_sz);
    heap_free(old);
    return n;
}
