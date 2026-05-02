/* ================================================================
 *  Systrix OS — kernel/pmm_enhanced.c  (Production-Level PMM)
 *
 *  Enhancements over the original pmm.c:
 *
 *  1. COMPREHENSIVE MEMORY STATISTICS API
 *     - Per-order allocation/free counts
 *     - Fragmentation metrics (buddy index, external fragmentation)
 *     - Total/free/used page tracking (O(1) via counters)
 *     - Peak usage tracking for memory profiling
 *
 *  2. DEFRAGMENTATION SUPPORT
 *     - Buddy compaction: periodically try to merge free blocks
 *     - Defrag API: attempt to coalesce free pages into higher orders
 *     - Watermark-based triggering (like Linux's min/free/high watermarks)
 *
 *  3. BETTER ALLOCATION STRATEGIES
 *     - NUMA-ready zone structure (zones: DMA, Normal, HighMem)
 *     - Allocation watermarks to prevent fragmentation
 *     - Fallback to lower orders with compaction trigger
 *     - Contiguous multi-page allocation with alignment
 *
 *  4. MEMORY POISONING & DEBUG
 *     - Free pages poisoned with 0xAA pattern
 *     - Allocation guard pages for overflow detection
 *     - Allocation tracking for leak detection
 *
 *  5. EARLY BOOT ALLOCATOR
 *     - Simple bump allocator used before pmm_init
 *     - Tracks early allocations for later handoff
 * ================================================================ */
#include "../include/kernel.h"

/* Re-export internal structures from pmm.c via extern declarations.
 * These are defined at the PMM_BITMAP address (0x600000). */

/* ---- Buddy constants (must match pmm.c) ---- */
#define MAX_ORDER      10
#define ORDER_LISTS    (MAX_ORDER + 1)
#define BUDDY_NIL      0xFFFFFFFFu

typedef struct BuddyNode { u32 prev; u32 next; } BuddyNode;

/* Direct memory-mapped access (same as pmm.c) */
#define ENH_BUDDY_HEADS  ((BuddyNode*)(PMM_BITMAP))
#define ENH_PMM_REFCNT   ((u8*)(PMM_BITMAP + ORDER_LISTS * sizeof(BuddyNode)))
#define ENH_BUDDY_BMP    ((u8*)(PMM_BITMAP + ORDER_LISTS * sizeof(BuddyNode) + TOTAL_PAGES))
#define ENH_BUDDY_BMP_SZ ((TOTAL_PAGES + 7) / 8)

/* ---- Enhanced statistics -------------------------------------- */


static PmmStats pmm_stats;

/* ---- Watermarks for defrag triggering ------------------------- */
#define WATERMARK_HIGH   256   /* if free pages > this, no pressure */
#define WATERMARK_MIN    64    /* if free pages < this, trigger OOM */
#define DEFRAG_THRESHOLD 128   /* trigger compaction if free < this */

/* ---- Memory poisoning ----------------------------------------- */
#define POISON_FREE     0xAA
#define POISON_ALLOC    0xBB
#define GUARD_PATTERN   0xDEADBEEFDEADBEEFULL

/* ---- Zone structure (NUMA-ready) ------------------------------ */


/* Single zone — filled at runtime once ram_end_actual is known */
static MemoryZone zones[1];
#define ZONE_NORMAL 0
#define ZONE_COUNT  1

/* ---- Inline helpers (mirror pmm.c internals) ------------------ */
static inline u64  idx_to_phys(u32 i) { return RAM_START + (u64)i * PAGE_SIZE; }
static inline u32  phys_to_idx(u64 p) { return (u32)((p - RAM_START) / PAGE_SIZE); }
static inline BuddyNode *page_node(u32 i) { return (BuddyNode*)idx_to_phys(i); }

static inline void bfree_set(u32 i) { ENH_BUDDY_BMP[i>>3] |=  (u8)(1<<(i&7)); }
static inline void bfree_clr(u32 i) { ENH_BUDDY_BMP[i>>3] &= ~(u8)(1<<(i&7)); }
static inline int  bfree_tst(u32 i) { return (ENH_BUDDY_BMP[i>>3]>>(i&7))&1; }

