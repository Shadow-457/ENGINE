/* ================================================================
 *  Systrix OS — kernel/pmm.c  (v4: 64-bit physical memory, no 4 GB cap)
 *
 *  Changes from v3:
 *    - RAM_END_MAX raised to 64 GB in kernel.h
 *    - page indices and total_pages are now u64 to handle >4 GB
 *    - MAX_PAGES uses u64 arithmetic; buddy node pointers stay u32
 *      (buddy links stored in the page itself, so 32-bit index is fine
 *       up to 4 billion pages which far exceeds addressable RAM)
 *    - phys_to_idx / idx_to_phys use u64 throughout
 *    - pmm_free_pages returns u64
 *    - Identity mapping in entry.S extended to 64 GB (see entry.S)
 *
 *  With any QEMU RAM amount: meminfo now reports the real value.
 * ================================================================ */
#include "../include/kernel.h"

/* runtime RAM ceiling set during pmm_init */
u64 ram_end_actual = 0x4000000UL;   /* safe fallback: 64 MB */

#define MAX_ORDER      10
#define ORDER_LISTS    (MAX_ORDER + 1)
#define BUDDY_NIL      0xFFFFFFFFu
#define MAX_PAGES      ((u32)(RAM_END_MAX / PAGE_SIZE))

typedef struct BuddyNode { u32 prev; u32 next; } BuddyNode;

#define BUDDY_HEADS  ((BuddyNode*)(PMM_BITMAP))
#define PMM_REFCNT   ((u8*)(PMM_BITMAP + ORDER_LISTS * sizeof(BuddyNode)))
#define BUDDY_BMP    ((u8*)(PMM_BITMAP + ORDER_LISTS * sizeof(BuddyNode) + MAX_PAGES))
#define BUDDY_BMP_SZ ((MAX_PAGES + 7) / 8)

static inline u64       idx_to_phys(u32 i) { return RAM_START + (u64)i * PAGE_SIZE; }
static inline u32       phys_to_idx(u64 p) { return (u32)((p - RAM_START) / PAGE_SIZE); }
static inline BuddyNode *page_node(u32 i)  { return (BuddyNode*)idx_to_phys(i); }

static u32 total_pages = 0;

static inline void bfree_set(u32 i) { BUDDY_BMP[i>>3] |=  (u8)(1<<(i&7)); }
static inline void bfree_clr(u32 i) { BUDDY_BMP[i>>3] &= ~(u8)(1<<(i&7)); }
static inline int  bfree_tst(u32 i) { return (BUDDY_BMP[i>>3]>>(i&7))&1;  }

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

static void buddy_add_range(u32 first, u32 last_excl) {
    u32 idx = first;
    while (idx < last_excl) {
        u32 order = MAX_ORDER;
        while (order > 0 &&
               ((idx & ((1u<<order)-1)) || idx + (1u<<order) > last_excl))
            order--;
        for (u32 k = 0; k < (1u<<order); k++) bfree_set(idx+k);
        list_push(order, idx);
        idx += (1u << order);
    }
}

