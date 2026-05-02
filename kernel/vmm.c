/* ================================================================
 *  Systrix OS — kernel/vmm.c  (v2: VMA tracking + Demand Paging + CoW)
 *
 *  Builds on the page-table walker from v1, adding:
 *
 *  1. VMA (Virtual Memory Area) tracking
 *     Each process owns a list of VMAs stored in the PCB padding.
 *     A VMA records [start, end, flags, type] similar to Linux's
 *     vm_area_struct.  sys_mmap registers a VMA; sys_munmap tears
 *     it down.  vmm_find_vma() is used by the page-fault handler.
 *
 *  2. Demand Paging
 *     mmap() no longer eagerly allocates physical pages.  Instead
 *     it just registers the VMA.  On first access, the CPU raises
 *     a #PF.  The fault handler (vmm_page_fault) allocates a page
 *     and maps it, then returns.  This is how Linux handles
 *     anonymous mappings and the heap.
 *
 *  3. Copy-on-Write (CoW)
 *     vmm_cow_fork() duplicates an address space cheaply:
 *       - Walk the parent's page tables.
 *       - For each writable user PTE, mark it read-only and bump
 *         the physical page's reference count.
 *       - Copy the PML4 structure so the child gets independent
 *         page-table pages.
 *     On write, the CPU raises #PF (protection fault).  The handler
 *     detects refcount > 1, allocates a private copy, remaps it
 *     writable, and decrements the old page's refcount.
 *
 *  4. TLB shootdown (single-CPU stub)
 *     vmm_invlpg() flushes one page from the TLB.  On SMP this
 *     would be an IPI; for now it's a single invlpg instruction.
 * ================================================================ */
#include "../include/kernel.h"

/* VMA table block: pointed to by PCB->vma_table (stored in pad area).
 * We read/write it via process_get_vmas() / process_set_vmas(). */

/* ================================================================
 *  PAGE TABLE WALKER  (unchanged from v1)
 * ================================================================ */

u64 kernel_cr3 = 0;

void vmm_init_kernel(void) { kernel_cr3 = read_cr3(); }

