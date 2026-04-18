/* ================================================================
 *  ENGINE OS — kernel/pmm.c  (v2: Buddy Allocator)
 *
 *  Replaces the old O(n) bitmap first-fit allocator with a two-level
 *  system matching Linux's physical memory model:
 *
 *  1. BUDDY ALLOCATOR (orders 0–MAX_ORDER)
 *     - Each order k manages contiguous blocks of 2^k pages.
 *     - Alloc: find the smallest free block >= requested size,
 *       split it down, push unused halves back as free buddies.
 *     - Free: coalesce with buddy partner repeatedly until no
 *       free buddy exists or we reach MAX_ORDER -- O(log N).
 *     - Per-order doubly-linked free lists (intrusive, in the
 *       free pages themselves -- like Linux's page->lru).
 *
 *  2. REFERENCE COUNTING  (for Copy-on-Write)
 *     - Every physical page frame has a u8 refcount.
 *     - pmm_ref()   : increment (pin) a page.
 *     - pmm_unref() : decrement; actually frees when count -> 0.
 *
 *  Layout in PMM_BITMAP region (0x600000):
 *    [0..ORDER_LISTS*8-1]  free-list head nodes
 *    [ORDER_LISTS*8..]     refcount array (1 byte per page)
 *    [+TOTAL_PAGES..]      free/used bitmap (1 bit per page)
 *
 *  The managed range is RAM_START (4 MB) to RAM_END (64 MB),
 *  giving TOTAL_PAGES = 15360 page frames.
 * ================================================================ */
#include "../include/kernel.h"

/* ---- Buddy constants ----------------------------------------- */
#define MAX_ORDER      10          /* max block = 2^10 = 1024 pages = 4 MB */
#define ORDER_LISTS    (MAX_ORDER + 1)
#define BUDDY_NIL      0xFFFFFFFFu

/* ---- Intrusive free-list node (stored at start of free page) -- */
typedef struct BuddyNode { u32 prev; u32 next; } BuddyNode;

/* Free-list heads array at the start of PMM_BITMAP */
#define BUDDY_HEADS  ((BuddyNode*)(PMM_BITMAP))

/* Refcount array: one u8 per page, right after the heads */
#define PMM_REFCNT   ((u8*)(PMM_BITMAP + ORDER_LISTS * sizeof(BuddyNode)))

/* Free/used bitmap: one bit per page, after the refcounts */
#define BUDDY_BMP    ((u8*)(PMM_BITMAP + ORDER_LISTS * sizeof(BuddyNode) + TOTAL_PAGES))
#define BUDDY_BMP_SZ ((TOTAL_PAGES + 7) / 8)

/* ---- addr <-> index helpers ----------------------------------- */
static inline u64       idx_to_phys(u32 i) { return RAM_START + (u64)i * PAGE_SIZE; }
static inline u32       phys_to_idx(u64 p) { return (u32)((p - RAM_START) / PAGE_SIZE); }
static inline BuddyNode *page_node(u32 i)  { return (BuddyNode*)idx_to_phys(i); }

/* ---- Free-bit helpers ----------------------------------------- */
static inline void bfree_set(u32 i) { BUDDY_BMP[i>>3] |=  (u8)(1<<(i&7)); }
static inline void bfree_clr(u32 i) { BUDDY_BMP[i>>3] &= ~(u8)(1<<(i&7)); }
static inline int  bfree_tst(u32 i) { return (BUDDY_BMP[i>>3]>>(i&7))&1;  }

/* ---- Intrusive list ops --------------------------------------- */
static void list_init(u32 o) {
    BUDDY_HEADS[o].prev = BUDDY_HEADS[o].next = BUDDY_NIL;
}
static void list_push(u32 o, u32 idx) {
    BuddyNode *h = &BUDDY_HEADS[o], *n = page_node(idx);
    n->prev = BUDDY_NIL; n->next = h->next;
    if (h->next != BUDDY_NIL) page_node(h->next)->prev = idx;
    h->next = idx;
}
static void list_remove(u32 o, u32 idx) {
    BuddyNode *h = &BUDDY_HEADS[o], *n = page_node(idx);
    if (n->prev != BUDDY_NIL) page_node(n->prev)->next = n->next;
    else h->next = n->next;
    if (n->next != BUDDY_NIL) page_node(n->next)->prev = n->prev;
}
static u32 list_pop(u32 o) {
    u32 idx = BUDDY_HEADS[o].next;
    if (idx != BUDDY_NIL) list_remove(o, idx);
    return idx;
}

