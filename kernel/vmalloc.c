/* ================================================================
 *  Systrix OS — kernel/vmalloc.c  (Kernel Virtual Memory Allocator)
 *
 *  Purpose:
 *    Provides a kernel-space virtual memory allocator for large
 *    allocations that don't need to be physically contiguous.
 *    Similar to Linux's vmalloc().
 *
 *  Design:
 *    - Manages a dedicated kernel virtual address range
 *    - Maps physically non-contiguous pages into contiguous VA space
 *    - Uses a bitmap allocator for VA space management
 *    - Supports allocations from PAGE_SIZE up to VMALLOC_MAX_SIZE
 *
 *  Memory Layout:
 *    VMALLOC_BASE = 0xFFFF800000000000 (higher half kernel space)
 *    VMALLOC_SIZE = 256 MB
 *    Each page is individually mapped from PMM into the VA range.
 *
 *  Use Cases:
 *    - Large kernel buffers (network, filesystem caches)
 *    - Module loading (when modules are added)
 *    - Temporary mappings for I/O operations
 *    - Memory-mapped I/O regions
 * ================================================================ */
#include "../include/kernel.h"

/* ---- Configuration -------------------------------------------- */
#define VMALLOC_BASE    0xFFFF800000000000ULL
#define VMALLOC_SIZE    (256ULL * 1024 * 1024)  /* 256 MB */
#define VMALLOC_MAX_ALLOC (VMALLOC_SIZE / 2)     /* max single alloc */
#define VMALLOC_PAGES   (VMALLOC_SIZE / PAGE_SIZE)
#define VMALLOC_BITMAP_SZ ((VMALLOC_PAGES + 7) / 8)

/* ---- VA bitmap: 1 bit per page (0=free, 1=allocated) --------- */
static u8 vmalloc_bitmap[VMALLOC_BITMAP_SZ];
static u64 vmalloc_allocated_pages = 0;
static u64 vmalloc_peak_pages = 0;
static u64 vmalloc_total_allocs = 0;
static u64 vmalloc_total_frees = 0;

/* ---- Allocation tracking for leak detection ------------------- */
#define VMALLOC_MAX_TRACKED 256
typedef struct VmallocEntry {
    u64 virt;
    u64 pages;
    u64 caller;  /* could be extended with caller address */
    char tag[16];
} VmallocEntry;

static VmallocEntry vmalloc_tracking[VMALLOC_MAX_TRACKED];
static u32 vmalloc_track_count = 0;

/* ---- Bitmap helpers ------------------------------------------- */
static inline int vm_btest(u64 page) {
    return (vmalloc_bitmap[page >> 3] >> (page & 7)) & 1;
}
static inline void vm_bset(u64 page) {
    vmalloc_bitmap[page >> 3] |= (u8)(1 << (page & 7));
}
static inline void vm_bclr(u64 page) {
    vmalloc_bitmap[page >> 3] &= ~(u8)(1 << (page & 7));
}

/* ---- Find contiguous free pages (first-fit) ------------------- */
static u64 vm_find_free_pages(u64 count) {
    if (count == 0 || count > VMALLOC_PAGES) return 0;

    u64 run_start = 0;
    u64 run_len = 0;

    for (u64 i = 0; i < VMALLOC_PAGES; i++) {
        if (!vm_btest(i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len >= count) return run_start;
        } else {
            run_len = 0;
        }
    }
    return 0;  /* not enough contiguous space */
}

/* ---- Map physical pages into kernel page tables ---------------
 * Maps 'count' physical pages starting at 'phys' into the kernel
 * virtual address space starting at 'virt'. */
static void vm_map_pages(u64 virt, u64 phys, u64 count) {
    u64 cr3 = read_cr3();
    for (u64 i = 0; i < count; i++) {
        u64 v = virt + i * PAGE_SIZE;
        u64 p = phys + i * PAGE_SIZE;
        vmm_map(cr3, v, p, PTE_PRESENT | PTE_WRITE);
    }
}

/* ---- Unmap pages from kernel virtual space -------------------- */
static void vm_unmap_pages(u64 virt, u64 count) {
    u64 cr3 = read_cr3();
    for (u64 i = 0; i < count; i++) {
        vmm_unmap(cr3, virt + i * PAGE_SIZE);
    }
}

/* ---- Track allocation ----------------------------------------- */
static void vm_track(u64 virt, u64 pages, const char *tag) {
    if (vmalloc_track_count >= VMALLOC_MAX_TRACKED) return;

    VmallocEntry *e = &vmalloc_tracking[vmalloc_track_count++];
    e->virt = virt;
    e->pages = pages;
    e->caller = 0;  /* could capture return address */
    if (tag) {
        usize len = strlen(tag);
        usize copy = (len < 15) ? len : 15;
        memcpy(e->tag, tag, copy);
        e->tag[copy] = '\0';
    } else {
        e->tag[0] = '\0';
    }
}