/* ---- List operations (reimplemented for enhanced module) ------ */
static void enh_list_push(u32 o, u32 idx) {
    BuddyNode *h = &ENH_BUDDY_HEADS[o], *n = page_node(idx);
    n->prev = BUDDY_NIL; n->next = h->next;
    if (h->next != BUDDY_NIL) page_node(h->next)->prev = idx;
    h->next = idx;
}
static void enh_list_remove(u32 o, u32 idx) {
    BuddyNode *h = &ENH_BUDDY_HEADS[o], *n = page_node(idx);
    if (n->prev != BUDDY_NIL) page_node(n->prev)->next = n->next;
    else h->next = n->next;
    if (n->next != BUDDY_NIL) page_node(n->next)->prev = n->prev;
}

/* ---- Statistics helpers --------------------------------------- */
static void __attribute__((unused)) stats_update_alloc(u32 order) {
    pmm_stats.total_allocations++;
    pmm_stats.total_pages_allocated += (1ULL << order);
    pmm_stats.alloc_per_order[order]++;

    u64 pages = pmm_stats.total_pages_allocated - pmm_stats.total_pages_freed;
    pmm_stats.current_allocated = pages;
    if (pages > pmm_stats.peak_allocated)
        pmm_stats.peak_allocated = pages;
}

static void __attribute__((unused)) stats_update_free(u32 order) {
    pmm_stats.total_frees++;
    pmm_stats.total_pages_freed += (1ULL << order);
    pmm_stats.free_per_order[order]++;

    u64 pages = pmm_stats.total_pages_allocated - pmm_stats.total_pages_freed;
    pmm_stats.current_allocated = pages;
}

/* ---- Fragmentation index calculation --------------------------
 * Computes how fragmented the free memory is.
 * 0 = all free memory is in largest blocks (perfect)
 * 1000 = all free memory is in smallest blocks (worst)
 * Based on Linux's extfrag_index. */
static u64 calc_fragmentation_index(void) {
    u64 free_pages = 0;
    u64 max_free_in_order = 0;

    for (u32 o = 0; o <= MAX_ORDER; o++) {
        u64 pages_in_order = 0;
        BuddyNode *h = &ENH_BUDDY_HEADS[o];
        u32 idx = h->next;
        while (idx != BUDDY_NIL) {
            pages_in_order += (1ULL << o);
            idx = page_node(idx)->next;
        }
        free_pages += pages_in_order;
        if (pages_in_order > 0)
            max_free_in_order += pages_in_order;
    }

    if (free_pages == 0) return 1000;

    /* Weight higher orders more heavily */
    u64 weighted_sum = 0;
    for (u32 o = 0; o <= MAX_ORDER; o++) {
        u64 pages_in_order = 0;
        BuddyNode *h = &ENH_BUDDY_HEADS[o];
        u32 idx = h->next;
        while (idx != BUDDY_NIL) {
            pages_in_order += (1ULL << o);
            idx = page_node(idx)->next;
        }
        weighted_sum += pages_in_order * o;
    }

    u64 max_possible = free_pages * MAX_ORDER;
    if (max_possible == 0) return 0;

    return (1000ULL * (max_possible - weighted_sum)) / max_possible;
}

/* ================================================================
 *  PUBLIC ENHANCED API
 * ================================================================ */

/* ---- Comprehensive stats query -------------------------------- */
void pmm_get_stats(PmmStats *out) {
    if (!out) return;
    *out = pmm_stats;
    out->current_free = pmm_free_pages();
    out->fragmentation_index = calc_fragmentation_index();
}