/* ---- pmm_init -------------------------------------------------- */
void pmm_init(void) {
    usize meta = ORDER_LISTS * sizeof(BuddyNode) + TOTAL_PAGES + BUDDY_BMP_SZ;
    memset((void*)PMM_BITMAP, 0, meta);
    for (u32 o = 0; o <= MAX_ORDER; o++) list_init(o);

    /* First reserved page index: everything below meta_end */
    u64 meta_end = PMM_BITMAP + meta;
    u32 reserved = (u32)((meta_end - RAM_START + PAGE_SIZE - 1) / PAGE_SIZE);

    /* Feed free pages into the buddy system at the highest valid order */
    u32 idx = reserved;
    while (idx < TOTAL_PAGES) {
        u32 order = MAX_ORDER;
        while (order > 0 &&
               ((idx & ((1u<<order)-1)) || idx+(1u<<order) > TOTAL_PAGES))
            order--;
        for (u32 k = 0; k < (1u<<order); k++) bfree_set(idx+k);
        list_push(order, idx);
        idx += (1u << order);
    }
}

/* ---- pmm_alloc_order ------------------------------------------ */
u64 pmm_alloc_order(u32 order) {
    if (order > MAX_ORDER) return 0;
    u32 found = MAX_ORDER + 1;
    for (u32 o = order; o <= MAX_ORDER; o++)
        if (BUDDY_HEADS[o].next != BUDDY_NIL) { found = o; break; }
    if (found > MAX_ORDER) {
        oom_kill();   /* free pages from a victim process, then retry */
        for (u32 o = order; o <= MAX_ORDER; o++)
            if (BUDDY_HEADS[o].next != BUDDY_NIL) { found = o; break; }
        if (found > MAX_ORDER) return 0;  /* oom_kill itself panics if nothing freed */
    }

    u32 idx = list_pop(found);
    /* Split down */
    while (found > order) {
        found--;
        u32 bud = idx + (1u << found);
        bfree_set(bud);
        list_push(found, bud);
    }
    /* Mark used */
    for (u32 k = 0; k < (1u<<order); k++) bfree_clr(idx+k);
    PMM_REFCNT[idx] = 1;
    return idx_to_phys(idx);
}

u64 pmm_alloc(void)          { return pmm_alloc_order(0); }

u64 pmm_alloc_n(usize n) {
    u32 o = 0;
    while ((1u<<o) < n && o < MAX_ORDER) o++;
    return pmm_alloc_order(o);
}

/* ---- pmm_free_order ------------------------------------------- */
void pmm_free_order(u64 phys, u32 order) {
    if (phys < RAM_START || phys >= RAM_END) return;
    u32 idx = phys_to_idx(phys);
    if (idx >= TOTAL_PAGES) return;

    /* Buddy coalescing -- O(log N) */
    while (order < MAX_ORDER) {
        u32 bud = idx ^ (1u << order);
        if (bud >= TOTAL_PAGES || !bfree_tst(bud)) break;
        list_remove(order, bud);
        for (u32 k = 0; k < (1u<<order); k++) bfree_clr(bud+k);
        if (bud < idx) idx = bud;
        order++;
    }
    for (u32 k = 0; k < (1u<<order); k++) bfree_set(idx+k);
    list_push(order, idx);
}

void pmm_free(u64 phys) { pmm_free_order(phys, 0); }

/* ---- Reference counting (CoW support) ------------------------- */
void pmm_ref(u64 phys) {
    if (phys < RAM_START || phys >= RAM_END) return;
    u32 idx = phys_to_idx(phys);
    if (PMM_REFCNT[idx] < 255) PMM_REFCNT[idx]++;
}

u8 pmm_unref(u64 phys) {
    if (phys < RAM_START || phys >= RAM_END) return 0;
    u32 idx = phys_to_idx(phys);
    if (!PMM_REFCNT[idx]) return 0;
    if (--PMM_REFCNT[idx] == 0) { pmm_free(phys); return 0; }
    return PMM_REFCNT[idx];
}

u8 pmm_refcount(u64 phys) {
    if (phys < RAM_START || phys >= RAM_END) return 0;
    return PMM_REFCNT[phys_to_idx(phys)];
}

/* ---- Stats ---------------------------------------------------- */
u32 pmm_free_pages(void) {
    u32 free = 0;
    for (u32 i = 0; i < TOTAL_PAGES; i++) if (bfree_tst(i)) free++;
    return free;
}

void pmm_clear_region(u64 base, usize size) { memset((void*)base, 0, size); }