/* ---- Untrack allocation --------------------------------------- */
static void vm_untrack(u64 virt) {
    for (u32 i = 0; i < vmalloc_track_count; i++) {
        if (vmalloc_tracking[i].virt == virt) {
            /* Swap with last entry and decrement */
            if (i < vmalloc_track_count - 1)
                vmalloc_tracking[i] = vmalloc_tracking[vmalloc_track_count - 1];
            vmalloc_track_count--;
            return;
        }
    }
}

/* ================================================================
 *  PUBLIC API
 * ================================================================ */

/* ---- Initialize vmalloc subsystem ----------------------------- */
void vmalloc_init(void) {
    memset(vmalloc_bitmap, 0, VMALLOC_BITMAP_SZ);
    vmalloc_allocated_pages = 0;
    vmalloc_peak_pages = 0;
    vmalloc_total_allocs = 0;
    vmalloc_total_frees = 0;
    vmalloc_track_count = 0;
}

/* ---- Allocate virtually contiguous memory ---------------------
 * Returns kernel virtual address, or NULL on failure.
 * Physical pages may be non-contiguous. */
void *vmalloc(u64 size, const char *tag) {
    if (size == 0 || size > VMALLOC_MAX_ALLOC) return NULL;

    u64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Find free VA space */
    u64 page_idx = vm_find_free_pages(pages);
    if (page_idx == 0) return NULL;

    u64 virt = VMALLOC_BASE + page_idx * PAGE_SIZE;

    /* Allocate and map physical pages */
    for (u64 i = 0; i < pages; i++) {
        u64 phys = pmm_alloc();
        if (!phys) {
            /* Rollback: unmap already-mapped pages */
            for (u64 j = 0; j < i; j++) {
                vmm_unmap(read_cr3(), virt + j * PAGE_SIZE);
                u64 old_phys = vmm_virt_to_phys(read_cr3(), virt + j * PAGE_SIZE);
                if (old_phys) pmm_free(old_phys);
            }
            return NULL;
        }
        memset((void*)phys, 0, PAGE_SIZE);
        vmm_map(read_cr3(), virt + i * PAGE_SIZE, phys, PTE_PRESENT | PTE_WRITE);
    }

    /* Mark bitmap */
    for (u64 i = 0; i < pages; i++)
        vm_bset(page_idx + i);

    vmalloc_allocated_pages += pages;
    if (vmalloc_allocated_pages > vmalloc_peak_pages)
        vmalloc_peak_pages = vmalloc_allocated_pages;
    vmalloc_total_allocs++;

    vm_track(virt, pages, tag);

    return (void*)virt;
}

/* ---- Free vmalloc'd memory ------------------------------------ */
void vfree(void *addr) {
    if (!addr) return;

    u64 virt = (u64)addr;
    if (virt < VMALLOC_BASE || virt >= VMALLOC_BASE + VMALLOC_SIZE) return;

    u64 page_idx = (virt - VMALLOC_BASE) / PAGE_SIZE;

    /* Count allocated pages starting from page_idx */
    u64 pages = 0;
    while (page_idx + pages < VMALLOC_PAGES && vm_btest(page_idx + pages))
        pages++;

    if (pages == 0) return;  /* not allocated */

    /* Unmap and free physical pages */
    u64 cr3 = read_cr3();
    for (u64 i = 0; i < pages; i++) {
        u64 v = virt + i * PAGE_SIZE;
        u64 phys = vmm_virt_to_phys(cr3, v);
        vmm_unmap(cr3, v);
        if (phys) pmm_free(phys);
        vm_bclr(page_idx + i);
    }

    vmalloc_allocated_pages -= pages;
    vmalloc_total_frees++;

    vm_untrack(virt);
}