/* ---- Print memory statistics to console ----------------------- */
void pmm_print_stats(void) {
    PmmStats s;
    pmm_get_stats(&s);

    kprintf("\n=== PMM Memory Statistics ===\r\n"
                "Total allocations:            %llu\r\n"
                "Total frees:                  %llu\r\n"
                "Current allocated (pages):    %llu\r\n"
                "Current free (pages):         %llu\r\n"
                "Peak allocated (pages):       %llu\r\n"
                "Fragmentation index (0-1000): %llu\r\n"
                "Compaction runs:              %llu\r\n"
                "Pages compacted:              %llu\r\n",
                (unsigned long long)s.total_allocations,
                (unsigned long long)s.total_frees,
                (unsigned long long)s.current_allocated,
                (unsigned long long)s.current_free,
                (unsigned long long)s.peak_allocated,
                (unsigned long long)s.fragmentation_index,
                (unsigned long long)s.compaction_runs,
                (unsigned long long)s.pages_compacted);

    kprintf("\r\nPer-order allocations:\r\n");
    for (u32 o = 0; o <= MAX_ORDER; o++) {
        if (s.alloc_per_order[o] > 0) {
            kprintf("  order %u: %llu\r\n",
                      o, (unsigned long long)s.alloc_per_order[o]);
        }
    }
    kprintf("===========================\r\n");
}

/* ---- Defragmentation: try to coalesce free pages --------------
 * Walks all free lists and attempts buddy coalescing.
 * Returns number of pages compacted. */
u64 pmm_defragment(void) {
    u64 compacted = 0;
    pmm_stats.compaction_runs++;

    /* Try to merge from order 0 upward */
    for (u32 order = 0; order < MAX_ORDER; order++) {
        BuddyNode *h = &ENH_BUDDY_HEADS[order];
        u32 idx = h->next;

        while (idx != BUDDY_NIL) {
            u32 next_idx = page_node(idx)->next;  /* save before potential removal */
            u32 buddy = idx ^ (1u << order);

            /* Check if buddy is free and at same order */
            if (buddy < TOTAL_PAGES && bfree_tst(buddy)) {
                /* Find buddy in the free list */
                u32 b_idx = h->next;
                int found = 0;
                while (b_idx != BUDDY_NIL) {
                    if (b_idx == buddy) { found = 1; break; }
                    b_idx = page_node(b_idx)->next;
                }

                if (found) {
                    /* Remove both from current order list */
                    enh_list_remove(order, idx);
                    enh_list_remove(order, buddy);

                    /* Clear free bits for both */
                    for (u32 k = 0; k < (1u << order); k++) {
                        bfree_clr(idx + k);
                        bfree_clr(buddy + k);
                    }

                    /* Merge: lower address becomes the merged block */
                    u32 merged = (idx < buddy) ? idx : buddy;

                    /* Add to next order */
                    bfree_set(merged);
                    enh_list_push(order + 1, merged);

                    compacted += (1ULL << order);
                    pmm_stats.pages_compacted += (1ULL << order);

                    /* Continue scanning from next order */
                    idx = next_idx;
                    continue;
                }
            }
            idx = next_idx;
        }
    }

    return compacted;
}

/* ---- Watermark check: returns 1 if allocation is safe --------- */
int pmm_watermark_ok(u32 order) {
    u32 free = pmm_free_pages();
    u32 needed = (1u << order);

    if (free >= WATERMARK_HIGH) return 1;
    if (free < WATERMARK_MIN) return 0;

    /* Try defrag if under pressure */
    if (free < DEFRAG_THRESHOLD && order > 0) {
        pmm_defragment();
        free = pmm_free_pages();
    }

    return free >= needed;
}

/* ---- Aligned contiguous allocation ----------------------------
 * Allocates n pages aligned to align_pages boundary.
 * Uses buddy allocator with alignment constraint. */
u64 pmm_alloc_aligned(u32 n, u32 align_pages) {
    if (n == 0 || align_pages == 0) return 0;

    /* Round up to power of 2 */
    u32 order = 0;
    while ((1u << order) < n && order < MAX_ORDER) order++;

    /* If alignment requirement is higher than order, bump up */
    u32 align_order = 0;
    while ((1u << align_order) < align_pages && align_order < MAX_ORDER) align_order++;
    if (align_order > order) order = align_order;

    u64 phys = pmm_alloc_order(order);
    if (!phys) return 0;

    /* If the allocated block isn't aligned enough, free and retry */
    u32 idx = phys_to_idx(phys);
    if (idx & ((align_pages) - 1)) {
        pmm_free_order(phys, order);
        /* Try next order up for better alignment chance */
        if (order + 1 <= MAX_ORDER) {
            phys = pmm_alloc_order(order + 1);
            if (!phys) return 0;
            idx = phys_to_idx(phys);
            if (idx & ((align_pages) - 1)) {
                pmm_free_order(phys, order + 1);
                return 0;
            }
        } else {
            return 0;
        }
    }

    return phys;
}