static u64 *pte_get(u64 cr3, u64 virt, int alloc) {
    u64 *pml4 = (u64*)cr3;
    u64 i4 = (virt >> 39) & 0x1FF;
    u64 i3 = (virt >> 30) & 0x1FF;
    u64 i2 = (virt >> 21) & 0x1FF;
    u64 i1 = (virt >> 12) & 0x1FF;

    if (!(pml4[i4] & PTE_PRESENT)) {
        if (!alloc) return NULL;
        u64 p = pmm_alloc(); if (!p) return NULL;
        memset((void*)p, 0, PAGE_SIZE);
        pml4[i4] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    u64 *pdpt = (u64*)(pml4[i4] & PTE_PHYS_MASK);

    if (!(pdpt[i3] & PTE_PRESENT)) {
        if (!alloc) return NULL;
        u64 p = pmm_alloc(); if (!p) return NULL;
        memset((void*)p, 0, PAGE_SIZE);
        pdpt[i3] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    u64 *pd = (u64*)(pdpt[i3] & PTE_PHYS_MASK);

    /* Split 2 MB huge page if needed */
    if (pd[i2] & (1UL << 7)) {
        if (!alloc) return NULL;
        u64 huge_phys  = pd[i2] & (PTE_PHYS_MASK & ~((u64)0x1FFFFF));
        u64 huge_flags = (pd[i2] & 0xFFF) | (pd[i2] & (1UL << 63));
        huge_flags &= ~(1UL << 7); /* clear huge bit */
        u64 pt_phys = pmm_alloc(); if (!pt_phys) return NULL;
        u64 *new_pt = (u64*)pt_phys;
        for (int k = 0; k < 512; k++)
            new_pt[k] = (huge_phys + (u64)k * PAGE_SIZE) | huge_flags;
        pd[i2] = pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
        return &new_pt[i1];
    }
    if (!(pd[i2] & PTE_PRESENT)) {
        if (!alloc) return NULL;
        u64 p = pmm_alloc(); if (!p) return NULL;
        memset((void*)p, 0, PAGE_SIZE);
        pd[i2] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    u64 *pt = (u64*)(pd[i2] & PTE_PHYS_MASK);
    return &pt[i1];
}

/* ================================================================
 *  BASIC VMM OPS
 * ================================================================ */

u64 vmm_create_space(void) {
    u64 phys = pmm_alloc();
    if (!phys) return 0;
    memset((void*)phys, 0, PAGE_SIZE);
    return phys;
}

void vmm_map(u64 cr3, u64 virt, u64 phys, u64 flags) {
    u64 *pte = pte_get(cr3, virt, 1);
    if (pte) *pte = (phys & PTE_PHYS_MASK) | flags | PTE_PRESENT;
}

void vmm_unmap(u64 cr3, u64 virt) {
    u64 *pte = pte_get(cr3, virt, 0);
    if (pte) { *pte = 0; vmm_invlpg(virt); }
}

void vmm_switch(u64 cr3) { write_cr3(cr3); }

void vmm_invlpg(u64 virt) {
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

/* Look up the physical address mapped at virt in cr3.
 * Returns 0 if not mapped. */
u64 vmm_virt_to_phys(u64 cr3, u64 virt) {
    u64 *pte = pte_get(cr3, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return (*pte & PTE_PHYS_MASK) | (virt & (PAGE_SIZE - 1));
}

/* ================================================================
 *  KERNEL MAP (private per-process copy, unchanged from v1)
 * ================================================================ */

void vmm_map_kernel(u64 cr3) {
    u64 *new_pml4  = (u64*)cr3;
    u64 *kern_pml4 = (u64*)kernel_cr3;
    if (!(kern_pml4[0] & PTE_PRESENT)) return;

    u64 new_pdpt_phys = pmm_alloc();
    if (!new_pdpt_phys) return;
    memset((void*)new_pdpt_phys, 0, PAGE_SIZE);
    u64 *new_pdpt  = (u64*)new_pdpt_phys;
    u64 *kern_pdpt = (u64*)(kern_pml4[0] & PTE_PHYS_MASK);

    for (int i3 = 0; i3 < 512; i3++) {
        if (!(kern_pdpt[i3] & PTE_PRESENT)) continue;
        if (kern_pdpt[i3] & (1UL<<7)) { new_pdpt[i3] = kern_pdpt[i3]; continue; }
        u64 *kern_pd = (u64*)(kern_pdpt[i3] & PTE_PHYS_MASK);
        u64 new_pd_phys = pmm_alloc(); if (!new_pd_phys) continue;
        memcpy((void*)new_pd_phys, kern_pd, PAGE_SIZE);
        u64 flags = (kern_pdpt[i3] & 0xFFF) | (kern_pdpt[i3] & (1UL << 63));
        new_pdpt[i3] = new_pd_phys | flags | PTE_USER;
    }
    u64 flags0 = (kern_pml4[0] & 0xFFF) | (kern_pml4[0] & (1UL << 63));
    new_pml4[0] = new_pdpt_phys | flags0 | PTE_USER;
}

/* ================================================================
 *  vmm_destroy — free all page-table pages for cr3
 * ================================================================ */

void vmm_destroy(u64 cr3) {
    if (cr3 < RAM_START || cr3 >= ram_end_actual || (cr3 & (PAGE_SIZE-1))) return;
    u64 *pml4 = (u64*)cr3;
    for (int i4 = 0; i4 < 512; i4++) {
        watchdog_pet();
        if (!(pml4[i4] & PTE_PRESENT)) continue;
        u64 pdpt_phys = pml4[i4] & PTE_PHYS_MASK;
        u64 *pdpt = (u64*)pdpt_phys;
        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            if (pdpt[i3] & (1UL<<7)) continue;
            u64 pd_phys = pdpt[i3] & PTE_PHYS_MASK;
            u64 *pd = (u64*)pd_phys;
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT)) continue;
                if (pd[i2] & (1UL<<7)) continue;
                u64 pt_phys = pd[i2] & PTE_PHYS_MASK;
                u64 *pt = (u64*)pt_phys;
                /* Unref user pages */
                for (int i1 = 0; i1 < 512; i1++) {
                    if ((pt[i1] & PTE_PRESENT) && (pt[i1] & PTE_USER)) {
                        u64 phys_unref = pt[i1] & PTE_PHYS_MASK;
                        if (phys_unref >= RAM_START && phys_unref < ram_end_actual)
                            pmm_unref(phys_unref);
                    }
                }
                if (pt_phys >= RAM_START) pmm_free(pt_phys);
            }
            if (pd_phys >= RAM_START) pmm_free(pd_phys);
        }
        if (pdpt_phys >= RAM_START) pmm_free(pdpt_phys);
    }
    if (cr3 >= RAM_START) pmm_free(cr3);
}

/* ================================================================
 *  VMA MANAGEMENT
 * ================================================================ */

/* Allocate a VMA table for a new process */
VMA *vmm_vma_alloc(void) {
    VMA *t = (VMA*)heap_malloc(VMA_MAX * sizeof(VMA));
    if (t) memset(t, 0, VMA_MAX * sizeof(VMA));
    return t;
}

void vmm_vma_free(VMA *table) { heap_free(table); }

/* Add a VMA to a process's table.  Returns 0 on success. */
int vmm_vma_add(VMA *table, u64 start, u64 end, u32 flags) {
    for (int i = 0; i < VMA_MAX; i++) {
        if (table[i].start == table[i].end) {  /* empty slot */
            table[i].start       = start;
            table[i].end         = end;
            table[i].flags       = flags;
            table[i].fd          = -1;
            table[i].file_offset = 0;
            return 0;
        }
    }
    return -1; /* too many VMAs */
}

/* Remove VMAs that overlap [addr, addr+len) */
void vmm_vma_remove(VMA *table, u64 addr, u64 len) {
    u64 end = addr + len;
    for (int i = 0; i < VMA_MAX; i++) {
        if (table[i].start >= table[i].end) continue;
        if (table[i].end <= addr || table[i].start >= end) continue;
        /* Partial or full overlap — zero out (simplification: full removal) */
        table[i].start = table[i].end = 0;
    }
}

/* Find the VMA that contains 'addr' */
VMA *vmm_find_vma(VMA *table, u64 addr) {
    for (int i = 0; i < VMA_MAX; i++) {
        if (table[i].start < table[i].end &&
            addr >= table[i].start && addr < table[i].end)
            return &table[i];
    }
    return NULL;
}

/* ================================================================
 *  DEMAND PAGING PAGE-FAULT HANDLER
 *
 *  Called from the #PF ISR (vector 14) with:
 *    fault_addr = CR2 (the linear address that caused the fault)
 *    error_code = pushed error code:
 *      bit 0: P  (0=not-present, 1=protection)
 *      bit 1: W  (0=read, 1=write)
 *      bit 2: U  (0=supervisor, 1=user)
 *
 *  Returns 1 if the fault was handled, 0 if it should panic.
 * ================================================================ */

int vmm_page_fault(u64 fault_addr, u64 error_code) {
    PCB *pcb = process_current_pcb();
    if (!pcb) return 0;

    VMA *vmas = (VMA*)(usize)pcb->vma_table;
    if (!vmas) return 0;

    u64 page = fault_addr & ~(u64)(PAGE_SIZE - 1);
    int is_write = (error_code & 2) != 0;
    int is_prot  = (error_code & 1) != 0;  /* present but protection fault */

    /* ---- Copy-on-Write ---------------------------------------- */
    if (is_prot && is_write) {
        u64 *pte = pte_get(pcb->cr3, page, 0);
        if (!pte || !(*pte & PTE_PRESENT)) return 0;

        u64 old_phys = *pte & PTE_PHYS_MASK;
        u8  rc       = pmm_refcount(old_phys);

        if (rc <= 1) {
            /* Sole owner — just restore write permission */
            *pte |= PTE_WRITE;
            vmm_invlpg(page);
            return 1;
        }

        /* Multiple owners — allocate a private copy */
        u64 new_phys = pmm_alloc();
        if (!new_phys) return 0;  /* OOM */
        memcpy((void*)new_phys, (void*)old_phys, PAGE_SIZE);
        pmm_unref(old_phys);      /* drop share of old page */

        *pte = (new_phys & PTE_PHYS_MASK) | PTE_PRESENT | PTE_WRITE | PTE_USER;
        vmm_invlpg(page);
        return 1;
    }

    /* ---- Demand paging (not-present fault) -------------------- */
    if (!is_prot) {
        VMA *vma = vmm_find_vma(vmas, fault_addr);
        if (!vma) return 0;  /* not in any VMA -> SIGSEGV */

        /* Allocate and zero a new physical page */
        u64 phys = pmm_alloc();
        if (!phys) return 0;   /* OOM */
        memset((void*)phys, 0, PAGE_SIZE);

        /* File-backed: read file data into the page */
        if ((vma->flags & VMA_FILE) && vma->fd >= 0) {
            u64 page_index  = (page - vma->start) / PAGE_SIZE;
            u64 file_off    = vma->file_offset + page_index * PAGE_SIZE;
            /* Save/restore fd position around our read */
            extern FD fd_table[];
            FD *f = &fd_table[vma->fd];
            if (f->in_use && file_off < f->size) {
                u32 old_pos = f->pos;
                vfs_seek(vma->fd, (i64)file_off, 0);
                usize to_read = PAGE_SIZE;
                if (file_off + to_read > f->size)
                    to_read = f->size - (u32)file_off;
                vfs_read(vma->fd, (void*)phys, to_read);
                f->pos = old_pos;  /* restore position */
            }
        }

        u64 flags = PTE_PRESENT | PTE_USER;
        if (vma->flags & VMA_WRITE) flags |= PTE_WRITE;

        vmm_map(pcb->cr3, page, phys, flags);
        return 1;
    }

    return 0;
}

/* ================================================================
 *  COPY-ON-WRITE FORK
 *
 *  vmm_cow_fork(parent_cr3, child_cr3):
 *    For every writable user PTE in the parent:
 *      1. Mark it read-only (CoW).
 *      2. Copy the PTE to the child with the same read-only flag.
 *      3. Increment the physical page's refcount.
 *    Kernel mappings are handled by vmm_map_kernel().
 * ================================================================ */

void vmm_cow_fork(u64 parent_cr3, u64 child_cr3) {
    vmm_map_kernel(child_cr3);

    u64 *ppml4 = (u64*)parent_cr3;
    u64 *cpml4 = (u64*)child_cr3;

    /* Walk PML4 entries 1..511 (entry 0 = kernel, handled above) */
    for (int i4 = 1; i4 < 512; i4++) {
        if (!(ppml4[i4] & PTE_PRESENT)) continue;

        u64 *ppdpt = (u64*)(ppml4[i4] & PTE_PHYS_MASK);

        /* Allocate child PDPT */
        u64 cpdpt_phys = pmm_alloc(); if (!cpdpt_phys) continue;
        memset((void*)cpdpt_phys, 0, PAGE_SIZE);
        u64 *cpdpt = (u64*)cpdpt_phys;
        cpml4[i4] = cpdpt_phys | (ppml4[i4] & ~PTE_PHYS_MASK);

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(ppdpt[i3] & PTE_PRESENT)) continue;
            if (ppdpt[i3] & (1UL<<7)) { cpdpt[i3] = ppdpt[i3]; continue; }

            u64 *ppd = (u64*)(ppdpt[i3] & PTE_PHYS_MASK);

            u64 cpd_phys = pmm_alloc(); if (!cpd_phys) continue;
            memset((void*)cpd_phys, 0, PAGE_SIZE);
            u64 *cpd = (u64*)cpd_phys;
            cpdpt[i3] = cpd_phys | (ppdpt[i3] & ~PTE_PHYS_MASK);

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(ppd[i2] & PTE_PRESENT)) continue;
                if (ppd[i2] & (1UL<<7)) { cpd[i2] = ppd[i2]; continue; }

                u64 *ppt = (u64*)(ppd[i2] & PTE_PHYS_MASK);

                u64 cpt_phys = pmm_alloc(); if (!cpt_phys) continue;
                memset((void*)cpt_phys, 0, PAGE_SIZE);
                u64 *cpt = (u64*)cpt_phys;
                cpd[i2] = cpt_phys | (ppd[i2] & ~PTE_PHYS_MASK);

                for (int i1 = 0; i1 < 512; i1++) {
                    if (!(ppt[i1] & PTE_PRESENT)) continue;
                    if (!(ppt[i1] & PTE_USER))    { cpt[i1] = ppt[i1]; continue; }

                    u64 phys = ppt[i1] & PTE_PHYS_MASK;

                    /* Mark parent PTE read-only for CoW */
                    if (ppt[i1] & PTE_WRITE) {
                        ppt[i1] &= ~(u64)PTE_WRITE;
                        vmm_invlpg(/* reconstruct virt */
                            ((u64)i4 << 39) | ((u64)i3 << 30) |
                            ((u64)i2 << 21) | ((u64)i1 << 12));
                    }

                    /* Child gets same read-only mapping */
                    cpt[i1] = (ppt[i1] & ~(u64)PTE_WRITE);

                    /* Both parent and child now share the page */
                    pmm_ref(phys);
                }
            }
        }
    }
}