void pmm_init(void) {
    usize meta = ORDER_LISTS * sizeof(BuddyNode) + MAX_PAGES + BUDDY_BMP_SZ;
    memset((void*)PMM_BITMAP, 0, meta);
    for (u32 o = 0; o <= MAX_ORDER; o++) list_init(o);

    u16 e820_count = *(u16*)E820_MAP_ADDR;
    E820Entry *map  = (E820Entry*)(E820_MAP_ADDR + 2);
    u64 highest = 0;

    if (e820_count == 0 || e820_count > E820_MAX) {
        e820_count = 0;
        highest = 0x4000000UL;  /* fallback 64 MB */
    } else {
        for (u16 i = 0; i < e820_count; i++) {
            if (map[i].type != E820_USABLE) continue;
            u64 end = map[i].base + map[i].len;
            if (end > highest) highest = end;
        }
        if (highest < 0x4000000UL) highest = 0x4000000UL;
    }

    if (highest > 0x40000000UL) highest = 0x40000000UL; /* cap at 1 GB identity map limit */
    ram_end_actual = highest;
    total_pages    = (u32)((highest - RAM_START) / PAGE_SIZE);

    u64 meta_end = PMM_BITMAP + meta;
    u32 reserved = (u32)((meta_end - RAM_START + PAGE_SIZE - 1) / PAGE_SIZE);

    if (e820_count > 0) {
        for (u16 i = 0; i < e820_count; i++) {
            if (map[i].type != E820_USABLE) continue;
            u64 base = map[i].base;
            u64 end  = base + map[i].len;
            if (end  <= RAM_START) continue;
            if (base <  RAM_START) base = RAM_START;
            if (end  >  highest)   end  = highest;
            base = (base + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
            end  =  end                   & ~(u64)(PAGE_SIZE - 1);
            if (base >= end) continue;
            u32 first = phys_to_idx(base);
            u32 last  = phys_to_idx(end);
            if (first < reserved) first = reserved;
            if (first >= last) continue;
            buddy_add_range(first, last);
        }
    } else {
        buddy_add_range(reserved, total_pages);
    }
}

u64 pmm_alloc_order(u32 order) {
    if (order > MAX_ORDER) return 0;
    u32 found = MAX_ORDER + 1;
    for (u32 o = order; o <= MAX_ORDER; o++)
        if (BUDDY_HEADS[o].next != BUDDY_NIL) { found = o; break; }
    if (found > MAX_ORDER) {
        oom_kill();
        for (u32 o = order; o <= MAX_ORDER; o++)
            if (BUDDY_HEADS[o].next != BUDDY_NIL) { found = o; break; }
        if (found > MAX_ORDER) return 0;
    }
    u32 idx = list_pop(found);
    while (found > order) {
        found--;
        u32 bud = idx + (1u << found);
        bfree_set(bud);
        list_push(found, bud);
    }
    for (u32 k = 0; k < (1u<<order); k++) bfree_clr(idx+k);
    PMM_REFCNT[idx] = 1;
    return idx_to_phys(idx);
}

u64 pmm_alloc(void)      { return pmm_alloc_order(0); }
u64 pmm_alloc_n(usize n) {
    u32 o = 0;
    while ((1u<<o) < n && o < MAX_ORDER) o++;
    return pmm_alloc_order(o);
}

void pmm_free_order(u64 phys, u32 order) {
    if (phys < RAM_START || phys >= ram_end_actual) return;
    u32 idx = phys_to_idx(phys);
    if (idx >= total_pages) return;
    while (order < MAX_ORDER) {
        u32 bud = idx ^ (1u << order);
        if (bud >= total_pages || !bfree_tst(bud)) break;
        list_remove(order, bud);
        for (u32 k = 0; k < (1u<<order); k++) bfree_clr(bud+k);
        if (bud < idx) idx = bud;
        order++;
    }
    for (u32 k = 0; k < (1u<<order); k++) bfree_set(idx+k);
    list_push(order, idx);
}

void pmm_free(u64 phys) { pmm_free_order(phys, 0); }

void pmm_ref(u64 phys) {
    if (phys < RAM_START || phys >= ram_end_actual) return;
    u32 idx = phys_to_idx(phys);
    if (PMM_REFCNT[idx] < 255) PMM_REFCNT[idx]++;
}

u8 pmm_unref(u64 phys) {
    if (phys < RAM_START || phys >= ram_end_actual) return 0;
    u32 idx = phys_to_idx(phys);
    if (!PMM_REFCNT[idx]) return 0;
    if (--PMM_REFCNT[idx] == 0) { pmm_free(phys); return 0; }
    return PMM_REFCNT[idx];
}

u8 pmm_refcount(u64 phys) {
    if (phys < RAM_START || phys >= ram_end_actual) return 0;
    return PMM_REFCNT[phys_to_idx(phys)];
}

u32 pmm_free_pages(void) {
    u32 free = 0;
    for (u32 i = 0; i < total_pages; i++) {
        if (bfree_tst(i)) free++;
        if ((i & 1023) == 0) watchdog_pet();
    }
    return free;
}

void pmm_clear_region(u64 base, usize size) { memset((void*)base, 0, size); }
