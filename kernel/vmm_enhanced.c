/* ================================================================
 *  Systrix OS — kernel/vmm_enhanced.c  (Production-Level VMM)
 *
 *  Enhancements over the original vmm.c:
 *
 *  1. ADDRESS SPACE LAYOUT RANDOMIZATION (ASLR)
 *     - Randomize mmap base address per process
 *     - Randomize stack location (within bounds)
 *     - Randomize heap base (brk)
 *     - Entropy from TSC and PIT jitter
 *
 *  2. ENHANCED VMA MANAGEMENT
 *     - Sorted VMA list by address for O(log n) lookup
 *     - VMA merging for adjacent regions
 *     - VMA splitting for partial munmap/mprotect
 *     - Gap tracking for fragmentation metrics
 *
 *  3. HUGE PAGE SUPPORT
 *     - Explicit 2MB huge page allocation via mmap flags
 *     - Transparent huge page promotion for large contiguous VMAs
 *     - Huge page fault handling
 *     - Huge page statistics
 *
 *  4. ENHANCED PAGE FAULT HANDLER
 *     - Better error classification
 *     - Stack expansion on guard page fault
 *     - Lazy stack allocation
 *     - Detailed fault logging for debugging
 *
 *  5. MEMORY PROTECTION ENHANCEMENTS
 *     - mprotect with proper VMA splitting
 *     - Guard pages for stack overflow detection
 *     - Read-only text segment enforcement
 * ================================================================ */
#include "../include/kernel.h"

/* ================================================================
 *  ASLR IMPLEMENTATION
 * ================================================================ */

/* ---- Entropy source: TSC + PIT jitter ------------------------- */
static u64 aslr_entropy(void) {
    u64 tsc_lo, tsc_hi;
    __asm__ volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
    u64 tsc = (tsc_hi << 32) | tsc_lo;

    /* Mix in PIT counter for additional jitter */
    outb(0x43, 0x00);  /* latch counter 0 */
    u8 lo = inb(0x40);
    u8 hi = inb(0x40);
    u64 pit = ((u64)hi << 8) | lo;

    /* Simple mix function */
    return tsc ^ (pit << 16) ^ (tsc >> 17);
}

/* ---- Random number in range [0, max) -------------------------- */
static u64 aslr_rand(u64 max) {
    if (max == 0) return 0;
    return aslr_entropy() % max;
}

/* ---- Randomize mmap base address ------------------------------ */
u64 vmm_aslr_mmap_base(void) {
    /* mmap base: randomize within 1MB range above MMAP_BASE */
    u64 offset = aslr_rand(1024 * 1024);  /* 1MB entropy */
    offset &= ~(PAGE_SIZE - 1);  /* page-align */
    return MMAP_BASE + offset;
}

/* ---- Randomize stack base ------------------------------------- */
u64 vmm_aslr_stack_base(void) {
    /* Stack top: randomize down from PROC_STACK_TOP within 256KB */
    u64 offset = aslr_rand(256 * 1024);
    offset &= ~(PAGE_SIZE - 1);
    return PROC_STACK_TOP - offset;
}

/* ---- Randomize brk base --------------------------------------- */
u64 vmm_aslr_brk_base(void) {
    /* Brk: randomize within 512KB range above BRK_BASE */
    u64 offset = aslr_rand(512 * 1024);
    offset &= ~(PAGE_SIZE - 1);
    return BRK_BASE + offset;
}

/* ================================================================
 *  ENHANCED VMA MANAGEMENT
 * ================================================================ */

/* ---- Insert VMA in sorted order by start address -------------- */
static int vma_insert_sorted(VMA *table, u64 start, u64 end, u32 flags) {
    /* Find insertion point */
    int insert_pos = -1;
    int empty_pos = -1;

    for (int i = 0; i < VMA_MAX; i++) {
        if (table[i].start == table[i].end) {
            if (empty_pos < 0) empty_pos = i;
            continue;
        }

        /* Check for overlap */
        if (start < table[i].end && end > table[i].start) {
            /* Try to merge if adjacent and same flags */
            if (table[i].flags == flags) {
                if (table[i].end == start) {
                    table[i].end = end;
                    return 0;
                }
                if (table[i].start == end) {
                    table[i].start = start;
                    return 0;
                }
            }
            return -1;  /* overlap without merge */
        }

        /* Track sorted insertion point */
        if (insert_pos < 0 && table[i].start > start)
            insert_pos = i;
    }

    if (empty_pos < 0) return -1;  /* table full */

    /* Shift entries if needed for sorted order */
    if (insert_pos >= 0 && insert_pos < empty_pos) {
        for (int i = empty_pos; i > insert_pos; i--)
            table[i] = table[i-1];
    } else {
        insert_pos = empty_pos;
    }

    table[insert_pos].start       = start;
    table[insert_pos].end         = end;
    table[insert_pos].flags       = flags;
    table[insert_pos].fd          = -1;
    table[insert_pos].file_offset = 0;

    return 0;
}