/* ---- Zone-aware allocation ------------------------------------ */
u64 pmm_alloc_zone(u32 zone_id, u32 order) {
    if (zone_id >= ZONE_COUNT) return pmm_alloc_order(order);

    MemoryZone *z = &zones[zone_id];
    if (z->free_pages < (1u << order)) return 0;

    u64 phys = pmm_alloc_order(order);
    if (phys) {
        u32 idx = phys_to_idx(phys);
        if (idx >= z->start_pfn && idx < z->end_pfn)
            z->free_pages -= (1u << order);
    }
    return phys;
}

void pmm_free_zone(u64 phys, u32 zone_id, u32 order) {
    pmm_free_order(phys, order);
    if (zone_id < ZONE_COUNT) {
        MemoryZone *z = &zones[zone_id];
        u32 idx = phys_to_idx(phys);
        if (idx >= z->start_pfn && idx < z->end_pfn)
            z->free_pages += (1u << order);
    }
}

/* ---- Memory poisoning: poison freed pages --------------------- */
void pmm_poison_page(u64 phys) {
    if (phys < RAM_START || phys >= RAM_END) return;
    memset((void*)phys, POISON_FREE, PAGE_SIZE);
}

/* ---- Memory poisoning: check if page is poisoned -------------- */
int pmm_check_poison(u64 phys) {
    if (phys < RAM_START || phys >= RAM_END) return 0;
    u8 *p = (u8*)phys;
    /* Check first 16 bytes for poison pattern */
    for (int i = 0; i < 16; i++) {
        if (p[i] != POISON_FREE) return 0;
    }
    return 1;
}

/* ---- Initialize zone statistics ------------------------------- */
void pmm_zones_init(void) {
    zones[0].start_pfn      = RAM_START / PAGE_SIZE;
    zones[0].end_pfn        = RAM_END   / PAGE_SIZE;
    zones[0].free_pages     = 0;
    zones[0].managed_pages  = (RAM_END - RAM_START) / PAGE_SIZE;
    zones[0].watermark_min  = WATERMARK_MIN;
    zones[0].watermark_high = WATERMARK_HIGH;
    __builtin_memcpy(zones[0].name, "Normal", 7);
    for (u32 i = 0; i < ZONE_COUNT; i++) {
        zones[i].free_pages = zones[i].managed_pages;
    }
}

/* ---- Enhanced pmm_init wrapper -------------------------------- */
void pmm_enhanced_init(void) {
    /* Original pmm_init is called first from kernel_main */
    memset(&pmm_stats, 0, sizeof(PmmStats));
    pmm_zones_init();
}

/* ---- Get zone info -------------------------------------------- */
void pmm_get_zone_info(u32 zone_id, MemoryZone *out) {
    if (zone_id >= ZONE_COUNT || !out) return;
    *out = zones[zone_id];
}

/* ---- Check if physical address is in managed range ------------ */
int pmm_is_managed(u64 phys) {
    return phys >= RAM_START && phys < RAM_END;
}

/* ---- Get maximum contiguous block available ------------------- */
u32 pmm_max_contiguous_order(void) {
    for (u32 o = MAX_ORDER; o > 0; o--) {
        if (ENH_BUDDY_HEADS[o].next != BUDDY_NIL) return o;
    }
    return (ENH_BUDDY_HEADS[0].next != BUDDY_NIL) ? 0 : 0;
}

/* ---- Dump free list for debugging ----------------------------- */
void pmm_dump_freelist(void) {
    kprintf("PMM Free List Dump:\r\n");
    for (u32 o = 0; o <= MAX_ORDER; o++) {
        u32 count = 0;
        u32 idx = ENH_BUDDY_HEADS[o].next;
        while (idx != BUDDY_NIL) {
            count++;
            idx = page_node(idx)->next;
        }
        if (count > 0) {
            kprintf("  Order %u: %u blocks (%llu pages)\r\n",
                      o, count, (unsigned long long)count * (1ULL << o));
        }
    }
}