/* ================================================================
 *  OOM KILLER
 *
 *  Called when pmm_alloc() returns 0.  Finds the process with the
 *  largest RSS (resident set size) and kills it, freeing its pages.
 *  Like Linux's oom_kill_process(), but simpler.
 * ================================================================ */

void vmm_oom_kill(void) {
    kprintf("[OOM] Out of memory -- killing largest process\r\n");

    PCB *victim   = NULL;
    u32  max_rss  = 0;

    u8 *p = (u8*)PROC_TABLE;
    for (int i = 0; i < PROC_MAX; i++, p += PROC_PCB_SIZE) {
        PCB *t = (PCB*)p;
        if (t->state == PSTATE_EMPTY || t->state == PSTATE_DEAD) continue;

        /* Count resident pages in this process's address space */
        u32 rss = 0;
        u64 *pml4 = (u64*)t->cr3;
        for (int i4 = 1; i4 < 512; i4++) {
            if (!(pml4[i4] & PTE_PRESENT)) continue;
            u64 *pdpt = (u64*)(pml4[i4] & PTE_PHYS_MASK);
            for (int i3 = 0; i3 < 512; i3++) {
                if (!(pdpt[i3] & PTE_PRESENT) || (pdpt[i3] & (1UL<<7))) continue;
                u64 *pd = (u64*)(pdpt[i3] & PTE_PHYS_MASK);
                for (int i2 = 0; i2 < 512; i2++) {
                    if (!(pd[i2] & PTE_PRESENT) || (pd[i2] & (1UL<<7))) continue;
                    u64 *pt = (u64*)(pd[i2] & PTE_PHYS_MASK);
                    for (int i1 = 0; i1 < 512; i1++)
                        if ((pt[i1] & PTE_PRESENT) && (pt[i1] & PTE_USER)) rss++;
                }
            }
        }

        if (rss > max_rss) { max_rss = rss; victim = t; }
    }

    if (!victim) { kprintf("[OOM] No victim found -- halting\r\n"); for(;;); }

    kprintf("[OOM] Killing PID ");
    vga_putchar((u8)('0' + victim->pid % 10));
    kprintf(" ("); kprintf("%s", victim->name); kprintf(")\r\n");

    vmm_destroy(victim->cr3);
    victim->state = PSTATE_DEAD;
}

/* Public wrapper so syscall.c can walk PTEs without exposing internals */
u64 *vmm_pte_get(u64 cr3, u64 virt, int alloc) {
    return pte_get(cr3, virt, alloc);
}