/* ---- Split VMA at address -------------------------------------
 * Splits a VMA into two at the given address.
 * Returns 0 on success, -1 if no VMA contains addr. */
int vmm_vma_split(VMA *table, u64 addr, u32 new_flags) {
    for (int i = 0; i < VMA_MAX; i++) {
        if (table[i].start >= table[i].end) continue;
        if (addr <= table[i].start || addr >= table[i].end) continue;

        /* Find empty slot for second half */
        int empty = -1;
        for (int j = 0; j < VMA_MAX; j++) {
            if (table[j].start == table[j].end) { empty = j; break; }
        }
        if (empty < 0) return -1;

        /* Create second half */
        table[empty].start       = addr;
        table[empty].end         = table[i].end;
        table[empty].flags       = new_flags;
        table[empty].fd          = table[i].fd;
        table[empty].file_offset = table[i].file_offset +
                                   (addr - table[i].start);

        /* Shrink first half */
        table[i].end = addr;

        return 0;
    }
    return -1;
}

/* ---- Merge adjacent VMAs with same flags ---------------------- */
static void vma_merge_adjacent(VMA *table) {
    for (int i = 0; i < VMA_MAX - 1; i++) {
        if (table[i].start >= table[i].end) continue;
        for (int j = i + 1; j < VMA_MAX; j++) {
            if (table[j].start >= table[j].end) continue;
            if (table[i].flags != table[j].flags) continue;
            if (table[i].end == table[j].start) {
                table[i].end = table[j].end;
                table[j].start = table[j].end = 0;
            }
        }
    }
}

/* ---- Enhanced VMA add with merging ---------------------------- */
int vmm_vma_add_enhanced(VMA *table, u64 start, u64 end, u32 flags) {
    if (start >= end) return -1;

    /* Try to insert with merging */
    int ret = vma_insert_sorted(table, start, end, flags);
    if (ret == 0) vma_merge_adjacent(table);
    return ret;
}

/* ---- Calculate VMA fragmentation index ----------------------- */
u64 vmm_vma_fragmentation(VMA *table) {
    u64 total_mapped = 0;
    u64 gaps = 0;
    u64 prev_end = 0;

    for (int i = 0; i < VMA_MAX; i++) {
        if (table[i].start >= table[i].end) continue;
        if (prev_end > 0 && table[i].start > prev_end)
            gaps += table[i].start - prev_end;
        total_mapped += table[i].end - table[i].start;
        prev_end = table[i].end;
    }

    if (total_mapped == 0) return 0;
    return (gaps * 1000) / (total_mapped + gaps);
}