/* ---- Allocate and zero ---------------------------------------- */
void *vzalloc(u64 size, const char *tag) {
    void *ptr = vmalloc(size, tag);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

/* ---- Reallocate vmalloc'd memory ------------------------------ */
void *vrealloc(void *old, u64 new_size, const char *tag) {
    if (!old) return vmalloc(new_size, tag);
    if (!new_size) { vfree(old); return NULL; }

    u64 virt = (u64)old;
    if (virt < VMALLOC_BASE || virt >= VMALLOC_BASE + VMALLOC_SIZE) return NULL;

    u64 page_idx = (virt - VMALLOC_BASE) / PAGE_SIZE;

    /* Count current pages */
    u64 old_pages = 0;
    while (page_idx + old_pages < VMALLOC_PAGES && vm_btest(page_idx + old_pages))
        old_pages++;

    u64 new_pages = (new_size + PAGE_SIZE - 1) / PAGE_SIZE;

    if (new_pages <= old_pages) return old;  /* fits in place */

    /* Allocate new, copy, free old */
    void *new_ptr = vmalloc(new_size, tag);
    if (!new_ptr) return NULL;

    u64 old_size = old_pages * PAGE_SIZE;
    memcpy(new_ptr, old, old_size);
    vfree(old);

    return new_ptr;
}

/* ---- Get physical address from vmalloc VA --------------------- */
u64 vmalloc_to_phys(void *addr) {
    u64 virt = (u64)addr;
    if (virt < VMALLOC_BASE || virt >= VMALLOC_BASE + VMALLOC_SIZE) return 0;
    return vmm_virt_to_phys(read_cr3(), virt);
}

/* ---- Print vmalloc statistics --------------------------------- */
void vmalloc_print_stats(void) {
    kprintf("\n=== Vmalloc Statistics ===\r\n"
                "Total size:        256 MB\r\n"
                "Allocated pages:   %llu (%llu KB)\r\n"
                "Peak pages:        %llu\r\n"
                "Total allocations: %llu\r\n"
                "Total frees:       %llu\r\n\r\n"
                "Tracked allocations:\r\n",
                (unsigned long long)vmalloc_allocated_pages,
                (unsigned long long)vmalloc_allocated_pages * 4,
                (unsigned long long)vmalloc_peak_pages,
                (unsigned long long)vmalloc_total_allocs,
                (unsigned long long)vmalloc_total_frees);

    for (u32 i = 0; i < vmalloc_track_count; i++) {
        VmallocEntry *e = &vmalloc_tracking[i];
        kprintf("  %s: %llu pages (%llu KB)\r\n",
                  e->tag,
                  (unsigned long long)e->pages,
                  (unsigned long long)e->pages * 4);
    }
    kprintf("==========================\r\n");
}

/* ---- Dump all tracked allocations (leak detection) ------------ */
void vmalloc_dump_leaks(void) {
    if (vmalloc_track_count == 0) {
        kprintf("[VMALLOC] No tracked allocations.\r\n");
        return;
    }

    kprintf("[VMALLOC] Tracked allocations (potential leaks):\r\n");
    for (u32 i = 0; i < vmalloc_track_count; i++) {
        VmallocEntry *e = &vmalloc_tracking[i];
        kprintf("  VA=%016llx size=%llu tag=%s\r\n",
                  (unsigned long long)e->virt,
                  (unsigned long long)e->pages * PAGE_SIZE,
                  e->tag);
    }
}

/* ---- Memory-mapped I/O region mapping -------------------------
 * Maps a physical MMIO region into kernel virtual space.
 * Unlike vmalloc, this maps existing physical addresses. */
void *vmalloc_map_mmio(u64 phys, u64 size, const char *tag) {
    u64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    u64 page_idx = vm_find_free_pages(pages);
    if (page_idx == 0) return NULL;

    u64 virt = VMALLOC_BASE + page_idx * PAGE_SIZE;

    /* Map MMIO region (no NX for device memory if needed) */
    u64 cr3 = read_cr3();
    for (u64 i = 0; i < pages; i++) {
        vmm_map(cr3, virt + i * PAGE_SIZE, phys + i * PAGE_SIZE,
                PTE_PRESENT | PTE_WRITE);
    }

    /* Mark bitmap */
    for (u64 i = 0; i < pages; i++)
        vm_bset(page_idx + i);

    vmalloc_allocated_pages += pages;
    if (vmalloc_allocated_pages > vmalloc_peak_pages)
        vmalloc_peak_pages = vmalloc_allocated_pages;
    vmalloc_total_allocs++;

    vm_track(virt, pages, tag);

    return (void*)virt;
}

/* ---- Unmap MMIO region ---------------------------------------- */
void vmalloc_unmap_mmio(void *addr) {
    if (!addr) return;

    u64 virt = (u64)addr;
    if (virt < VMALLOC_BASE || virt >= VMALLOC_BASE + VMALLOC_SIZE) return;

    u64 page_idx = (virt - VMALLOC_BASE) / PAGE_SIZE;

    /* Count pages */
    u64 pages = 0;
    while (page_idx + pages < VMALLOC_PAGES && vm_btest(page_idx + pages))
        pages++;

    if (pages == 0) return;

    /* Unmap only (don't free physical pages - they're MMIO) */
    u64 cr3 = read_cr3();
    for (u64 i = 0; i < pages; i++) {
        vmm_unmap(cr3, virt + i * PAGE_SIZE);
        vm_bclr(page_idx + i);
    }

    vmalloc_allocated_pages -= pages;
    vmalloc_total_frees++;

    vm_untrack(virt);
}