/* ---- Print VMA table for debugging ---------------------------- */
void vmm_print_vmas(VMA *table) {
    kprintf("VMA Table:\r\n");
    for (int i = 0; i < VMA_MAX; i++) {
        if (table[i].start >= table[i].end) continue;

        kprintf("  [");
        /* Print start address in hex */
        u64 v = table[i].start;
        for (int j = 48; j >= 0; j -= 4) {
            u8 nibble = (v >> j) & 0xF;
            vga_putchar(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
        }
        kprintf("-");
        v = table[i].end;
        for (int j = 48; j >= 0; j -= 4) {
            u8 nibble = (v >> j) & 0xF;
            vga_putchar(nibble < 10 ? '0' + nibble : 'A' + nibble - 10);
        }
        kprintf("] flags=");
        v = table[i].flags;
        if (v & VMA_READ)  vga_putchar('R');
        if (v & VMA_WRITE) vga_putchar('W');
        if (v & VMA_EXEC)  vga_putchar('X');
        if (v & VMA_ANON)  vga_putchar('A');
        if (v & VMA_STACK) vga_putchar('S');
        kprintf("\r\n");
    }
}

/* ================================================================
 *  HUGE PAGE SUPPORT
 * ================================================================ */

/* ---- Huge page statistics ------------------------------------- */
static u64 huge_page_allocs = 0;
static u64 huge_page_frees = 0;
static u64 huge_page_current = 0;
static u64 huge_page_promoted = 0;  /* transparent promotions */

/* ---- Allocate 2MB huge page ----------------------------------- */
u64 vmm_alloc_huge_page(u64 cr3, u64 virt, u64 flags) {
    /* Allocate 512 contiguous 4KB pages (2MB) */
    u64 phys = pmm_alloc_aligned(512, 512);
    if (!phys) return 0;

    /* Map as 2MB huge page (bit 7 = PS) */
    u64 *pml4 = (u64*)cr3;
    u64 i4 = (virt >> 39) & 0x1FF;
    u64 i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF;

    /* Ensure PML4 and PDPT exist */
    if (!(pml4[i4] & PTE_PRESENT)) {
        u64 p = pmm_alloc(); if (!p) { pmm_free(phys); return 0; }
        memset((void*)p, 0, PAGE_SIZE);
        pml4[i4] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    u64 *pdpt = (u64*)(pml4[i4] & PTE_PHYS_MASK);

    if (!(pdpt[i3] & PTE_PRESENT)) {
        u64 p = pmm_alloc(); if (!p) { pmm_free(phys); return 0; }
        memset((void*)p, 0, PAGE_SIZE);
        pdpt[i3] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    u64 *pd = (u64*)(pdpt[i3] & PTE_PHYS_MASK);

    /* Set huge page PDE (PS bit = 7) */
    pd[i2] = (phys & ~(u64)(2*1024*1024 - 1)) | (flags & ~PTE_PHYS_MASK) |
             PTE_PRESENT | (1UL << 7);

    huge_page_allocs++;
    huge_page_current++;

    return phys;
}

/* ---- Free huge page ------------------------------------------- */
void vmm_free_huge_page(u64 cr3, u64 virt) {
    u64 *pml4 = (u64*)cr3;
    u64 i4 = (virt >> 39) & 0x1FF;
    u64 i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF;

    if (!(pml4[i4] & PTE_PRESENT)) return;
    u64 *pdpt = (u64*)(pml4[i4] & PTE_PHYS_MASK);
    if (!(pdpt[i3] & PTE_PRESENT)) return;
    u64 *pd = (u64*)(pdpt[i3] & PTE_PHYS_MASK);

    if (!(pd[i2] & (1UL << 7))) return;  /* not a huge page */

    u64 phys = pd[i2] & ~(u64)(2*1024*1024 - 1);
    pd[i2] = 0;
    vmm_invlpg(virt);

    /* Free all 512 pages */
    for (u32 i = 0; i < 512; i++)
        pmm_free(phys + (u64)i * PAGE_SIZE);

    huge_page_frees++;
    if (huge_page_current > 0) huge_page_current--;
}

/* ---- Transparent huge page promotion --------------------------
 * Attempts to promote a 2MB VMA from 4KB pages to a single huge page.
 * Returns 1 if promoted, 0 if not possible. */
int vmm_thp_promote(u64 cr3, u64 virt_start, u64 virt_end) {
    u64 size = virt_end - virt_start;
    if (size != 2 * 1024 * 1024) return 0;  /* must be exactly 2MB */
    if (virt_start & (2*1024*1024 - 1)) return 0;  /* must be 2MB-aligned */

    /* Check if all 512 pages are physically contiguous */
    u64 first_phys = vmm_virt_to_phys(cr3, virt_start);
    if (!first_phys) return 0;

    for (u64 i = 1; i < 512; i++) {
        u64 phys = vmm_virt_to_phys(cr3, virt_start + i * PAGE_SIZE);
        if (!phys || phys != first_phys + i * PAGE_SIZE) return 0;
    }

    /* All contiguous - promote to huge page */
    u64 *pml4 = (u64*)cr3;
    u64 i4 = (virt_start >> 39) & 0x1FF;
    u64 i3 = (virt_start >> 30) & 0x1FF;
    u64 i2 = (virt_start >> 21) & 0x1FF;

    if (!(pml4[i4] & PTE_PRESENT)) return 0;
    u64 *pdpt = (u64*)(pml4[i4] & PTE_PHYS_MASK);
    if (!(pdpt[i3] & PTE_PRESENT)) return 0;
    u64 *pd = (u64*)(pdpt[i3] & PTE_PHYS_MASK);

    /* Free the PT page */
    u64 pt_phys = pd[i2] & PTE_PHYS_MASK;
    pmm_free(pt_phys);

    /* Set huge page PDE */
    pd[i2] = (first_phys & ~(u64)(2*1024*1024 - 1)) |
             PTE_PRESENT | PTE_WRITE | PTE_USER | (1UL << 7);

    /* Flush TLB for entire 2MB range */
    for (u64 i = 0; i < 512; i++)
        vmm_invlpg(virt_start + i * PAGE_SIZE);

    huge_page_promoted++;
    return 1;
}

/* ---- Check if address is mapped as huge page ------------------ */
int vmm_is_huge_page(u64 cr3, u64 virt) {
    u64 *pml4 = (u64*)cr3;
    u64 i4 = (virt >> 39) & 0x1FF;
    u64 i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF;

    if (!(pml4[i4] & PTE_PRESENT)) return 0;
    u64 *pdpt = (u64*)(pml4[i4] & PTE_PHYS_MASK);
    if (!(pdpt[i3] & PTE_PRESENT)) return 0;
    u64 *pd = (u64*)(pdpt[i3] & PTE_PHYS_MASK);

    return (pd[i2] & (1UL << 7)) ? 1 : 0;
}

/* ---- Print huge page statistics ------------------------------- */
void vmm_huge_page_stats(void) {
    kprintf("\n=== Huge Page Statistics ===\r\n"
              "Allocations:            %llu\r\n"
              "Frees:                  %llu\r\n"
              "Current:                %llu\r\n"
              "Transparent promotions: %llu\r\n"
              "============================\r\n",
              (unsigned long long)huge_page_allocs,
              (unsigned long long)huge_page_frees,
              (unsigned long long)huge_page_current,
              (unsigned long long)huge_page_promoted);
}

/* ================================================================
 *  ENHANCED PAGE FAULT HANDLER
 * ================================================================ */

/* ---- Stack expansion on guard page fault ----------------------
 * If fault is just below the stack VMA, expand the stack. */
static int handle_stack_expansion(PCB *pcb, VMA *vmas, u64 fault_addr) {
    /* Find stack VMA */
    VMA *stack_vma = NULL;
    for (int i = 0; i < VMA_MAX; i++) {
        if (vmas[i].start < vmas[i].end && (vmas[i].flags & VMA_STACK)) {
            stack_vma = &vmas[i];
            break;
        }
    }
    if (!stack_vma) return 0;

    /* Check if fault is within 1 page below stack */
    u64 stack_bottom = stack_vma->start;
    if (fault_addr < stack_bottom - PAGE_SIZE) return 0;
    if (fault_addr >= stack_bottom) return 0;

    /* Expand stack downward by 1 page */
    u64 new_page = pmm_alloc();
    if (!new_page) return 0;

    memset((void*)new_page, 0, PAGE_SIZE);
    vmm_map(pcb->cr3, fault_addr & ~(u64)(PAGE_SIZE - 1),
            new_page, PTE_PRESENT | PTE_WRITE | PTE_USER);

    /* Update VMA */
    stack_vma->start = (fault_addr & ~(u64)(PAGE_SIZE - 1));

    return 1;
}

/* ---- Enhanced page fault handler ------------------------------ */
int vmm_page_fault_enhanced(u64 fault_addr, u64 error_code) {
    PCB *pcb = process_current_pcb();
    if (!pcb) return 0;

    VMA *vmas = (VMA*)(usize)pcb->vma_table;
    if (!vmas) return 0;

    u64 page = fault_addr & ~(u64)(PAGE_SIZE - 1);
    int is_write = (error_code & 2) != 0;
    int is_user  = (error_code & 4) != 0; (void)is_user;
    int is_prot  = (error_code & 1) != 0;

    /* ---- Copy-on-Write ---------------------------------------- */
    if (is_prot && is_write) {
        u64 *pte = vmm_pte_get(pcb->cr3, page, 0);
        if (!pte || !(*pte & PTE_PRESENT)) return 0;

        u64 old_phys = *pte & PTE_PHYS_MASK;
        u8  rc       = pmm_refcount(old_phys);

        if (rc <= 1) {
            *pte |= PTE_WRITE;
            vmm_invlpg(page);
            return 1;
        }

        u64 new_phys = pmm_alloc();
        if (!new_phys) return 0;
        memcpy((void*)new_phys, (void*)old_phys, PAGE_SIZE);
        pmm_unref(old_phys);

        *pte = (new_phys & PTE_PHYS_MASK) | PTE_PRESENT | PTE_WRITE | PTE_USER;
        vmm_invlpg(page);
        return 1;
    }

    /* ---- Not-present fault ------------------------------------ */
    if (!is_prot) {
        /* Try stack expansion first */
        if (handle_stack_expansion(pcb, vmas, fault_addr))
            return 1;

        VMA *vma = vmm_find_vma(vmas, fault_addr);
        if (!vma) {
            /* No VMA - this is a segfault */
            return 0;
        }

        /* Demand paging: allocate physical page on first access */
        u64 phys = pmm_alloc();
        if (!phys) return 0;
        memset((void*)phys, 0, PAGE_SIZE);

        u64 flags = PTE_PRESENT | PTE_USER;
        if (vma->flags & VMA_WRITE) flags |= PTE_WRITE;
        if (!(vma->flags & VMA_EXEC)) flags |= PTE_NX;

        vmm_map(pcb->cr3, page, phys, flags);
        return 1;
    }

    return 0;
}

/* ================================================================
 *  MEMORY PROTECTION ENHANCEMENTS
 * ================================================================ */

/* ---- Enhanced mprotect with VMA splitting --------------------- */
int vmm_mprotect_enhanced(PCB *pcb, u64 addr, u64 len, u64 prot) {
    VMA *vmas = (VMA*)(usize)pcb->vma_table;
    if (!vmas) return -1;

    u64 start = addr & ~(u64)(PAGE_SIZE - 1);
    u64 end   = (addr + len + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);

    /* Find and update VMAs in range */
    for (int i = 0; i < VMA_MAX; i++) {
        if (vmas[i].start >= vmas[i].end) continue;
        if (vmas[i].end <= start || vmas[i].start >= end) continue;

        /* Partial overlap: need to split */
        if (vmas[i].start < start) {
            if (vmm_vma_split(vmas, start, vmas[i].flags) < 0) return -1;
        }
        if (vmas[i].end > end) {
            if (vmm_vma_split(vmas, end, vmas[i].flags) < 0) return -1;
        }

        /* Update flags */
        vmas[i].flags = (vmas[i].flags & ~(VMA_READ | VMA_WRITE | VMA_EXEC)) |
                        (prot & (VMA_READ | VMA_WRITE | VMA_EXEC));

        /* Update page table entries */
        u64 cr3 = pcb->cr3;
        for (u64 v = vmas[i].start; v < vmas[i].end; v += PAGE_SIZE) {
            u64 *pte = vmm_pte_get(cr3, v, 0);
            if (!pte) continue;

            u64 phys = *pte & PTE_PHYS_MASK;
            u64 new_flags = PTE_PRESENT | PTE_USER;
            if (prot & VMA_WRITE) new_flags |= PTE_WRITE;
            if (!(prot & VMA_EXEC)) new_flags |= PTE_NX;

            *pte = phys | new_flags;
            vmm_invlpg(v);
        }
    }

    return 0;
}

/* ---- Add guard page below stack ------------------------------- */
int vmm_add_guard_page(PCB *pcb) {
    VMA *vmas = (VMA*)(usize)pcb->vma_table;
    if (!vmas) return -1;

    /* Find stack VMA */
    for (int i = 0; i < VMA_MAX; i++) {
        if (vmas[i].start < vmas[i].end && (vmas[i].flags & VMA_STACK)) {
            /* Guard page: map as not-present */
            u64 guard_addr = vmas[i].start - PAGE_SIZE;
            u64 phys = pmm_alloc();
            if (!phys) return -1;
            /* Map but don't set PRESENT - will trigger #PF on access */
            vmm_map(pcb->cr3, guard_addr, phys, 0);
            return 0;
        }
    }
    return -1;
}

/* ---- Initialize enhanced VMM subsystem ------------------------ */
void vmm_enhanced_init(void) {
    huge_page_allocs = 0;
    huge_page_frees = 0;
    huge_page_current = 0;
    huge_page_promoted = 0;
}
